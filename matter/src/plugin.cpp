/* ucxmatter: the Matter node of u-connectXpressApp, loaded by the app's matter.* commands (app/src/ucxapp_matter.h).
 * The node reaches the module through the app's AT client and sees a copy of every URC, so it never owns the UART.
 *
 * This file is the module side: the plug-in entry, the commands and Bluetooth LE advertising. What needs the Matter
 * SDK sits in its own files (onboarding.cpp), see the note in onboarding.h. */
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "endpoints.h"
#include "onboarding.h"
#include "ucxm_host.h"

extern "C" {
#include "u_cx_bluetooth.h"
#include "u_cx_general.h"
#include "u_cx_system.h"
}

#ifdef _WIN32
#define UCXM_EXPORT __declspec(dllexport)
#else
#define UCXM_EXPORT __attribute__((visibility("default")))
#endif

#ifndef UCXM_SDK_VERSION
#define UCXM_SDK_VERSION "unknown"
#endif

namespace {

using namespace ucxm;

// Identity and passcode of the Matter SDK's examples: accepted by every commissioner as a test device.
constexpr uint16_t kVendorId  = 0xFFF1;
constexpr uint16_t kProductId = 0x8000;
constexpr uint32_t kPasscode  = 20202021;

const ucxapp_matter_host_t * gHost;
uCxHandle_t * gUcx;
Onboarding gOnboarding;
char gModel[32];
const char * gNetwork;
bool gAdvertising;
std::atomic<int> gBleLink{ -1 };

void Print(const char * format, ...) __attribute__((format(gnu_printf, 1, 2)));
void Log(int level, const char * format, ...) __attribute__((format(gnu_printf, 2, 3)));
void Event(const char * format, ...) __attribute__((format(gnu_printf, 1, 2)));

void Print(const char * format, ...)
{
    char text[256];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gHost->print(text);
}

void Log(int level, const char * format, ...)
{
    char line[200];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gHost->log(level, line);
}

void Event(const char * format, ...)
{
    char line[120];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gHost->event(line);
}

// On the AT client's thread (ucxapp_matter.h): note it and return
void OnBleUp(struct uCxHandle *, int32_t link, uBtLeAddress_t *)
{
    gBleLink = static_cast<int>(link);
    Event("matter: Bluetooth LE link %d up", static_cast<int>(link));
}

void OnBleDown(struct uCxHandle *, int32_t link)
{
    int mine = static_cast<int>(link);

    if (gBleLink.compare_exchange_strong(mine, -1))
    {
        Event("matter: Bluetooth LE link %d down", static_cast<int>(link));
    }
}

bool BuildEndpoints(int argc, const char * const argv[])
{
    EndpointsClear();
    for (int i = 0; i < (argc > 0 ? argc : KindCount()); i++)
    {
        const EndpointKind * kind = argc > 0 ? KindFind(argv[i]) : &KindAt(i);

        if (kind == nullptr)
        {
            Print("ERROR: unknown endpoint %s; this plug-in builds:", argv[i]);
            for (int k = 0; k < KindCount(); k++)
            {
                Print(" %s", KindAt(k).name);
            }
            Print("\n");
            return false;
        }
        if (EndpointAdd(*kind) == nullptr)
        {
            Print("ERROR: a node takes at most %d endpoints\n", kMaxEndpoints);
            return false;
        }
    }
    return true;
}

// CHIPoBLE advertisement: the Matter service UUID and its service data (discriminator, vendor, product).
// The module adds the flags itself.
bool Advertise()
{
    const uint8_t data[] = {
        0x03, 0x03, 0xF6, 0xFF,
        0x0B, 0x16, 0xF6, 0xFF,
        0x00, // opcode: commissionable
        static_cast<uint8_t>(gOnboarding.discriminator & 0xFF),
        static_cast<uint8_t>((gOnboarding.discriminator >> 8) & 0x0F), // high nibble: advertisement version 0
        static_cast<uint8_t>(kVendorId & 0xFF),
        static_cast<uint8_t>(kVendorId >> 8),
        static_cast<uint8_t>(kProductId & 0xFF),
        static_cast<uint8_t>(kProductId >> 8),
        0x00, // no additional data
    };
    int32_t result;

    // An advertisement the module still runs (bt.adv, a profile) would keep its own data; no error if there is none
    (void) uCxBluetoothLegacyAdvertisementStop(gUcx);
    result = uCxBluetoothSetLegacyAdvertiseDataAndType1(gUcx, data, static_cast<int32_t>(sizeof(data)));
    if (result == 0)
    {
        result = uCxBluetoothLegacyAdvertisementStart(gUcx);
    }
    if (result != 0)
    {
        Print("ERROR: the module does not advertise the Matter service (%d)\n", static_cast<int>(result));
        return false;
    }
    gAdvertising = true;
    return true;
}

void ShowCodes()
{
    const char * m = gOnboarding.manual;

    gHost->qr(gOnboarding.qr);
    if (strlen(m) == 11)
    {
        Print("manual code: %.4s-%.3s-%.4s\n", m, m + 4, m + 7);
    }
    else
    {
        Print("manual code: %s\n", m);
    }
    Print("QR payload: %s\n", gOnboarding.qr);
}

int Start(const ucxapp_matter_host_t * host, int argc, const char * const argv[])
{
    const char * model = nullptr;
    uMacAddress_t address;
    uBtMode_t mode;

    if (host == nullptr || host->abi != UCXAPP_MATTER_ABI || host->size < sizeof(ucxapp_matter_host_t))
    {
        return UCXAPP_MATTER_ERR_FAILED;
    }
    gHost        = host;
    gAdvertising = false;
    gBleLink     = -1;
    if (!BuildEndpoints(argc, argv))
    {
        return UCXAPP_MATTER_ERR_USAGE;
    }
    gNetwork = (host->radios & UCXAPP_MATTER_RADIO_WIFI) != 0 ? "Wi-Fi"
        : (host->radios & UCXAPP_MATTER_RADIO_THREAD) != 0 ? "Thread" : nullptr;
    if (gNetwork == nullptr)
    {
        Print("ERROR: this module has neither Wi-Fi nor Thread; a Matter node needs one of them\n");
        return UCXAPP_MATTER_ERR_FAILED;
    }

    gUcx = ucxm_host_open(host);
    uCxBluetoothRegisterConnect(gUcx, OnBleUp);
    uCxBluetoothRegisterDisconnect(gUcx, OnBleDown);

    snprintf(gModel, sizeof(gModel), "module");
    if (uCxGeneralGetDeviceModelIdentificationBegin(gUcx, &model) && model != nullptr)
    {
        snprintf(gModel, sizeof(gModel), "%s", model);
    }
    if (uCxEnd(gUcx) != 0 || uCxSystemGetLocalAddress(gUcx, U_INTERFACE_ID_BLUETOOTH, &address) != 0)
    {
        Print("ERROR: the module does not give its model and Bluetooth address\n");
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (uCxBluetoothGetMode(gUcx, &mode) != 0 || (mode != U_BT_MODE_PERIPHERAL && mode != U_BT_MODE_CENTRAL_PERIPHERAL))
    {
        Print("ERROR: Bluetooth LE on the module has no peripheral role, which commissioning needs;\n"
              "       set it with: at AT+UBTM=3, at AT&W, dev.reboot\n");
        return UCXAPP_MATTER_ERR_FAILED;
    }

    // The same module always gets the same discriminator: the low 12 bits of its Bluetooth address
    gOnboarding               = Onboarding();
    gOnboarding.vendorId      = kVendorId;
    gOnboarding.productId     = kProductId;
    gOnboarding.passcode      = kPasscode;
    gOnboarding.discriminator = static_cast<uint16_t>(((address.address[4] << 8) | address.address[5]) & 0x0FFF);
    if (!OnboardingCodes(gOnboarding))
    {
        Print("ERROR: the Matter SDK made no setup codes\n");
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (!Advertise())
    {
        return UCXAPP_MATTER_ERR_FAILED;
    }
    Log(UCXAPP_MATTER_LOG_INFO, "%s over %s, %d endpoint(s), discriminator %u", gModel, gNetwork, EndpointCount(),
        static_cast<unsigned>(gOnboarding.discriminator));
    Event("matter: advertising over Bluetooth LE, discriminator %u", static_cast<unsigned>(gOnboarding.discriminator));
    ShowCodes();
    return UCXAPP_MATTER_OK;
}

void Stop()
{
    if (gAdvertising)
    {
        (void) uCxBluetoothLegacyAdvertisementStop(gUcx);
        gAdvertising = false;
    }
    Log(UCXAPP_MATTER_LOG_INFO, "node stopped");
}

void Urc(char * line, size_t length, const uint8_t *, size_t)
{
    ucxm_host_urc(line, length);
}

int Status()
{
    int link = gBleLink;

    Print("node: %s over %s, %d endpoint(s)\n", gModel, gNetwork, EndpointCount());
    if (link >= 0)
    {
        Print("state: Bluetooth LE link %d up, not commissioned\n", link);
    }
    else
    {
        Print("state: advertising over Bluetooth LE, not commissioned\n");
    }
    Print("setup: discriminator %u, vendor 0x%04X, product 0x%04X, passcode %u (the SDK's test device)\n",
          static_cast<unsigned>(gOnboarding.discriminator), static_cast<unsigned>(kVendorId),
          static_cast<unsigned>(kProductId), static_cast<unsigned>(kPasscode));
    Print("commissioning: not in this plug-in yet; it advertises and shows the setup codes\n");
    return UCXAPP_MATTER_OK;
}

int Endpoints()
{
    char value[24];

    for (int i = 0; i < EndpointCount(); i++)
    {
        const Endpoint & e = EndpointAt(i);

        e.kind->show(e, value, sizeof(value));
        Print("%u  %-8s %-8s %s, set: %s\n", static_cast<unsigned>(e.id), e.name, value, e.kind->title, e.kind->values);
    }
    Print("kinds:");
    for (int k = 0; k < KindCount(); k++)
    {
        Print(" %s", KindAt(k).name);
    }
    Print("\n");
    return UCXAPP_MATTER_OK;
}

int Set(int argc, const char * const argv[])
{
    char value[24];
    Endpoint * e = argc >= 3 ? EndpointFind(argv[1]) : nullptr;

    if (argc < 3)
    {
        return UCXAPP_MATTER_ERR_USAGE;
    }
    if (e == nullptr)
    {
        Print("ERROR: no endpoint %s; matter.endpoints lists them\n", argv[1]);
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (!e->kind->set(*e, argc - 2, argv + 2))
    {
        Print("ERROR: %s takes %s\n", e->name, e->kind->values);
        return UCXAPP_MATTER_ERR_FAILED;
    }
    e->kind->show(*e, value, sizeof(value));
    Event("matter: %s = %s", e->name, value);
    return UCXAPP_MATTER_OK;
}

int Command(int argc, const char * const argv[])
{
    if (argc < 1)
    {
        return UCXAPP_MATTER_ERR_USAGE;
    }
    if (strcmp(argv[0], "status") == 0)
    {
        return Status();
    }
    if (strcmp(argv[0], "endpoints") == 0)
    {
        return Endpoints();
    }
    if (strcmp(argv[0], "set") == 0)
    {
        return Set(argc, argv);
    }
    if (strcmp(argv[0], "qr") == 0)
    {
        ShowCodes();
        return UCXAPP_MATTER_OK;
    }
    if (strcmp(argv[0], "reset") == 0)
    {
        return UCXAPP_MATTER_OK; // nothing is stored yet; the app removes the files
    }
    return UCXAPP_MATTER_ERR_UNSUPPORTED;
}

const ucxapp_matter_plugin_t kPlugin = {
    UCXAPP_MATTER_ABI, sizeof(ucxapp_matter_plugin_t), "Matter SDK " UCXM_SDK_VERSION, Start, Stop, Urc, Command,
};

} // namespace

extern "C" UCXM_EXPORT const ucxapp_matter_plugin_t * ucxmatter_plugin(void)
{
    return &kPlugin;
}
