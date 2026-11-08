#ifndef UCXAPP_TLS_H
#define UCXAPP_TLS_H

/* TLS (mbedTLS) terminated in the app on top of module TCP sockets, plus a small local CA.
 * Certificates and keys live in the app store as tls-<name>.crt / tls-<name>.key (PEM). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u_cx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCXAPP_TLS_CA_NAME "ca"

/** Fills buf from the cryptographic random generator (OS entropy). */
bool ucxapp_tls_random(void *buf, size_t len);

/** Creates the local CA (tls-ca.crt/.key). Fails if it exists unless force. */
bool ucxapp_tls_create_ca(const char *commonName, bool force, char *err, size_t errLen);

/** Creates tls-<name>.crt/.key signed by the local CA. `sans` are DNS names or IPv4 addresses
 *  (server certificates; the first one is also the common name). */
bool ucxapp_tls_create_cert(const char *name, bool client, const char *const *sans, int sanCount,
                            char *err, size_t errLen);

/** One-line summary (subject, issuer, validity) of a stored certificate. */
bool ucxapp_tls_describe(const char *name, char *out, size_t outLen);

/** Store file names for a certificate name. */
void ucxapp_tls_cert_file(const char *name, char *out, size_t outLen);
void ucxapp_tls_key_file(const char *name, char *out, size_t outLen);

#define UCXAPP_TLS_WEB_NAME "web"

/** Makes sure tls-web.crt/.key exist for `ip` (its common name): creates the local CA when there is none and
 *  (re)issues the certificate when it is missing or was made for another address. *created tells which. */
bool ucxapp_tls_web_cert(const char *ip, bool *created, char *err, size_t errLen);

typedef struct ucxapp_tls_server ucxapp_tls_server_t;
typedef struct ucxapp_tls_conn ucxapp_tls_conn_t;

/** Loads tls-<certName>.crt/.key. requireClientCert: clients must present a certificate signed by the local CA. */
ucxapp_tls_server_t *ucxapp_tls_server_new(const char *certName, bool requireClientCert, char *err, size_t errLen);
void ucxapp_tls_server_free(ucxapp_tls_server_t *srv);

/** Starts TLS on an accepted module socket. The socket itself stays owned by the caller. */
ucxapp_tls_conn_t *ucxapp_tls_accept(ucxapp_tls_server_t *srv, uCxHandle_t *ucx, int32_t sock);

/** Drives the handshake: 1 = established, 0 = still in progress, < 0 = failed. Never blocks on data. */
int ucxapp_tls_handshake(ucxapp_tls_conn_t *c);

/** Decrypted bytes (> 0), 0 when nothing is available, < 0 when the peer closed or on error. */
int32_t ucxapp_tls_read(ucxapp_tls_conn_t *c, uint8_t *buf, size_t len);

/** Writes everything or fails (< 0). */
int32_t ucxapp_tls_write(ucxapp_tls_conn_t *c, const uint8_t *data, size_t len);

/** Peer certificate common name after a client-authenticated handshake ("" otherwise). */
const char *ucxapp_tls_peer(const ucxapp_tls_conn_t *c);

/** Sends close_notify (when sendClose) and frees the connection; does not close the socket. */
void ucxapp_tls_close(ucxapp_tls_conn_t *c, bool sendClose);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_TLS_H */
