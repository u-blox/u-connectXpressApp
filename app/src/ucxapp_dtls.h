#ifndef UCXAPP_DTLS_H
#define UCXAPP_DTLS_H

/* DTLS 1.2 (mbedTLS) run on the host over a datagram transport. The transport is either a module UDP
 * socket (uCxSocket*) or an in-process pipe (selftest). Host only: not built for UCXAPP_LITE targets. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCXAPP_DTLS_MAX_DGRAM 1500
#define UCXAPP_DTLS_MTU       1000   /* smallest AT+USORFB limit across NORA-W36 firmware */

/* One datagram per call. recv returns 0 when nothing is pending, the byte count otherwise, < 0 on error. */
typedef struct {
    int (*send)(void *ctx, const unsigned char *buf, size_t len);
    int (*recv)(void *ctx, unsigned char *buf, size_t len);
    void *ctx;
} ucxapp_dtls_xport_t;

typedef struct ucxapp_dtls_conn ucxapp_dtls_conn_t;

/** Client: verifies the server certificate against tls-<caName>.crt; hostname is checked against the certificate. */
ucxapp_dtls_conn_t *ucxapp_dtls_client_new(ucxapp_dtls_xport_t *x, const char *caName, const char *hostname,
                                           char *err, size_t errLen);

/** Server: presents tls-<certName>.crt/.key. peerId identifies the client (HelloVerifyRequest cookie binding);
 *  the caller passes the first datagram's source address through the transport and keeps the same peerId. */
ucxapp_dtls_conn_t *ucxapp_dtls_server_new(ucxapp_dtls_xport_t *x, const char *certName,
                                           const unsigned char *peerId, size_t peerLen, char *err, size_t errLen);

/** 1 = established, 0 = in progress, < 0 = failed. Never blocks. */
int ucxapp_dtls_handshake(ucxapp_dtls_conn_t *c);

/** Decrypted bytes (> 0), 0 when nothing is pending, < 0 on error or close. */
int32_t ucxapp_dtls_read(ucxapp_dtls_conn_t *c, uint8_t *buf, size_t len);

/** Writes one application record (< 0 on failure). */
int32_t ucxapp_dtls_write(ucxapp_dtls_conn_t *c, const uint8_t *data, size_t len);

/** Cipher suite name after the handshake ("" before). */
const char *ucxapp_dtls_cipher(const ucxapp_dtls_conn_t *c);

void ucxapp_dtls_free(ucxapp_dtls_conn_t *c);

/** Runs a client and a server in-process over a memory datagram pair (server cert tls-<serverCert>). */
bool ucxapp_dtls_selftest(const char *serverCert, char *out, size_t outLen);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_DTLS_H */
