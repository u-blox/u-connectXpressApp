/* Demo material for the examples on a virtual module (vucx, the browser demo): values for the ${VAR} placeholders
 * that are not set, the files the examples read (made only when missing, never overwritten) and virt:pc for the
 * second COM port. Real modules never see any of it. */

#include "ucxapp_demo.h"

#if UCXAPP_HOST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "ucxapp_cmd.h"
#include "ucxapp_store.h"
#include "ucxapp_tls.h"
#include "ucxapp_vport.h"

#define DEMO_CLIENT "demo-client"
#define DER_MAX     4096
#define SEQ         0x30

static const char *const kVars[][2] = {
    { "NTRIP_USER", "demo" }, { "NTRIP_PASS", "demo" }, { "PP_CLIENT_ID", "demo-client" },
    { "ASSISTNOW_TOKEN", "00000000-0000-0000-0000-000000000000" }, { "GOOGLE_API_KEY", "demo-key" },
    { "COMBAIN_API_KEY", "demo-key" }, { "MQTT_PASS", "demo" }, { "PEAP_PASSWORD", "demo" },
    { "UCX_WIFI_SSID", "vucx-net" }, { "UCX_WIFI_PASS", "demo-pass" },
};

static const char *const kCaFiles[] = { "ca.pem", "broker-ca.pem", "root-ca.pem", "AmazonRootCA1.pem" };
static const char *const kCrtFiles[] = { "client.pem", "device.pem", "device-cert.pem" };
static const char *const kKeyFiles[] = { "client.key", "device.key", "device-key.pem" };
static const char *const kTextFiles[] = { "config.txt", "notes.txt", "log.txt", "session.txt" };
static const char *const kCfgFiles[] = { "golden.cfg", "field.cfg", "line.cfg", "release.cfg" };

/* A small rectangle round the u-blox Malmo office, where the virtual receiver sits by default. */
static const char kGardenKml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><name>garden (demo)</name><Placemark><name>garden</name>"
    "<Polygon><outerBoundaryIs><LinearRing><coordinates>\n"
    "12.9915,55.6120,0 12.9935,55.6120,0 12.9935,55.6130,0 12.9915,55.6130,0 12.9915,55.6120,0\n"
    "</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark></Document></kml>\n";

static int gMade;

static void setDemoEnv(const char *name, const char *value)
{
#ifdef _WIN32
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 0);
#endif
}

static bool exists(const char *name)
{
    FILE *f = fopen(name, "rb");

    if (f != NULL) {
        fclose(f);
        return true;
    }
    return ucxapp_store_exists(name);
}

static void made(const char *name)
{
    ucxapp_print("%s%s", gMade++ == 0 ? "demo files for the virtual module: " : ", ", name);
}

/* Ends the list of made files, so a note or a command's output starts on its own line. */
static void endList(void)
{
    if (gMade > 0) {
        ucxapp_print("\n");
        gMade = 0;
    }
}

static void writeFile(const char *name, const void *data, size_t len)
{
    FILE *f;

    if (exists(name) || (f = fopen(name, "wb")) == NULL) {
        return;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    if (ok) {
        made(name);
    }
}

/* ---- DER, built back to front so every length is known when its header is written ---- */

typedef struct {
    uint8_t buf[DER_MAX];
    size_t at;   /* the content starts at buf + at */
    bool ok;
} der_t;

static void derInit(der_t *d)
{
    d->at = DER_MAX;
    d->ok = true;
}

static void derRaw(der_t *d, const void *data, size_t len)
{
    if (!d->ok || len > d->at) {
        d->ok = false;
        return;
    }
    d->at -= len;
    memcpy(d->buf + d->at, data, len);
}

/* Wraps everything from mark to the end in tag + length. */
static void derWrap(der_t *d, size_t mark, uint8_t tag)
{
    size_t len = mark - d->at;
    uint8_t hdr[4];
    size_t n = 0;

    if (len < 0x80) {
        hdr[n++] = (uint8_t)len;
    } else if (len < 0x100) {
        hdr[n++] = 0x81;
        hdr[n++] = (uint8_t)len;
    } else {
        hdr[n++] = 0x82;
        hdr[n++] = (uint8_t)(len >> 8);
        hdr[n++] = (uint8_t)len;
    }
    derRaw(d, hdr, n);
    derRaw(d, &tag, 1);
}

static size_t derEnd(const der_t *d)
{
    return DER_MAX - d->at;
}

static void derOid(der_t *d, const uint8_t *oid, size_t len)
{
    size_t mark = d->at;

    derRaw(d, oid, len);
    derWrap(d, mark, 0x06);
}

static void derInt(der_t *d, uint8_t v)
{
    uint8_t body[3] = { 0x02, 0x01, v };

    derRaw(d, body, sizeof(body));
}

static const uint8_t kOidData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01 };
static const uint8_t kOidSignedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };
static const uint8_t kOidKeyBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01 };
static const uint8_t kOidCertBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03 };
static const uint8_t kOidX509Cert[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x16, 0x01 };
static const uint8_t kOidEcKey[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t kOidP256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };

/* PKCS#7 certificates only (.p7b): ContentInfo { signedData, [0] SignedData with no signers }. */
static void writeP7b(const char *name, const mbedtls_x509_crt *const *certs, int n)
{
    static der_t d;
    size_t mark;

    derInit(&d);
    derRaw(&d, "\x31\x00", 2);                       /* signerInfos */
    mark = d.at;
    for (int i = n - 1; i >= 0; i--) {
        derRaw(&d, certs[i]->raw.p, certs[i]->raw.len);
    }
    derWrap(&d, mark, 0xA0);                         /* [0] certificates */
    mark = d.at;
    derOid(&d, kOidData, sizeof(kOidData));
    derWrap(&d, mark, SEQ);                          /* encapContentInfo */
    derRaw(&d, "\x31\x00", 2);                       /* digestAlgorithms */
    derInt(&d, 1);
    derWrap(&d, DER_MAX, SEQ);                       /* SignedData */
    derWrap(&d, DER_MAX, 0xA0);
    derOid(&d, kOidSignedData, sizeof(kOidSignedData));
    derWrap(&d, DER_MAX, SEQ);
    if (d.ok) {
        writeFile(name, d.buf + d.at, derEnd(&d));
    }
}

/* PKCS#12 (.p12/.pfx) with the key in a plain keyBag and no MAC: any password opens it. */
static void writeP12(const char *name, const uint8_t *pkcs8, size_t keyLen, const mbedtls_x509_crt *const *certs,
                     int n)
{
    static der_t d;
    size_t bag;

    derInit(&d);
    for (int i = n - 1; i >= 0; i--) {
        bag = d.at;
        derRaw(&d, certs[i]->raw.p, certs[i]->raw.len);
        derWrap(&d, bag, 0x04);
        derWrap(&d, bag, 0xA0);
        derOid(&d, kOidX509Cert, sizeof(kOidX509Cert));
        derWrap(&d, bag, SEQ);                       /* CertBag */
        derWrap(&d, bag, 0xA0);
        derOid(&d, kOidCertBag, sizeof(kOidCertBag));
        derWrap(&d, bag, SEQ);                       /* SafeBag */
    }
    bag = d.at;
    derRaw(&d, pkcs8, keyLen);
    derWrap(&d, bag, 0xA0);
    derOid(&d, kOidKeyBag, sizeof(kOidKeyBag));
    derWrap(&d, bag, SEQ);
    derWrap(&d, DER_MAX, SEQ);                       /* SafeContents */
    derWrap(&d, DER_MAX, 0x04);
    derWrap(&d, DER_MAX, 0xA0);
    derOid(&d, kOidData, sizeof(kOidData));
    derWrap(&d, DER_MAX, SEQ);                       /* ContentInfo */
    derWrap(&d, DER_MAX, SEQ);                       /* AuthenticatedSafe */
    derWrap(&d, DER_MAX, 0x04);
    derWrap(&d, DER_MAX, 0xA0);
    derOid(&d, kOidData, sizeof(kOidData));
    derWrap(&d, DER_MAX, SEQ);                       /* authSafe */
    derInt(&d, 3);
    derWrap(&d, DER_MAX, SEQ);                       /* PFX */
    if (d.ok) {
        writeFile(name, d.buf + d.at, derEnd(&d));
    }
}
/* The SEC1 key as PKCS#8 PrivateKeyInfo, as a keyBag holds it. */
static size_t pkcs8(mbedtls_pk_context *key, uint8_t *out, size_t size)
{
    static der_t d;
    uint8_t sec1[256];
    int n = mbedtls_pk_write_key_der(key, sec1, sizeof(sec1));
    size_t alg;

    if (n <= 0) {
        return 0;
    }
    derInit(&d);
    derRaw(&d, sec1 + sizeof(sec1) - (size_t)n, (size_t)n);
    derWrap(&d, DER_MAX, 0x04);
    alg = d.at;
    derOid(&d, kOidP256, sizeof(kOidP256));
    derOid(&d, kOidEcKey, sizeof(kOidEcKey));
    derWrap(&d, alg, SEQ);
    derInt(&d, 0);
    derWrap(&d, DER_MAX, SEQ);
    if (!d.ok || derEnd(&d) > size) {
        return 0;
    }
    memcpy(out, d.buf + d.at, derEnd(&d));
    return derEnd(&d);
}

/* The demo CA and a client certificate from the app's own CA (utils.tls), as the files the examples upload. */
static void demoCerts(void)
{
    char err[128], file[96];
    uint8_t *caPem, *crtPem, *keyPem;
    size_t caLen = 0, crtLen = 0, keyLen = 0;
    bool need = false;
    const char *const other[] = { "device.p12", "device.pfx", "chain.p7b", "root.cer" };

    for (size_t i = 0; i < 4; i++) {
        need = need || !exists(other[i]) || !exists(kCaFiles[i]) ||
               (i < 3 && (!exists(kCrtFiles[i]) || !exists(kKeyFiles[i])));
    }
    if (!need) {
        return;
    }
    ucxapp_tls_cert_file(UCXAPP_TLS_CA_NAME, file, sizeof(file));
    if (!ucxapp_store_exists(file) && !ucxapp_tls_create_ca("ucxapp-demo-ca", false, err, sizeof(err))) {
        endList();
        ucxapp_print("note: no demo certificates (%s)\n", err);
        return;
    }
    ucxapp_tls_cert_file(DEMO_CLIENT, file, sizeof(file));
    if (!ucxapp_store_exists(file) && !ucxapp_tls_create_cert(DEMO_CLIENT, true, NULL, 0, err, sizeof(err))) {
        endList();
        ucxapp_print("note: no demo certificates (%s)\n", err);
        return;
    }
    ucxapp_tls_cert_file(UCXAPP_TLS_CA_NAME, file, sizeof(file));
    caPem = ucxapp_store_load(file, &caLen);
    ucxapp_tls_cert_file(DEMO_CLIENT, file, sizeof(file));
    crtPem = ucxapp_store_load(file, &crtLen);
    ucxapp_tls_key_file(DEMO_CLIENT, file, sizeof(file));
    keyPem = ucxapp_store_load(file, &keyLen);
    if (caPem != NULL && crtPem != NULL && keyPem != NULL) {
        mbedtls_x509_crt ca, crt;
        mbedtls_pk_context key;
        uint8_t key8[300];
        size_t key8Len = 0;

        for (size_t i = 0; i < sizeof(kCaFiles) / sizeof(kCaFiles[0]); i++) {
            writeFile(kCaFiles[i], caPem, caLen);
        }
        for (size_t i = 0; i < sizeof(kCrtFiles) / sizeof(kCrtFiles[0]); i++) {
            writeFile(kCrtFiles[i], crtPem, crtLen);
        }
        for (size_t i = 0; i < sizeof(kKeyFiles) / sizeof(kKeyFiles[0]); i++) {
            writeFile(kKeyFiles[i], keyPem, keyLen);
        }
        (void)psa_crypto_init();
        mbedtls_x509_crt_init(&ca);
        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&key);
        if (mbedtls_x509_crt_parse(&ca, caPem, caLen + 1) == 0 && mbedtls_x509_crt_parse(&crt, crtPem, crtLen + 1) == 0 &&
            mbedtls_pk_parse_key(&key, keyPem, keyLen + 1, NULL, 0) == 0) {
            const mbedtls_x509_crt *chain[2] = { &crt, &ca };

            writeFile("root.cer", ca.raw.p, ca.raw.len);
            writeP7b("chain.p7b", chain, 2);
            key8Len = pkcs8(&key, key8, sizeof(key8));
            if (key8Len > 0) {
                writeP12("device.p12", key8, key8Len, chain, 2);
                writeP12("device.pfx", key8, key8Len, chain, 2);
            }
        } else {
            endList();
            ucxapp_print("note: the demo certificate or key does not parse\n");
        }
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&crt);
        mbedtls_x509_crt_free(&ca);
    } else {
        endList();
        ucxapp_print("note: no demo certificates (%s not readable)\n", caPem == NULL ? "CA" : "client");
    }
    free(caPem);
    free(crtPem);
    free(keyPem);
}

/* A minute of a slow walk north from the Malmo office, for gnss.replay. */
static void demoTrack(void)
{
    char text[4096];
    size_t n = 0;

    if (exists("drive.nmea")) {
        return;
    }
    for (int i = 0; i < 60 && n < sizeof(text) - 200; i++) {
        char body[2][100];
        double minLat = 36.75 + i * 0.0005;   /* 55 deg 36.75 min N, about 1 m a second */

        snprintf(body[0], sizeof(body[0]), "GPRMC,1200%02d.00,A,55%08.5f,N,01259.55000,E,1.9,0.0,010125,,,A", i, minLat);
        snprintf(body[1], sizeof(body[1]), "GPGGA,1200%02d.00,55%08.5f,N,01259.55000,E,1,10,0.8,10.0,M,0.0,M,,", i,
                 minLat);
        for (int k = 0; k < 2; k++) {
            uint8_t x = 0;
            for (const char *c = body[k]; *c != '\0'; c++) {
                x ^= (uint8_t)*c;
            }
            n += (size_t)snprintf(text + n, sizeof(text) - n, "$%s*%02X\r\n", body[k], x);
        }
    }
    writeFile("drive.nmea", text, n);
}

static void demoFiles(ucxapp_t *app)
{
    static uint8_t blob[8192];
    char text[160];

    for (size_t i = 0; i < sizeof(kTextFiles) / sizeof(kTextFiles[0]); i++) {
        int n = snprintf(text, sizeof(text), "%s: a demo file the examples send and fetch (ucxapp virtual module)\n",
                         kTextFiles[i]);
        writeFile(kTextFiles[i], text, (size_t)n);
    }
    for (size_t i = 0; i < sizeof(blob); i++) {
        blob[i] = (uint8_t)(i * 7u + (i >> 8));
    }
    writeFile("big.bin", blob, sizeof(blob));
    writeFile("fw.bin", blob, 4096);
    writeFile("garden.kml", kGardenKml, sizeof(kGardenKml) - 1);
    demoTrack();
    demoCerts();
    for (size_t i = 0; i < sizeof(kCfgFiles) / sizeof(kCfgFiles[0]); i++) {
        /* the module's own settings; a module that returns none (Thread only) gets no cfg files */
        if (!exists(kCfgFiles[i]) && ucxapp_cfg_save_quiet(app, kCfgFiles[i])) {
            made(kCfgFiles[i]);
        }
    }
}

void ucxapp_demo_prepare(ucxapp_t *app)
{
    if (!ucxapp_module_is_virtual(app)) {
        return;
    }
    for (size_t i = 0; i < sizeof(kVars) / sizeof(kVars[0]); i++) {
        if (getenv(kVars[i][0]) == NULL) {
            setDemoEnv(kVars[i][0], kVars[i][1]);
        }
    }
    gMade = 0;
    demoFiles(app);
    endList();
}

void ucxapp_demo_line(ucxapp_t *app, char *line, size_t size)
{
    char *com = strstr(line, "COM20");

    if (com == NULL || !ucxapp_module_is_virtual(app) || strlen(line) + 2 >= size) {
        return;
    }
    memmove(com + 7, com + 5, strlen(com + 5) + 1);
    memcpy(com, UCXAPP_VPORT_PC, 7);
}

#else

void ucxapp_demo_prepare(ucxapp_t *app)
{
    (void)app;
}

void ucxapp_demo_line(ucxapp_t *app, char *line, size_t size)
{
    (void)app;
    (void)line;
    (void)size;
}

#endif
