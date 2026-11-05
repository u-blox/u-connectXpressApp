#ifndef UCXAPP_STREAM_H
#define UCXAPP_STREAM_H

/* Serial sensor stream (host only): a reader thread feeds a protocol parser, optionally logs the raw bytes and
 * can bridge them through the connected module (TCP server, UDP, BLE Nordic UART). Shared by gnss.* and imu.*. */

#include "ucxapp.h"

typedef struct ucxapp_stream ucxapp_stream_t;

typedef struct {
    const char *kind;                                         /* "gnss", "imu" (names bridge messages) */
    void (*feed)(void *ctx, const uint8_t *data, size_t n);   /* called with the stream lock held */
    void (*poll)(void *ctx, ucxapp_stream_t *s);              /* optional, every pollMs, lock not held */
    uint32_t pollMs;
    void *ctx;
    /* Virtual streams (port "virt:...") have no UART; bytes written to them arrive here. */
    bool (*write)(void *ctx, const uint8_t *data, size_t n);
} ucxapp_stream_cfg_t;

ucxapp_stream_t *ucxapp_stream_open(const char *port, int32_t baud, const ucxapp_stream_cfg_t *cfg);
void ucxapp_stream_close(ucxapp_stream_t *s);   /* also stops a running bridge */
void ucxapp_stream_lock(ucxapp_stream_t *s);
void ucxapp_stream_unlock(ucxapp_stream_t *s);
bool ucxapp_stream_write(ucxapp_stream_t *s, const uint8_t *data, size_t len);
/* Data produced by a virtual stream's owner, handled like bytes read from a UART. */
void ucxapp_stream_push(ucxapp_stream_t *s, const uint8_t *data, size_t len);
const char *ucxapp_stream_port(const ucxapp_stream_t *s);
int32_t ucxapp_stream_baud(const ucxapp_stream_t *s);
uint32_t ucxapp_stream_rx_bytes(const ucxapp_stream_t *s);

/* Raw capture of everything received; NULL file name stops. */
bool ucxapp_stream_log(ucxapp_stream_t *s, const char *file);
const char *ucxapp_stream_log_name(const ucxapp_stream_t *s);   /* "" when off */

/* Bridges: the raw byte stream is forwarded through the module. Up to UCXAPP_STREAM_MAX_BRIDGES run side by side,
   each with its own buffer; at most one BLE bridge and one NTRIP client per stream. */
#define UCXAPP_STREAM_MAX_BRIDGES 4
bool ucxapp_stream_bridge_tcp(ucxapp_stream_t *s, ucxapp_t *app, int32_t port);
/* TCP server with TLS terminated in the app (certificate tls-<certName>.crt); clientAuth requires a client certificate from the local CA. */
bool ucxapp_stream_bridge_tls(ucxapp_stream_t *s, ucxapp_t *app, int32_t port, const char *certName, bool clientAuth);
bool ucxapp_stream_bridge_udp(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, int32_t local);
/* TCP client that reconnects when the link drops. */
bool ucxapp_stream_bridge_tcp_connect(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port);
/* NTRIP v1 client: corrections from the caster go to the device, its latest GGA goes up every ggaSeconds;
 * ble also sends the device stream to a BLE Nordic UART client (RTK rover with a phone). */
bool ucxapp_stream_bridge_ntrip(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, const char *mount,
                                const char *user, const char *pass, int32_t ggaSeconds, bool ble);
/* The same NTRIP client over a serial data link that is already connected to the caster (cellular modem). */
bool ucxapp_stream_bridge_ntrip_uart(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud,
                                     const char *mount, const char *user, const char *pass, int32_t ggaSeconds,
                                     bool ble);
/* MQTT subscriber over TLS (client certificate certName, key certName-key, broker CA certName-ca): every message on
 * the topics goes to the device unchanged, e.g. PointPerfect SPARTN corrections and keys. One per app. */
bool ucxapp_stream_bridge_mqtt(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, const char *clientId,
                               const char *certName, const char *const *topics, int nTopics);
/* u-blox Serial Port Service over BLE. */
bool ucxapp_stream_bridge_sps(ucxapp_stream_t *s, ucxapp_t *app);
bool ucxapp_stream_bridge_ble(ucxapp_stream_t *s, ucxapp_t *app);
/* Own 128-bit service with one write (rx) and one notify (tx) characteristic. */
bool ucxapp_stream_bridge_ble_custom(ucxapp_stream_t *s, ucxapp_t *app, const uint8_t svc[16], const uint8_t rx[16],
                                     const uint8_t tx[16]);
/* Second PC serial port, both directions, no module needed. */
bool ucxapp_stream_bridge_uart(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud);
/* Shared "bridge tcp|udp|ble|uart|off [n]|status" handler; argv[0] is the command name. */
int ucxapp_stream_bridge_cmd(ucxapp_stream_t *s, ucxapp_t *app, int argc, char *argv[], int32_t defaultPort);
void ucxapp_stream_bridge_stop(ucxapp_stream_t *s);   /* every bridge */
bool ucxapp_stream_bridge_stop_one(ucxapp_stream_t *s, int n);   /* bridge n (1-based); false if there is none */
int ucxapp_stream_bridge_count(const ucxapp_stream_t *s);
int ucxapp_stream_bridge_stop_port(const char *port);   /* every bridge (of any source) that writes to a UART port */
/* "bridge: off" or one "bridge N: ..." line per bridge; lines after the first start with indent. */
void ucxapp_stream_bridge_status(ucxapp_stream_t *s, const char *indent);
bool ucxapp_stream_bridge_active(const ucxapp_stream_t *s);
/* Port of the open stream of this kind ("gnss", "imu", "cell"), or NULL. */
const char *ucxapp_stream_open_port(const char *kind);

/* Socket URCs for the TCP bridge (queue only); called from ucxapp_urc.c. */
void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock);
void ucxapp_stream_on_closed(int32_t sock);
void ucxapp_stream_on_data(int32_t sock);
void ucxapp_stream_on_sps_data(void);
void ucxapp_stream_on_ble_rx(const uint8_t *data, size_t len);

/* Runs probe(port, result) on every port at once, one thread each, and waits for all of them.
 * results[i] is resultSize bytes apart; hits[i] is what probe returned. */
void ucxapp_stream_probe_ports(const char (*ports)[32], int count, bool (*probe)(const char *port, void *result),
                               void *results, size_t resultSize, bool *hits);

#endif /* UCXAPP_STREAM_H */
