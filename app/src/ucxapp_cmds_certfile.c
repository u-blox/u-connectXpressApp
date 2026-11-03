/* Certificate files in the formats customers get (PKCS#12 .p12/.pfx, PKCS#7 .p7b, DER .cer/.der, PEM)
   turned into the PEM the module takes: client chain, private key and CA certificates. Host builds only. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "ucxapp_cmd.h"
#include "ucxapp_store.h"

#include "mbedtls/asn1.h"
#include "mbedtls/md.h"
#include "mbedtls/pem.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/private/pkcs5.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CERTS 16
#define MODULE_NAME_MAX 32
#define UPLOAD_MAX 8192   /* cert.upload limit */
#define KEY_PEM_MAX 16000
#define SEQ (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)
#define EXPLICIT0 (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0)

typedef struct {
    mbedtls_x509_crt certs;
    int certCount;
    mbedtls_pk_context key;
    bool haveKey;
    const char *format;
    const char *pw;
    /* client chain (leaf first) and the other certificates, filled by classify() */
    const mbedtls_x509_crt *chain[MAX_CERTS];
    int chainCount;
    const mbedtls_x509_crt *cas[MAX_CERTS];
    int caCount;
} bundle_t;

static const uint8_t kOidData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01 };
static const uint8_t kOidSignedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };
static const uint8_t kOidEncryptedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x06 };
static const uint8_t kOidKeyBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01 };
static const uint8_t kOidShroudedKeyBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x02 };
static const uint8_t kOidCertBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03 };
static const uint8_t kOidSafeContentsBag[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x06 };
static const uint8_t kOidX509Cert[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x16, 0x01 };
static const uint8_t kOidPbes2[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0D };
static const uint8_t kOidPbmac1[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0E };
static const uint8_t kOidPkcs12Pbe[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x01 };   /* prefix */
static const uint8_t kOidSha1[] = { 0x2B, 0x0E, 0x03, 0x02, 0x1A };
static const uint8_t kOidSha2[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02 };              /* prefix */

#define OID_IS(buf, oid) ((buf).len == sizeof(oid) && memcmp((buf).p, (oid), sizeof(oid)) == 0)
#define OID_UNDER(buf, oid) ((buf).len > sizeof(oid) && memcmp((buf).p, (oid), sizeof(oid)) == 0)

static int getOid(unsigned char **p, const unsigned char *end, mbedtls_asn1_buf *oid)
{
    int rc = mbedtls_asn1_get_tag(p, end, &oid->len, MBEDTLS_ASN1_OID);
    if (rc == 0) {
        oid->p = *p;
        *p += oid->len;
    }
    return rc;
}

static bool malformed(const char *what)
{
    ucxapp_print("ERROR: malformed %s\n", what);
    return false;
}

static size_t pwLen(const char *pw)
{
    return pw != NULL ? strlen(pw) : 0;
}

static void addCert(bundle_t *b, const unsigned char *der, size_t len)
{
    int rc = mbedtls_x509_crt_parse_der(&b->certs, der, len);
    if (rc != 0) {
        ucxapp_print("warning: skipped a certificate that does not parse (-0x%04X)\n", (unsigned)-rc);
    } else {
        b->certCount++;
    }
}

static bool keyError(int rc)
{
    if (rc == MBEDTLS_ERR_PK_PASSWORD_REQUIRED || rc == MBEDTLS_ERR_PEM_PASSWORD_REQUIRED) {
        ucxapp_print("ERROR: the private key is encrypted; give the password\n");
    } else if (rc == MBEDTLS_ERR_PK_PASSWORD_MISMATCH || rc == MBEDTLS_ERR_PEM_PASSWORD_MISMATCH) {
        ucxapp_print("ERROR: wrong password for the private key\n");
    } else if (rc == MBEDTLS_ERR_PEM_UNKNOWN_ENC_ALG || rc == MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE) {
        ucxapp_print("ERROR: the private key uses an unsupported encryption (only AES); re-encrypt it with "
                     "openssl pkcs8 -topk8 -v2 aes256 -in key.pem -out key-aes.pem\n");
    } else {
        ucxapp_print("ERROR: cannot read the private key (-0x%04X)\n", (unsigned)-rc);
    }
    return false;
}

/* DER or NUL-terminated PEM (len includes the NUL). */
static bool addKey(bundle_t *b, const unsigned char *data, size_t len, const char *pw)
{
    if (b->haveKey) {
        ucxapp_print("warning: more than one private key; keeping the first\n");
        return true;
    }
    int rc = mbedtls_pk_parse_key(&b->key, data, len, pwLen(pw) > 0 ? (const unsigned char *)pw : NULL, pwLen(pw));
    if (rc != 0) {
        mbedtls_pk_free(&b->key);
        mbedtls_pk_init(&b->key);
        return keyError(rc);
    }
    b->haveKey = true;
    return true;
}

/* PBES2 (PBKDF2 + AES-CBC) data; malloc'ed plain text or NULL. */
static uint8_t *decrypt(const mbedtls_asn1_buf *alg, const mbedtls_asn1_buf *params, const unsigned char *data,
                        size_t len, const char *pw, size_t *outLen)
{
    if (OID_UNDER(*alg, kOidPkcs12Pbe)) {
        ucxapp_print("ERROR: this file uses the legacy PKCS#12 encryption (3DES or RC2, OpenSSL 1.x or older "
                     "Windows exports); re-export it with AES: openssl pkcs12 -legacy -in old.p12 -nodes | "
                     "openssl pkcs12 -export -out new.p12, or pick AES256-SHA256 in the Windows export wizard\n");
        return NULL;
    }
    if (!OID_IS(*alg, kOidPbes2)) {
        ucxapp_print("ERROR: unsupported encryption in the file (only PBES2 with AES)\n");
        return NULL;
    }
    uint8_t *out = (uint8_t *)malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    int rc = mbedtls_pkcs5_pbes2_ext(params, MBEDTLS_PKCS5_DECRYPT, (const unsigned char *)(pw != NULL ? pw : ""),
                                     pwLen(pw), data, len, out, len, outLen);
    if (rc != 0) {
        free(out);
        if (rc == MBEDTLS_ERR_PKCS5_PASSWORD_MISMATCH) {
            ucxapp_print("ERROR: wrong password\n");
        } else {
            ucxapp_print("ERROR: cannot decrypt (-0x%04X); the file may use a cipher mbedTLS does not have\n",
                         (unsigned)-rc);
        }
        return NULL;
    }
    return out;
}

/* ---- PKCS#12 ---- */

/* RFC 7292 B.2 key derivation, one hash length of output (all the MAC key needs). */
static bool p12Kdf(mbedtls_md_type_t md, const uint8_t *pwd, size_t plen, const uint8_t *salt, size_t slen,
                   int iterations, uint8_t id, uint8_t *out)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md);
    size_t u = mbedtls_md_get_size(info);
    size_t v = (md == MBEDTLS_MD_SHA384 || md == MBEDTLS_MD_SHA512) ? 128 : 64;
    size_t sl = slen > 0 ? v * ((slen + v - 1) / v) : 0;
    size_t pl = plen > 0 ? v * ((plen + v - 1) / v) : 0;
    uint8_t *buf = (uint8_t *)malloc(v + sl + pl);
    uint8_t tmp[64];
    bool ok = buf != NULL;

    if (ok) {
        memset(buf, id, v);
        for (size_t i = 0; i < sl; i++) {
            buf[v + i] = salt[i % slen];
        }
        for (size_t i = 0; i < pl; i++) {
            buf[v + sl + i] = pwd[i % plen];
        }
        ok = mbedtls_md(info, buf, v + sl + pl, out) == 0;
        for (int i = 1; ok && i < iterations; i++) {
            memcpy(tmp, out, u);
            ok = mbedtls_md(info, tmp, u, out) == 0;
        }
        mbedtls_platform_zeroize(buf, v + sl + pl);
        mbedtls_platform_zeroize(tmp, sizeof(tmp));
        free(buf);
    }
    return ok;
}

/* The password as a NUL-terminated big-endian BMPString, as the PKCS#12 MAC wants it. */
static uint8_t *bmpPassword(const char *pw, size_t *outLen)
{
    size_t n = strlen(pw), o = 0;
    uint8_t *out = (uint8_t *)malloc(n * 4 + 2);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n;) {
        const uint8_t *s = (const uint8_t *)pw + i;
        uint32_t cp = s[0];
        size_t step = 1;
        if ((s[0] & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            step = 2;
        } else if ((s[0] & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            step = 3;
        } else if ((s[0] & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                 ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            step = 4;
        }
        i += step;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            uint32_t hi = 0xD800 | (cp >> 10), lo = 0xDC00 | (cp & 0x3FF);
            out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)hi;
            cp = lo;
        }
        out[o++] = (uint8_t)(cp >> 8);
        out[o++] = (uint8_t)cp;
    }
    out[o++] = 0;
    out[o++] = 0;
    *outLen = o;
    return out;
}

static bool verifyMac(unsigned char *p, const unsigned char *end, const uint8_t *content, size_t clen, const char *pw)
{
    size_t len;
    mbedtls_asn1_buf alg, params, digest, salt;
    int iterations = 1;
    mbedtls_md_type_t md;

    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0 || mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) {
        return malformed("PKCS#12 MAC");
    }
    const unsigned char *digestInfoEnd = p + len;
    if (mbedtls_asn1_get_alg(&p, digestInfoEnd, &alg, &params) != 0 ||
        mbedtls_asn1_get_tag(&p, digestInfoEnd, &digest.len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return malformed("PKCS#12 MAC");
    }
    digest.p = p;
    p += digest.len;
    if (mbedtls_asn1_get_tag(&p, end, &salt.len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return malformed("PKCS#12 MAC salt");
    }
    salt.p = p;
    p += salt.len;
    if (p < end && (mbedtls_asn1_get_int(&p, end, &iterations) != 0 || iterations < 1 || iterations > 10000000)) {
        return malformed("PKCS#12 MAC iterations");
    }
    if (OID_IS(alg, kOidPbmac1)) {
        ucxapp_print("ERROR: PBMAC1 integrity (openssl -pbmac1_pbkdf2) is not supported; re-export without it\n");
        return false;
    }
    if (OID_IS(alg, kOidSha1)) {
        md = MBEDTLS_MD_SHA1;
    } else if (alg.len == sizeof(kOidSha2) + 1 && memcmp(alg.p, kOidSha2, sizeof(kOidSha2)) == 0 &&
               alg.p[sizeof(kOidSha2)] >= 1 && alg.p[sizeof(kOidSha2)] <= 4) {
        static const mbedtls_md_type_t kSha2[] = { MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA384, MBEDTLS_MD_SHA512,
                                                   MBEDTLS_MD_SHA224 };
        md = kSha2[alg.p[sizeof(kOidSha2)] - 1];
    } else {
        ucxapp_print("ERROR: unsupported PKCS#12 MAC hash\n");
        return false;
    }
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md);
    if (info == NULL || mbedtls_md_get_size(info) != digest.len) {
        return malformed("PKCS#12 MAC");
    }

    size_t bmpLen = 0;
    uint8_t *bmp = bmpPassword(pw != NULL ? pw : "", &bmpLen);
    bool match = false;
    /* An empty password is a lone 00 00 for some tools and no bytes at all for others. */
    for (int attempt = 0; bmp != NULL && !match && attempt < (bmpLen == 2 ? 2 : 1); attempt++) {
        uint8_t key[64], mac[64];
        uint8_t diff = 0;
        if (p12Kdf(md, bmp, attempt == 0 ? bmpLen : 0, salt.p, salt.len, iterations, 3, key) &&
            mbedtls_md_hmac(info, key, digest.len, content, clen, mac) == 0) {
            for (size_t i = 0; i < digest.len; i++) {
                diff |= (uint8_t)(mac[i] ^ digest.p[i]);
            }
            match = diff == 0;
        }
        mbedtls_platform_zeroize(key, sizeof(key));
    }
    if (bmp != NULL) {
        mbedtls_platform_zeroize(bmp, bmpLen);
        free(bmp);
    }
    if (!match) {
        ucxapp_print("ERROR: wrong password (the PKCS#12 integrity check failed)\n");
    }
    return match;
}

static bool parseSafeContents(bundle_t *b, unsigned char *p, const unsigned char *end, int depth)
{
    size_t len;

    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) {
        return malformed("PKCS#12 SafeContents");
    }
    const unsigned char *listEnd = p + len;
    while (p < listEnd) {
        mbedtls_asn1_buf bagId;
        if (mbedtls_asn1_get_tag(&p, listEnd, &len, SEQ) != 0) {
            return malformed("PKCS#12 bag");
        }
        unsigned char *bagEnd = p + len;
        if (getOid(&p, bagEnd, &bagId) != 0 || mbedtls_asn1_get_tag(&p, bagEnd, &len, EXPLICIT0) != 0) {
            return malformed("PKCS#12 bag");
        }
        const unsigned char *valueEnd = p + len;
        if (OID_IS(bagId, kOidKeyBag)) {
            if (!addKey(b, p, len, NULL)) {
                return false;
            }
        } else if (OID_IS(bagId, kOidShroudedKeyBag)) {
            mbedtls_asn1_buf alg, params;
            size_t dataLen, plainLen = 0;
            if (mbedtls_asn1_get_tag(&p, valueEnd, &len, SEQ) != 0 ||
                mbedtls_asn1_get_alg(&p, valueEnd, &alg, &params) != 0 ||
                mbedtls_asn1_get_tag(&p, valueEnd, &dataLen, MBEDTLS_ASN1_OCTET_STRING) != 0) {
                return malformed("PKCS#12 key bag");
            }
            uint8_t *plain = decrypt(&alg, &params, p, dataLen, b->pw, &plainLen);
            if (plain == NULL) {
                return false;
            }
            bool ok = addKey(b, plain, plainLen, NULL);
            mbedtls_platform_zeroize(plain, plainLen);
            free(plain);
            if (!ok) {
                return false;
            }
        } else if (OID_IS(bagId, kOidCertBag)) {
            mbedtls_asn1_buf certId;
            if (mbedtls_asn1_get_tag(&p, valueEnd, &len, SEQ) != 0 || getOid(&p, valueEnd, &certId) != 0) {
                return malformed("PKCS#12 certificate bag");
            }
            if (OID_IS(certId, kOidX509Cert)) {
                if (mbedtls_asn1_get_tag(&p, valueEnd, &len, EXPLICIT0) != 0 ||
                    mbedtls_asn1_get_tag(&p, valueEnd, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
                    return malformed("PKCS#12 certificate bag");
                }
                addCert(b, p, len);
            }
        } else if (OID_IS(bagId, kOidSafeContentsBag) && depth < 4) {
            if (!parseSafeContents(b, p, valueEnd, depth + 1)) {
                return false;
            }
        }
        p = bagEnd;   /* CRL and secret bags are not needed */
    }
    return true;
}

static bool parsePkcs12(bundle_t *b, unsigned char *p, const unsigned char *end)
{
    size_t len;
    int version;
    mbedtls_asn1_buf oid;

    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) {
        return malformed("PKCS#12 file");
    }
    const unsigned char *pfxEnd = p + len;
    if (mbedtls_asn1_get_int(&p, pfxEnd, &version) != 0 || mbedtls_asn1_get_tag(&p, pfxEnd, &len, SEQ) != 0) {
        return malformed("PKCS#12 file");
    }
    unsigned char *authSafeEnd = p + len;
    if (getOid(&p, authSafeEnd, &oid) != 0 || !OID_IS(oid, kOidData)) {
        ucxapp_print("ERROR: PKCS#12 with public-key integrity is not supported (only password integrity)\n");
        return false;
    }
    if (mbedtls_asn1_get_tag(&p, authSafeEnd, &len, EXPLICIT0) != 0 ||
        mbedtls_asn1_get_tag(&p, authSafeEnd, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return malformed("PKCS#12 file (BER-encoded content; re-export it with OpenSSL)");
    }
    unsigned char *content = p;
    size_t contentLen = len;
    if (authSafeEnd < pfxEnd) {
        if (!verifyMac(authSafeEnd, pfxEnd, content, contentLen, b->pw)) {
            return false;
        }
    } else {
        ucxapp_print("note: the file has no integrity MAC\n");
    }

    p = content;
    const unsigned char *contentEnd = content + contentLen;
    if (mbedtls_asn1_get_tag(&p, contentEnd, &len, SEQ) != 0) {
        return malformed("PKCS#12 content");
    }
    const unsigned char *listEnd = p + len;
    while (p < listEnd) {
        if (mbedtls_asn1_get_tag(&p, listEnd, &len, SEQ) != 0) {
            return malformed("PKCS#12 content");
        }
        unsigned char *infoEnd = p + len;
        if (getOid(&p, infoEnd, &oid) != 0 || mbedtls_asn1_get_tag(&p, infoEnd, &len, EXPLICIT0) != 0) {
            return malformed("PKCS#12 content");
        }
        if (OID_IS(oid, kOidData)) {
            if (mbedtls_asn1_get_tag(&p, infoEnd, &len, MBEDTLS_ASN1_OCTET_STRING) != 0 ||
                !parseSafeContents(b, p, p + len, 0)) {
                return false;
            }
        } else if (OID_IS(oid, kOidEncryptedData)) {
            mbedtls_asn1_buf type, alg, params;
            size_t plainLen = 0;
            if (mbedtls_asn1_get_tag(&p, infoEnd, &len, SEQ) != 0 || mbedtls_asn1_get_int(&p, infoEnd, &version) != 0 ||
                mbedtls_asn1_get_tag(&p, infoEnd, &len, SEQ) != 0 || getOid(&p, infoEnd, &type) != 0 ||
                mbedtls_asn1_get_alg(&p, infoEnd, &alg, &params) != 0 ||
                mbedtls_asn1_get_tag(&p, infoEnd, &len, MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) != 0) {
                return malformed("PKCS#12 encrypted content");
            }
            uint8_t *plain = decrypt(&alg, &params, p, len, b->pw, &plainLen);
            if (plain == NULL) {
                return false;
            }
            bool ok = parseSafeContents(b, plain, plain + plainLen, 0);
            mbedtls_platform_zeroize(plain, plainLen);
            free(plain);
            if (!ok) {
                return false;
            }
        }
        p = infoEnd;
    }
    b->format = "PKCS#12";
    return true;
}

/* ---- PKCS#7 (certificates only, .p7b) ---- */

static bool parsePkcs7(bundle_t *b, unsigned char *p, const unsigned char *end)
{
    size_t len;
    int version;
    mbedtls_asn1_buf oid;

    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0 || getOid(&p, end, &oid) != 0 || !OID_IS(oid, kOidSignedData) ||
        mbedtls_asn1_get_tag(&p, end, &len, EXPLICIT0) != 0 || mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) {
        return malformed("PKCS#7 file");
    }
    const unsigned char *sdEnd = p + len;
    if (mbedtls_asn1_get_int(&p, sdEnd, &version) != 0 ||
        mbedtls_asn1_get_tag(&p, sdEnd, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) {
        return malformed("PKCS#7 file");
    }
    p += len;
    if (mbedtls_asn1_get_tag(&p, sdEnd, &len, SEQ) != 0) {
        return malformed("PKCS#7 file");
    }
    p += len;
    if (p < sdEnd && mbedtls_asn1_get_tag(&p, sdEnd, &len, EXPLICIT0) == 0) {
        const unsigned char *certsEnd = p + len;
        while (p < certsEnd) {
            unsigned char *start = p;
            if (mbedtls_asn1_get_tag(&p, certsEnd, &len, SEQ) != 0) {
                return malformed("PKCS#7 certificate list");
            }
            p += len;
            addCert(b, start, (size_t)(p - start));
        }
    }
    b->format = "PKCS#7";
    return true;
}

/* ---- PEM: any mix of certificate, key and PKCS#7 blocks (openssl pkcs12 -nodes output included) ---- */

static bool parsePem(bundle_t *b, const char *text)
{
    const char *s = text;

    b->format = "PEM";
    while ((s = strstr(s, "-----BEGIN ")) != NULL) {
        const char *label = s + 11, *labelEnd = strstr(label, "-----");
        char header[64], footer[64];
        if (labelEnd == NULL || labelEnd - label > 40) {
            return malformed("PEM block");
        }
        int ll = (int)(labelEnd - label);
        snprintf(header, sizeof(header), "-----BEGIN %.*s-----", ll, label);
        snprintf(footer, sizeof(footer), "-----END %.*s-----", ll, label);
        const char *f = strstr(labelEnd, footer);
        if (f == NULL) {
            ucxapp_print("ERROR: PEM block %s has no end line\n", header);
            return false;
        }
        const char *blockEnd = f + strlen(footer);

        if (strcmp(header, "-----BEGIN CERTIFICATE-----") == 0 || strcmp(header, "-----BEGIN X509 CERTIFICATE-----") == 0 ||
            strcmp(header, "-----BEGIN PKCS7-----") == 0) {
            mbedtls_pem_context pem;
            size_t used = 0, derLen = 0;
            mbedtls_pem_init(&pem);
            int rc = mbedtls_pem_read_buffer(&pem, header, footer, (const unsigned char *)s, NULL, 0, &used);
            if (rc != 0) {
                mbedtls_pem_free(&pem);
                return malformed("PEM block (bad base64)");
            }
            const unsigned char *der = mbedtls_pem_get_buffer(&pem, &derLen);
            bool ok = true;
            if (header[11] == 'P') {
                ok = parsePkcs7(b, (unsigned char *)der, der + derLen);
                b->format = "PEM";
            } else {
                addCert(b, der, derLen);
            }
            mbedtls_pem_free(&pem);
            if (!ok) {
                return false;
            }
        } else if (ll >= 11 && strncmp(labelEnd - 11, "PRIVATE KEY", 11) == 0) {
            size_t n = (size_t)(blockEnd - s);
            char *block = (char *)malloc(n + 1);
            if (block == NULL) {
                return false;
            }
            memcpy(block, s, n);
            block[n] = '\0';
            bool ok = addKey(b, (const unsigned char *)block, n + 1, b->pw);
            mbedtls_platform_zeroize(block, n);
            free(block);
            if (!ok) {
                return false;
            }
        } else {
            ucxapp_print("note: skipped a %.*s block\n", ll, label);
        }
        s = blockEnd;
    }
    return true;
}

static bool parseAny(bundle_t *b, uint8_t *data, size_t len)
{
    unsigned char *p = data;
    const unsigned char *end = data + len;
    size_t seqLen;
    mbedtls_asn1_buf oid;
    int version;

    if (data[0] != 0x30) {
        if (strstr((const char *)data, "-----BEGIN ") != NULL) {
            return parsePem(b, (const char *)data);
        }
        ucxapp_print("ERROR: not a certificate file (PEM, DER, PKCS#7 or PKCS#12)\n");
        return false;
    }
    if (mbedtls_asn1_get_tag(&p, end, &seqLen, SEQ) == 0) {
        unsigned char *q = p;
        if (mbedtls_asn1_get_int(&q, end, &version) == 0 && version == 3 &&
            mbedtls_asn1_get_tag(&q, end, &seqLen, SEQ) == 0 && getOid(&q, end, &oid) == 0 && OID_IS(oid, kOidData)) {
            return parsePkcs12(b, data, end);
        }
        q = p;
        if (getOid(&q, end, &oid) == 0 && OID_IS(oid, kOidSignedData)) {
            return parsePkcs7(b, data, end);
        }
    }
    if (mbedtls_x509_crt_parse_der(&b->certs, data, len) == 0) {
        b->certCount++;
        b->format = "DER certificate";
        return true;
    }
    b->format = "DER private key";
    int rc = mbedtls_pk_parse_key(&b->key, data, len, pwLen(b->pw) > 0 ? (const unsigned char *)b->pw : NULL,
                                  pwLen(b->pw));
    if (rc == MBEDTLS_ERR_PK_KEY_INVALID_FORMAT || rc == MBEDTLS_ERR_PK_UNKNOWN_PK_ALG) {
        ucxapp_print("ERROR: not a certificate file (PEM, DER, PKCS#7 or PKCS#12)\n");
        return false;
    }
    if (rc != 0) {
        mbedtls_pk_free(&b->key);
        mbedtls_pk_init(&b->key);
        return keyError(rc);
    }
    b->haveKey = true;
    return true;
}

static bool sameName(const mbedtls_x509_buf *a, const mbedtls_x509_buf *b)
{
    return a->len == b->len && memcmp(a->p, b->p, a->len) == 0;
}

static bool selfSigned(const mbedtls_x509_crt *c)
{
    return sameName(&c->subject_raw, &c->issuer_raw);
}

static bool listed(const mbedtls_x509_crt *const *list, int n, const mbedtls_x509_crt *c)
{
    for (int i = 0; i < n; i++) {
        if (list[i] == c) {
            return true;
        }
    }
    return false;
}

/* With a key: the certificate that matches it, then its issuers up to (not including) a root; the rest are CAs.
   Without a key every certificate goes in the chain list. */
static void classify(bundle_t *b)
{
    const mbedtls_x509_crt *leaf = NULL;

    b->chainCount = b->caCount = 0;
    if (b->haveKey) {
        /* Compare the encoded public keys; the DER is written at the end of the buffer. */
        unsigned char keyDer[2600], certDer[2600];
        int keyLen = mbedtls_pk_write_pubkey_der(&b->key, keyDer, sizeof(keyDer));
        for (const mbedtls_x509_crt *c = &b->certs; keyLen > 0 && c != NULL && c->raw.len > 0; c = c->next) {
            int certLen = mbedtls_pk_write_pubkey_der(&c->pk, certDer, sizeof(certDer));
            if (certLen == keyLen &&
                memcmp(keyDer + sizeof(keyDer) - keyLen, certDer + sizeof(certDer) - certLen, (size_t)keyLen) == 0) {
                leaf = c;
                break;
            }
        }
        if (leaf == NULL && b->certCount > 0) {
            ucxapp_print("warning: no certificate matches the private key\n");
        }
    }
    if (leaf != NULL) {
        b->chain[b->chainCount++] = leaf;
        for (const mbedtls_x509_crt *cur = leaf; !selfSigned(cur) && b->chainCount < MAX_CERTS;) {
            const mbedtls_x509_crt *parent = NULL;
            for (const mbedtls_x509_crt *c = &b->certs; c != NULL && c->raw.len > 0; c = c->next) {
                if (!listed(b->chain, b->chainCount, c) && sameName(&c->subject_raw, &cur->issuer_raw)) {
                    parent = c;
                    break;
                }
            }
            if (parent == NULL || selfSigned(parent)) {
                break;
            }
            b->chain[b->chainCount++] = parent;
            cur = parent;
        }
    }
    for (const mbedtls_x509_crt *c = &b->certs; c != NULL && c->raw.len > 0; c = c->next) {
        if (listed(b->chain, b->chainCount, c)) {
            continue;
        }
        if (leaf != NULL && b->caCount < MAX_CERTS) {
            b->cas[b->caCount++] = c;
        } else if (leaf == NULL && b->chainCount < MAX_CERTS) {
            b->chain[b->chainCount++] = c;
        } else {
            ucxapp_print("warning: more than %d certificates; the rest are left out\n", MAX_CERTS);
            break;
        }
    }
}

static void bundleFree(bundle_t *b)
{
    mbedtls_x509_crt_free(&b->certs);
    mbedtls_pk_free(&b->key);
}

static bool bundleLoad(bundle_t *b, const char *file, const char *pw)
{
    size_t len = 0;

    memset(b, 0, sizeof(*b));
    mbedtls_x509_crt_init(&b->certs);
    mbedtls_pk_init(&b->key);
    b->pw = pw;
    if (psa_crypto_init() != PSA_SUCCESS) {
        ucxapp_print("ERROR: crypto init failed\n");
        return false;
    }
    uint8_t *data = ucxapp_store_load(file, &len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", file);
        return false;
    }
    bool ok = len > 0 && parseAny(b, data, len);
    if (len == 0) {
        ucxapp_print("ERROR: %s is empty\n", file);
    }
    mbedtls_platform_zeroize(data, len);
    free(data);
    if (ok && b->certCount == 0 && !b->haveKey) {
        ucxapp_print("ERROR: no certificate or private key in %s\n", file);
        ok = false;
    }
    if (ok) {
        classify(b);
    }
    return ok;
}

/* Concatenated PEM of the certificates (malloc'ed, NUL-terminated). */
static char *certsPem(const mbedtls_x509_crt *const *list, int n, size_t *outLen)
{
    size_t size = 1, used = 0;
    for (int i = 0; i < n; i++) {
        size += list[i]->raw.len * 2 + 128;
    }
    char *out = (char *)malloc(size);
    if (out == NULL) {
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        size_t olen = 0;
        if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n", list[i]->raw.p,
                                     list[i]->raw.len, (unsigned char *)out + used, size - used, &olen) != 0) {
            free(out);
            return NULL;
        }
        used += olen - 1;   /* olen counts the NUL */
    }
    out[used] = '\0';
    *outLen = used;
    return out;
}

static char *keyPem(bundle_t *b, size_t *outLen)
{
    char *out = (char *)malloc(KEY_PEM_MAX);
    if (out != NULL && mbedtls_pk_write_key_pem(&b->key, (unsigned char *)out, KEY_PEM_MAX) != 0) {
        free(out);
        out = NULL;
    }
    if (out != NULL) {
        *outLen = strlen(out);
    }
    return out;
}

/* File name without folder and extension. */
static void stem(const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\') {
            base = s + 1;
        }
    }
    snprintf(out, size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot != NULL && dot != out) {
        *dot = '\0';
    }
}

static void printCert(int index, const mbedtls_x509_crt *c, const char *role)
{
    char name[256];

    mbedtls_x509_dn_gets(name, sizeof(name), &c->subject);
    ucxapp_print("cert %d   %s%s%s\n", index, name, role[0] ? "  - " : "", role);
    mbedtls_x509_dn_gets(name, sizeof(name), &c->issuer);
    ucxapp_print("  issuer  %s%s\n", name, selfSigned(c) ? " (self-signed)" : "");
    ucxapp_print("  valid   %04d-%02d-%02d .. %04d-%02d-%02d%s\n", c->valid_from.year, c->valid_from.mon,
                 c->valid_from.day, c->valid_to.year, c->valid_to.mon, c->valid_to.day,
#if defined(MBEDTLS_HAVE_TIME_DATE)
                 mbedtls_x509_time_is_past(&c->valid_to) ? " EXPIRED" : ""
#else
                 ""
#endif
    );
}

static void printKey(bundle_t *b)
{
    mbedtls_pk_type_t type = mbedtls_pk_get_type(&b->key);
    ucxapp_print("key      %s %u bits\n", type == MBEDTLS_PK_RSA ? "RSA" : type == MBEDTLS_PK_ECKEY ? "EC" : "other",
                 (unsigned)mbedtls_pk_get_bitlen(&b->key));
}

static int cmdInfo(ucxapp_t *app, int argc, char *argv[])
{
    bundle_t b;
    (void)app;

    if (argc < 2 || argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (!bundleLoad(&b, argv[1], argc > 2 ? argv[2] : NULL)) {
        bundleFree(&b);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("format   %s, %d certificate%s%s\n", b.format, b.certCount, b.certCount == 1 ? "" : "s",
                 b.haveKey ? ", private key" : "");
    int n = 0;
    for (int i = 0; i < b.chainCount; i++) {
        printCert(++n, b.chain[i], !b.haveKey ? "" : i == 0 ? "client, matches the key" : "intermediate");
    }
    for (int i = 0; i < b.caCount; i++) {
        printCert(++n, b.cas[i], "CA");
    }
    if (b.haveKey) {
        printKey(&b);
    }
    bundleFree(&b);
    return UCXAPP_OK;
}

static bool saveFile(const char *name, const char *data, size_t len, const char *what)
{
    if (!ucxapp_store_save(name, data, len)) {
        ucxapp_print("ERROR: cannot write %s\n", name);
        return false;
    }
    ucxapp_print("wrote %-24s %s, %u bytes%s\n", name, what, (unsigned)len,
                 len > UPLOAD_MAX ? " (too big for cert.upload)" : "");
    return true;
}

static int cmdConvert(ucxapp_t *app, int argc, char *argv[])
{
    bundle_t b;
    char out[96], name[112];
    char *pem;
    size_t len = 0;
    bool ok = true;
    (void)app;

    if (argc < 2 || argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 2 && argv[2][0] != '\0') {
        snprintf(out, sizeof(out), "%s", argv[2]);
    } else {
        stem(argv[1], out, sizeof(out));
    }
    if (!bundleLoad(&b, argv[1], argc > 3 ? argv[3] : NULL)) {
        bundleFree(&b);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s: %d certificate%s%s\n", b.format, b.certCount, b.certCount == 1 ? "" : "s",
                 b.haveKey ? " and a private key" : "");
    if (ok && b.chainCount > 0 && (pem = certsPem(b.chain, b.chainCount, &len)) != NULL) {
        snprintf(name, sizeof(name), "%s.crt", out);
        ok = saveFile(name, pem, len, b.haveKey ? "client certificate chain" : "certificates");
        free(pem);
    }
    if (ok && b.haveKey && (pem = keyPem(&b, &len)) != NULL) {
        snprintf(name, sizeof(name), "%s.key", out);
        ok = saveFile(name, pem, len, "private key (not encrypted)");
        mbedtls_platform_zeroize(pem, len);
        free(pem);
    }
    if (ok && b.caCount > 0 && (pem = certsPem(b.cas, b.caCount, &len)) != NULL) {
        snprintf(name, sizeof(name), "%s-ca.crt", out);
        ok = saveFile(name, pem, len, "CA certificates");
        free(pem);
    }
    if (ok) {
        ucxapp_print("upload all of it with: cert.import %s\n", argv[1]);
    }
    bundleFree(&b);
    return ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* pem and len come from certsPem/keyPem, called before this so that len is set. */
static int uploadPem(ucxapp_t *app, int type, const char *name, char *pem, size_t len)
{
    int rc = pem != NULL ? ucxapp_cert_upload(app, type, name, (const uint8_t *)pem, len, NULL) : UCXAPP_ERR_FAILED;
    if (pem != NULL) {
        mbedtls_platform_zeroize(pem, len);
        free(pem);
    }
    return rc;
}

static int cmdImport(ucxapp_t *app, int argc, char *argv[])
{
    bundle_t b;
    char base[64], name[sizeof(base) + 8];
    char *pem;
    size_t len = 0;
    int rc = UCXAPP_OK;

    if (argc < 2 || argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 2 && argv[2][0] != '\0') {
        snprintf(base, sizeof(base), "%s", argv[2]);
    } else {
        stem(argv[1], base, sizeof(base));
    }
    if (strlen(base) + 4 > MODULE_NAME_MAX) {
        ucxapp_print("ERROR: name '%s' is too long (max %d characters, so that %s-key fits)\n", base,
                     MODULE_NAME_MAX - 4, base);
        return UCXAPP_ERR_FAILED;
    }
    if (!bundleLoad(&b, argv[1], argc > 3 ? argv[3] : NULL)) {
        bundleFree(&b);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s: %d certificate%s%s\n", b.format, b.certCount, b.certCount == 1 ? "" : "s",
                 b.haveKey ? " and a private key" : "");
    if (b.chainCount > 0) {
        pem = certsPem(b.chain, b.chainCount, &len);
        rc = uploadPem(app, b.haveKey ? 1 : 0, base, pem, len);
    }
    if (rc == UCXAPP_OK && b.haveKey) {
        snprintf(name, sizeof(name), "%s-key", base);
        pem = keyPem(&b, &len);
        rc = uploadPem(app, 2, name, pem, len);
    }
    if (rc == UCXAPP_OK && b.caCount > 0) {
        snprintf(name, sizeof(name), "%s-ca", base);
        pem = certsPem(b.cas, b.caCount, &len);
        rc = uploadPem(app, 0, name, pem, len);
    }
    if (rc == UCXAPP_OK && b.haveKey && b.chainCount > 0) {
        ucxapp_print("client certificate %s, key %s-key%s%s\n", base, base, b.caCount > 0 ? ", CA " : "",
                     b.caCount > 0 ? name : "");
    }
    bundleFree(&b);
    return rc;
}

static const ucxapp_cmd_t gCertFileCmds[] = {
    { "utils.cert.info",    "<file> [pw]",         "What a certificate file holds: PKCS#12 (.p12/.pfx), PKCS#7 (.p7b), DER (.cer/.der) or PEM", 0, cmdInfo },
    { "utils.cert.convert", "<file> [out] [pw]",   "Convert PKCS#12, PKCS#7, DER or PEM to the PEM files the module takes: <out>.crt, <out>.key, <out>-ca.crt", 0, cmdConvert },
    { "cert.import",        "<file> [name] [pw]",  "Convert PKCS#12, PKCS#7, DER or PEM and upload it: client chain <name>, key <name>-key, CA <name>-ca", UCXAPP_CMD_NEEDS_DEVICE, cmdImport },
};

void ucxapp_register_certfile_cmds(void)
{
    ucxapp_cmd_register(gCertFileCmds, sizeof(gCertFileCmds) / sizeof(gCertFileCmds[0]));
}
