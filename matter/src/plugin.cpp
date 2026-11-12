/* ucxmatter: the Matter node of u-connectXpressApp, loaded by the app's matter.* commands (app/src/ucxapp_matter.h).
 * The node reaches the module through the app's AT client and sees a copy of every URC, so it never owns the UART.
 *
 * This file is the module side: the plug-in entry and the commands. What needs the Matter SDK sits in its own
 * files (node.cpp, onboarding.cpp), see the note in onboarding.h. */
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "endpoints.h"
#include "node.h"
#include "onboarding.h"
#include "progress.h"
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

#include <mbedtls/version.h>

// One component of app.sbom (ucxapp_matter.h)
#define UCXM_PART(name, version, licence, use, source) name " " version "  [" licence "]\n  " use "\n  " source "\n\n"

#if defined(UCX_KEYLOG) && UCX_KEYLOG
#define UCXM_BUILD ", KEY LOG BUILD"
#else
#define UCXM_BUILD ""
#endif

namespace {

using namespace ucxm;

const ucxapp_matter_host_t * gHost;
uCxHandle_t * gUcx;
Onboarding gOnboarding;
std::mutex gCodesLock; // gOnboarding, between the card's thread and matter.start
char gModel[32];
char gType[40];
char gSerial[40];
char gFirmware[40];
const char * gNetwork;
bool gStarted;
std::atomic<bool> gCardReady{ false };
NodeState gCardState; // the last one the Matter thread had time for

void Print(const char * format, ...) __attribute__((format(gnu_printf, 1, 2)));
void Log(int level, const char * format, ...) __attribute__((format(gnu_printf, 2, 3)));
void Event(const char * format, ...) __attribute__((format(gnu_printf, 1, 2)));
size_t CardLine(char * out, size_t size, size_t n, const char * format, ...) __attribute__((format(gnu_printf, 4, 5)));

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

// On the AT client's thread (ucxapp_matter.h): "+UEBTC:<link>,<address>" and "+UEBTDC:<link>" for the Events pane
void NoteLink(const char * line, size_t length)
{
    const bool up = length > 7 && strncmp(line, "+UEBTC:", 7) == 0;

    if (up || (length > 8 && strncmp(line, "+UEBTDC:", 8) == 0))
    {
        Event("matter: Bluetooth LE link %d %s", atoi(line + (up ? 7 : 8)), up ? "up" : "down");
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

// "0233-981-9486" for the usual 11 digits
void ManualText(char * out, size_t size)
{
    const char * m = gOnboarding.manual;

    if (strlen(m) == 11)
    {
        snprintf(out, size, "%.4s-%.3s-%.4s", m, m + 4, m + 7);
    }
    else
    {
        snprintf(out, size, "%s", m);
    }
}

void ShowCodes()
{
    char manual[32];

    ManualText(manual, sizeof(manual));
    gHost->qr(gOnboarding.qr);
    Print("manual code: %s\n", manual);
    Print("QR payload: %s\n", gOnboarding.qr);
}

// QR and manual code for the node's setup codes
bool TakeSetup(const NodeSetup & setup)
{
    Onboarding codes = Onboarding();

    codes.vendorId      = setup.vendorId;
    codes.productId     = setup.productId;
    codes.passcode      = setup.passcode;
    codes.discriminator = setup.discriminator;
    if (!OnboardingCodes(codes))
    {
        Print("ERROR: the Matter SDK made no setup codes\n");
        return false;
    }
    std::lock_guard<std::mutex> lock(gCodesLock);

    gOnboarding = codes;
    return true;
}

// One text answer of the module; "" when it gives none
void Ask(bool (*begin)(uCxHandle_t *, const char **), char * out, size_t size)
{
    const char * text = nullptr;

    out[0] = '\0';
    if (begin(gUcx, &text) && text != nullptr)
    {
        snprintf(out, size, "%s", text);
    }
    (void) uCxEnd(gUcx);
}

size_t CardLine(char * out, size_t size, size_t n, const char * format, ...)
{
    va_list args;
    int written;

    if (n + 1 >= size)
    {
        return n;
    }
    va_start(args, format);
    written = vsnprintf(out + n, size - n, format, args);
    va_end(args);
    return written < 0 ? n : n + static_cast<size_t>(written) < size ? n + static_cast<size_t>(written) : size - 1;
}

// The live card of ucxapp_matter.h: the app calls it from its web thread, one call at a time
size_t Card(char * out, size_t size)
{
    char text[128];
    size_t n = 0;

    if (size == 0)
    {
        return 0;
    }
    out[0] = '\0';
    if (!gCardReady)
    {
        return CardLine(out, size, n, "NODE  starting\n");
    }
    (void) NodeSnapshot(gCardState);
    n = CardLine(out, size, n, "NODE  %s over %s, %d endpoint(s)\n", gModel, gNetwork, EndpointCount());
    if (gCardState.fabrics > 0)
    {
        n = CardLine(out, size, n, "STATE commissioned, %d fabric(s)\n", gCardState.fabrics);
    }
    else
    {
        n = CardLine(out, size, n, "STATE not commissioned\n");
    }
    if (ProgressLine(text, sizeof(text)))
    {
        n = CardLine(out, size, n, "STEP  %s\n", text);
    }
    n = CardLine(out, size, n, "BLE   %s\nWIN   %s\nNET   %s\n",
                 gCardState.links > 0 ? "link up" : gCardState.advertising ? "advertising" : "off",
                 gCardState.windowOpen ? "open" : "closed", gCardState.network);
    {
        std::lock_guard<std::mutex> lock(gCodesLock);

        ManualText(text, sizeof(text));
        n = CardLine(out, size, n, "CODE  %s\nQR    %s\n", text, gOnboarding.qr);
    }
    for (int i = 0; i < EndpointCount(); i++)
    {
        const Endpoint & e = EndpointAt(i);

        e.kind->show(e, text, sizeof(text));
        n = CardLine(out, size, n, "EP    %u %s = %s  [%s]\n", static_cast<unsigned>(e.id), e.name, text, e.kind->values);
    }
    return n;
}

int Start(const ucxapp_matter_host_t * host, int argc, const char * const argv[])
{
    const char * model = nullptr;
    const char * failed;
    char suffix[8];
    NodeSetup setup = {};
    uMacAddress_t address;
    uBtMode_t mode;

    if (host == nullptr || host->abi != UCXAPP_MATTER_ABI || host->size < sizeof(ucxapp_matter_host_t))
    {
        return UCXAPP_MATTER_ERR_FAILED;
    }
    gHost    = host;
    gStarted = false;
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

    // An advertisement the module still runs (bt.adv, a profile) would keep its own data; no error if there is none
    (void) uCxBluetoothLegacyAdvertisementStop(gUcx);

    // What a controller shows for the node: the module is its hardware
    Ask(uCxGeneralGetTypeCodeBegin, gType, sizeof(gType));
    Ask(uCxGeneralGetSerialNumberBegin, gSerial, sizeof(gSerial));
    Ask(uCxGeneralGetSoftwareVersionBegin, gFirmware, sizeof(gFirmware));

    // Vendor and product are the Matter SDK's test ones, which every commissioner accepts as a test device
    snprintf(suffix, sizeof(suffix), "%02X%02X%02X", address.address[3], address.address[4], address.address[5]);
    const NodeModule module = { suffix, gModel, gType, gSerial, gFirmware };

    gStarted = true;
    failed   = NodeStart(module, (host->radios & UCXAPP_MATTER_RADIO_WIFI) != 0,
                         [](const char * line) { gHost->event(line); }, setup);
    if (failed != nullptr)
    {
        Print("ERROR: %s\n", failed);
        NodeStop();
        gStarted = false;
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (!TakeSetup(setup))
    {
        NodeStop();
        gStarted = false;
        return UCXAPP_MATTER_ERR_FAILED;
    }
    Log(UCXAPP_MATTER_LOG_INFO, "%s over %s, %d endpoint(s), discriminator %u", gModel, gNetwork, EndpointCount(),
        static_cast<unsigned>(gOnboarding.discriminator));
#if defined(UCX_KEYLOG) && UCX_KEYLOG
    Log(UCXAPP_MATTER_LOG_ERROR, "KEY LOG BUILD: the node writes its session keys to the file UCX_KEYLOG_FILE names");
#endif
    gCardState.fabrics     = NodeFabrics();
    gCardState.links       = NodeLinks();
    gCardState.advertising = NodeAdvertising();
    gCardState.windowOpen  = NodeWindowOpen();
    NodeNetwork(gCardState.network, sizeof(gCardState.network));
    gCardReady = true;
    if (gCardState.fabrics == 0)
    {
        ShowCodes();
    }
    return UCXAPP_MATTER_OK;
}

void Stop()
{
    gCardReady = false;
    if (gStarted)
    {
        NodeStop();
        gStarted = false;
    }
    Log(UCXAPP_MATTER_LOG_INFO, "node stopped");
}

bool Urc(char * line, size_t length, const uint8_t *, size_t)
{
    NoteLink(line, length);
    return NodeUrc(line, length);
}

int Status()
{
    const int fabrics = NodeFabrics();
    char network[160];
    const char * ble  = NodeLinks() > 0 ? "Bluetooth LE link up"
        : NodeAdvertising() ? "advertising over Bluetooth LE" : "not advertising";

    Print("node: %s over %s, %d endpoint(s)\n", gModel, gNetwork, EndpointCount());
    Print("product: u-blox u-connectXpressApp, hardware %s, serial %s, software %s\n", gType[0] != '\0' ? gType : gModel,
          gSerial[0] != '\0' ? gSerial : "-", gFirmware[0] != '\0' ? gFirmware : "-");
    if (fabrics > 0)
    {
        Print("state: commissioned (%d fabric(s)), %s\n", fabrics, ble);
    }
    else
    {
        Print("state: %s, not commissioned\n", ble);
    }
    Print("commissioning window: %s\n", NodeWindowOpen() ? "open" : "closed (matter.open opens it again)");
    Print("setup: discriminator %u, vendor 0x%04X, product 0x%04X, passcode %08u\n",
          static_cast<unsigned>(gOnboarding.discriminator), static_cast<unsigned>(gOnboarding.vendorId),
          static_cast<unsigned>(gOnboarding.productId), static_cast<unsigned>(gOnboarding.passcode));
    NodeNetwork(network, sizeof(network));
    Print("network: %s\n", network);
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

void Check(int & failed, bool ok, const char * name, const char * detail)
{
    Print("  %-12s %s  %s\n", name, ok ? "ok    " : "FAILED", detail);
    failed += ok ? 0 : 1;
}

// matter.selftest, the node's part; the app checked the plug-in, the module and the store
int Selftest()
{
    char text[160];
    Onboarding again  = Onboarding();
    int failed        = 0;
    const int fabrics = NodeFabrics();
    const int links   = NodeLinks();
    const bool window = NodeWindowOpen();

    Check(failed, NodeRandomOk(), "random", "the SDK's DRBG gives bytes, two draws differ");

    again.vendorId      = gOnboarding.vendorId;
    again.productId     = gOnboarding.productId;
    again.passcode      = gOnboarding.passcode;
    again.discriminator = gOnboarding.discriminator;
    snprintf(text, sizeof(text), "discriminator %u, not the SDK's test codes, QR and manual code match them",
             static_cast<unsigned>(gOnboarding.discriminator));
    Check(failed,
          OnboardingCodes(again) && strcmp(again.qr, gOnboarding.qr) == 0 && strcmp(again.manual, gOnboarding.manual) == 0 &&
              SetupCodesAllowed(gOnboarding.passcode, gOnboarding.discriminator),
          "setup codes", text);

    if (fabrics > 0)
    {
        snprintf(text, sizeof(text), "commissioned (%d fabric(s)), window %s", fabrics, window ? "open" : "closed");
        Check(failed, true, "Bluetooth LE", text);
    }
    else
    {
        const bool ble = links > 0 || NodeAdvertising();

        snprintf(text, sizeof(text), "%s, window %s%s", links > 0 ? "link up" : ble ? "advertising" : "not advertising",
                 window ? "open" : "closed", window ? "" : " (matter.open opens it)");
        Check(failed, ble && window, "Bluetooth LE", text);
    }

    NodeNetwork(text, sizeof(text));
    Check(failed, fabrics == 0 || strncmp(text, "Wi-Fi up", 8) == 0 || strncmp(text, "Thread", 6) == 0, "network",
          text);
    return failed == 0 ? UCXAPP_MATTER_OK : UCXAPP_MATTER_ERR_FAILED;
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
    if (strcmp(argv[0], "open") == 0)
    {
        if (argc > 2)
        {
            return UCXAPP_MATTER_ERR_USAGE;
        }
        if (!NodeOpenWindow(argc == 2 ? static_cast<unsigned>(atoi(argv[1])) : 900))
        {
            Print("ERROR: the node did not open a commissioning window (180 to 900 seconds)\n");
            return UCXAPP_MATTER_ERR_FAILED;
        }
        ShowCodes();
        return UCXAPP_MATTER_OK;
    }
    if (strcmp(argv[0], "selftest") == 0)
    {
        return Selftest();
    }
    return UCXAPP_MATTER_ERR_UNSUPPORTED;
}

// What is linked into the plug-in; the commits are those of the SDK checkout it was built from (ucxapp.py)
const char kSbom[] =
    UCXM_PART("Matter SDK", UCXM_SDK_VERSION " commit " UCXM_SDK_COMMIT, "Apache-2.0",
              "the Matter node of the plug-in ucxmatter (matter.*)", "https://github.com/project-chip/connectedhomeip")
    UCXM_PART("mbedTLS (Matter SDK)", MBEDTLS_VERSION_STRING " commit " UCXM_MBEDTLS_COMMIT, "Apache-2.0",
              "crypto of the Matter node", "https://github.com/Mbed-TLS/mbedtls")
    UCXM_PART("Pigweed", "commit " UCXM_PIGWEED_COMMIT, "Apache-2.0", "support library of the Matter SDK",
              "https://pigweed.googlesource.com/pigweed/pigweed")
    UCXM_PART("nlassert", "commit " UCXM_NLASSERT_COMMIT, "Apache-2.0", "assertions of the Matter SDK",
              "https://github.com/nestlabs/nlassert")
    UCXM_PART("nlio", "commit " UCXM_NLIO_COMMIT, "Apache-2.0", "byte order helpers of the Matter SDK",
              "https://github.com/nestlabs/nlio")
#ifdef _WIN32
    UCXM_PART("GCC runtime libraries", __VERSION__, "GPL-3.0-or-later with GCC Runtime Library Exception",
              "libstdc++ and libgcc, linked into ucxmatter.dll", "https://gcc.gnu.org")
    UCXM_PART("MinGW-w64 runtime", __MINGW64_VERSION_STR, "ZPL-2.1, MIT, BSD-3-Clause",
              "C runtime start-up and winpthreads, linked into ucxmatter.dll", "https://www.mingw-w64.org")
#endif
    ;

const ucxapp_matter_plugin_t kPlugin = {
    UCXAPP_MATTER_ABI, sizeof(ucxapp_matter_plugin_t), "Matter SDK " UCXM_SDK_VERSION UCXM_BUILD, Start, Stop, Urc, Command,
    Card, kSbom,
};

} // namespace

extern "C" UCXM_EXPORT const ucxapp_matter_plugin_t * ucxmatter_plugin(void)
{
    return &kPlugin;
}
