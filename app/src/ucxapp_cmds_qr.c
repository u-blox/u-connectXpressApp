#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

#include "qrcodegen/qrcodegen.h"

#define QR_BORDER 2

static bool gQrLarge;    /* large: two characters per module; default small: half blocks, two rows per line */

/* Takes "small" or "large" off the end of argv; returns the remaining argc. */
static int takeSize(int argc, char *argv[])
{
    if (argc > 1 && (strcmp(argv[argc - 1], "small") == 0 || strcmp(argv[argc - 1], "large") == 0)) {
        gQrLarge = strcmp(argv[argc - 1], "large") == 0;
        return argc - 1;
    }
    gQrLarge = false;
    return argc;
}

/* Escape characters that are special in a WIFI: payload. */
static bool escapeWifi(const char *in, char *out, size_t size)
{
    size_t n = 0;

    for (; *in != '\0'; in++) {
        bool special = strchr("\\;,:\"", *in) != NULL;
        if (n + (special ? 2 : 1) + 1 > size) {
            return false;
        }
        if (special) {
            out[n++] = '\\';
        }
        out[n++] = *in;
    }
    out[n] = '\0';
    return true;
}

bool ucxapp_qr_print(const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];

    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        ucxapp_print("ERROR: text too long for a QR code\n");
        return false;
    }
    int size = qrcodegen_getSize(qr);
    if (gQrLarge) {
        for (int y = -QR_BORDER; y < size + QR_BORDER; y++) {
            for (int x = -QR_BORDER; x < size + QR_BORDER; x++) {
                ucxapp_print(qrcodegen_getModule(qr, x, y) ? "\xE2\x96\x88\xE2\x96\x88" : "  ");
            }
            ucxapp_print("\n");
        }
        return true;
    }
    /* full block, upper half, lower half, space */
    static const char *const kHalf[4] = { " ", "\xE2\x96\x84", "\xE2\x96\x80", "\xE2\x96\x88" };
    for (int y = -QR_BORDER; y < size + QR_BORDER; y += 2) {
        for (int x = -QR_BORDER; x < size + QR_BORDER; x++) {
            ucxapp_print("%s", kHalf[(qrcodegen_getModule(qr, x, y) ? 2 : 0) | (qrcodegen_getModule(qr, x, y + 1) ? 1 : 0)]);
        }
        ucxapp_print("\n");
    }
    return true;
}

bool ucxapp_qr_print_wifi(const char *ssid, const char *password, const char *security, bool hidden)
{
    char s[128];
    char p[128];
    char payload[320];

    if (ssid[0] == '\0') {
        ucxapp_print("ERROR: SSID is empty\n");
        return false;
    }
    if (!escapeWifi(ssid, s, sizeof(s)) || !escapeWifi(password, p, sizeof(p))) {
        ucxapp_print("ERROR: SSID or password too long\n");
        return false;
    }
    if (strcmp(security, "nopass") == 0 || password[0] == '\0') {
        snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;%s;", s, hidden ? "H:true;" : "");
    } else {
        snprintf(payload, sizeof(payload), "WIFI:T:%s;S:%s;P:%s;%s;", security, s, p, hidden ? "H:true;" : "");
    }
    ucxapp_print("%s\n", payload);
    return ucxapp_qr_print(payload);
}

static int cmdQrText(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    argc = takeSize(argc, argv);
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    return ucxapp_qr_print(argv[1]) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* utils.qr.wifi <ssid> [password] [WPA|WEP|nopass] [hidden] [small|large]; the keywords may come in any order and
 * the password may be left out (or empty) for an open network. */
static int cmdQrWifi(ucxapp_t *app, int argc, char *argv[])
{
    const char *password = NULL;
    const char *security = NULL;
    bool hidden = false;
    (void)app;

    argc = takeSize(argc, argv);
    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "WPA") == 0 || strcmp(argv[i], "WEP") == 0 || strcmp(argv[i], "nopass") == 0) {
            if (security != NULL) {
                return UCXAPP_ERR_USAGE;
            }
            security = argv[i];
        } else if (strcmp(argv[i], "hidden") == 0 && (password != NULL || i > 2)) {
            hidden = true;
        } else if (password == NULL && i == 2) {
            password = argv[i];
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (password == NULL) {
        password = "";
    }
    if (security == NULL) {
        security = password[0] == '\0' ? "nopass" : "WPA";
    } else if (strcmp(security, "nopass") != 0 && password[0] == '\0') {
        ucxapp_print("ERROR: %s needs a password\n", security);
        return UCXAPP_ERR_FAILED;
    }
    return ucxapp_qr_print_wifi(argv[1], strcmp(security, "nopass") == 0 ? "" : password, security, hidden)
               ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdQrGeo(ucxapp_t *app, int argc, char *argv[])
{
    char url[128];
    (void)app;

    argc = takeSize(argc, argv);
    if (argc != 3) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(url, sizeof(url), "https://www.google.com/maps?q=%s,%s", argv[1], argv[2]);
    ucxapp_print("%s\n", url);
    return ucxapp_qr_print(url) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gQrCmds[] = {
    { "utils.qr.text", "<text> [small|large]",                      "Print any text as a QR code",                    0, cmdQrText },
    { "utils.qr.wifi", "<ssid> [password] [WPA|WEP|nopass] [hidden] [small|large]", "Print a Wi-Fi join QR code (WIFI: payload)",  0, cmdQrWifi },
    { "utils.qr.geo",  "<lat> <lon> [small|large]",                "Print a Google Maps location QR code",           0, cmdQrGeo },
};

void ucxapp_register_qr_cmds(void)
{
    ucxapp_cmd_register(gQrCmds, sizeof(gQrCmds) / sizeof(gQrCmds[0]));
}
