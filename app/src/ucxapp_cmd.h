#ifndef UCXAPP_CMD_H
#define UCXAPP_CMD_H

#include "ucxapp.h"
#include "ucxapp_platform.h"
#include "u_cx_socket.h"

#define UCXAPP_OK                  0
#define UCXAPP_ERR_UNKNOWN_CMD   (-100)
#define UCXAPP_ERR_USAGE         (-101)
#define UCXAPP_ERR_NOT_CONNECTED (-102)
#define UCXAPP_ERR_FAILED        (-103)
#define UCXAPP_ERR_UNSUPPORTED   (-104)

#define UCXAPP_MAX_ARGS    8
#define UCXAPP_MAX_PACKS   40

/* Command needs a connected module (app->ucx != NULL). */
#define UCXAPP_CMD_NEEDS_DEVICE  0x01
/* Command changes or erases module/host state irreversibly (firmware, factory reset, removals). */
#define UCXAPP_CMD_DANGER        0x02

/* argv[0] is the command name. Return UCXAPP_OK, UCXAPP_ERR_USAGE or another negative code. */
typedef int (*ucxapp_cmd_fn_t)(ucxapp_t *app, int argc, char *argv[]);

typedef struct {
    const char *name;     /* "wifi.connect" */
    const char *usage;    /* "<ssid> [password]" */
    const char *help;     /* one line */
    uint8_t flags;
    ucxapp_cmd_fn_t run;
} ucxapp_cmd_t;

/* Register a command table (a "pack"). The table must stay valid for the program lifetime. */
bool ucxapp_cmd_register(const ucxapp_cmd_t *table, size_t count);

/* Tokenize a line in place (whitespace separated, "double quotes" group) and run it. */
int ucxapp_cmd_exec(ucxapp_t *app, char *line);

/* Called before (done false) and after (done true, with rc) every command; argv is as typed. NULL removes it. */
typedef void (*ucxapp_recorder_fn_t)(const ucxapp_cmd_t *cmd, int argc, char *argv[], bool done, int rc);
void ucxapp_cmd_set_recorder(ucxapp_recorder_fn_t fn);

void ucxapp_cmd_print_help(const char *prefix);

/* Usage split into one top-level alternative per line, each prefixed by indent. */
void ucxapp_print_usage_lines(const char *indent, const char *usage);

/* Every command as a JSON array (name, usage, help, needsDevice, danger); "help --json". */
void ucxapp_cmd_print_json(const char *prefix);

/* JSON string literal of s, with quotes and escapes. */
void ucxapp_print_json_string(const char *s);

/* Stable name of a command result: "OK", "USAGE", "NOT_CONNECTED", "UNKNOWN_CMD", "UNSUPPORTED", "FAILED". */
const char *ucxapp_err_code(int rc);

/* Module error code of the last command (the last one passed to ucxapp_errname), 0 if none. */
int32_t ucxapp_last_module_error(void);

/* Forget the cached module model; call after (re)connecting. */
void ucxapp_caps_reset(void);

/* One aligned memory row, same look for module stacks and app memory:
   "<kind>:<name>  <sizeLabel>: <size>  used: <used>  usage(%): <pct>". size 0 prints "?". */
void ucxapp_print_usage_row(const char *kind, const char *name, const char *sizeLabel,
                            uint64_t size, uint64_t used);

/* Copy in to out with ${NAME} replaced by the environment variable; false (message printed) if one is unset
   or out is too small. Only for local input (--exec, shell), never for web commands. */
bool ucxapp_expand_env(const char *in, char *out, size_t size);

/* Visit every registered command in registration order. */
void ucxapp_cmd_foreach(void (*visit)(const ucxapp_cmd_t *cmd, void *user), void *user);

/* Longest common completion of prefix into out; returns the number of matching commands. */
int ucxapp_cmd_complete(const char *prefix, char *out, size_t size);

/* Exact name lookup; NULL if unknown. */
const ucxapp_cmd_t *ucxapp_cmd_find(const char *name);

/** False for Bluetooth LE only modules (NORA-B26); the Wi-Fi command groups are blocked on them. */
bool ucxapp_module_has_wifi(ucxapp_t *app);

/* Worked example number index (lines are '\n'-separated, '#' lines are notes); false past the end. */
bool ucxapp_example_get(size_t index, const char **group, const char **title, const char **lines);

/* Read-eval loop on the IO layer. Returns on "exit"/"quit" or end of input. */
void ucxapp_shell_run(ucxapp_t *app);

/* ": U_ERROR_COMMON_TIMEOUT" for a known u-connectXpress error code, else ""; meant to follow a printed %d. */
const char *ucxapp_errname(int32_t code);

/* Parse a whole decimal integer; false if s is empty or has trailing characters. */
bool ucxapp_parse_int(const char *s, int32_t *out);

/* Open sockets and BLE connections; the newest is "current" and is used when a command omits its handle. */
typedef enum { UCXAPP_HANDLE_SOCK = 0, UCXAPP_HANDLE_CONN = 1 } ucxapp_handle_kind_t;
void ucxapp_handle_add(ucxapp_handle_kind_t kind, int32_t id);
void ucxapp_handle_remove(ucxapp_handle_kind_t kind, int32_t id);
int32_t ucxapp_handle_current(ucxapp_handle_kind_t kind);   /* -1 if none */
uint32_t ucxapp_handle_version(void);                       /* changes whenever a handle is added or removed */
int ucxapp_handle_list(ucxapp_handle_kind_t kind, int32_t *out, int max);   /* oldest first; returns the count */

/* Command packs */
void ucxapp_register_device_cmds(void);

/** Free NORA EVK AT ports found on this host; returns the count written to names. */
int ucxapp_find_modules(char names[][32], int maxNames);
/* Serial port of the active module, "" when none (host builds). */
const char *ucxapp_device_port(void);
/* True for ports a detector should not probe: virtual, Bluetooth, labelled, unavailable or NORA ports. */
bool ucxapp_port_skip(const ucxapp_serial_port_t *sp);
/* True when USB ID or description say u-blox GNSS (VID 1546, "GNSS", "F9P", "ZED-", ...). */
bool ucxapp_port_is_gnss(const ucxapp_serial_port_t *sp);
/* Last port/baud where a device kind ("gnss", "imu", "cell") was found, so detection can try it first. */
bool ucxapp_last_port_load(const char *kind, char *port, size_t size, int32_t *baud);
void ucxapp_last_port_save(const char *kind, const char *port, int32_t baud);
void ucxapp_register_wifi_cmds(void);
void ucxapp_register_sock_cmds(void);
void ucxapp_register_dtls_cmds(void);    /* dtls.* (host only, empty in UCXAPP_LITE builds) */
void ucxapp_register_coap_cmds(void);    /* coap.* (host only, empty in UCXAPP_LITE builds) */
void ucxapp_register_tftp_cmds(void);    /* tftp.* (host only, empty in UCXAPP_LITE builds) */
void ucxapp_register_ft_cmds(void);      /* ft.* BLE file transfer (host only) */
void ucxapp_register_ancs_cmds(void);    /* ancs.* iPhone notifications (host only) */
void ucxapp_register_notify_cmds(void);  /* notify.* phone notifications over a custom GATT service (host only) */
void ucxapp_register_session_cmds(void); /* session.* record/replay and prod.test (host only) */
void ucxapp_session_end(void);           /* close a running session.record at exit */
void ucxapp_register_time_cmds(void);    /* app.time */
void ucxapp_register_can_cmds(void);     /* can.* (host only, empty in UCXAPP_LITE builds) */
void ucxapp_register_net_cmds(void);
void ucxapp_register_sys_cmds(void);
void ucxapp_register_app_cmds(void);     /* app.* (ucxapp_cmds_app.c) */
bool ucxappLogEventsEnabled(void);   /* app.log.events.on: URC events are also printed to the log */
void ucxapp_register_mqtt_cmds(void);
void ucxapp_register_bt_cmds(void);
void ucxapp_register_cert_cmds(void);
void ucxapp_register_tls_cmds(void);     /* tls.* local CA and certificates (ucxapp_cmds_tls.c) */
void ucxapp_register_fw_cmds(void);
void ucxapp_register_ymodem_cmds(void);  /* file.send, file.recv over the console (not in UCXAPP_LITE builds) */
void ucxapp_register_gatt_cmds(void);
void ucxapp_register_qr_cmds(void);
void ucxapp_register_gnss_cmds(void);    /* gnss.* (ucxapp_cmds_gnss.c) */
void ucxapp_register_imu_cmds(void);     /* imu.* (ucxapp_cmds_imu.c) */
void ucxapp_register_bridge_cmds(void);  /* bridge.list, bridge.off over all sources (ucxapp_stream.c) */
void ucxapp_register_hw_cmds(void);      /* hw.* GPIO/I2C/SPI/ADC/PWM/CAN (ucxapp_cmds_hw.c) */
void ucxapp_register_cell_cmds(void);    /* cell.* (ucxapp_cmds_cell.c) */
void ucxapp_register_dash_cmds(void);    /* dash (ucxapp_cmds_dash.c, host only) */
void ucxapp_register_utils_cmds(void);   /* utils.zip, utils.unzip (ucxapp_cmds_utils.c) */

/* Built-in zip extraction (stored and deflate); prints each file. */
bool ucxapp_unzip(const char *zipPath, const char *dir);
void ucxapp_register_hid_cmds(void);
void ucxapp_register_web_cmds(void);
void ucxapp_register_examples_cmds(void);   /* <group>.ex and examples [group]; register last */

/* Reboots the module and restores the session settings (dev.reboot). */
bool ucxapp_module_reboot(ucxapp_t *app);

/* AT+USORFB that adapts to the module's read limit (FW 3.2 allows 1000 bytes, newer firmware 1460). */
int32_t ucxapp_udp_recvfrom(uCxHandle_t *ucx, int32_t sock, int32_t len, uint8_t *buf, uCxSocketReceiveFrom_t *from);
/* Largest datagram the module can read (1460, or 1000 once a FW 3.2 module was seen). */
int32_t ucxapp_udp_max(void);

/* Raw AT: ucxapp_at_send runs one command (AT prefix optional); in AT mode ucxapp_cmd_exec sends every line, 'back' leaves it. */
int ucxapp_at_send(ucxapp_t *app, const char *cmd);
void ucxapp_at_mode(bool on);
bool ucxapp_at_mode_active(void);

/* Socket URCs for the web server (ucxapp_cmds_web.c); queued only, handled on its own thread. */
void ucxapp_web_on_incoming(int32_t sock, int32_t listenSock);
void ucxapp_web_on_data(int32_t sock, int32_t bytes);
void ucxapp_web_on_closed(int32_t sock);

/* Socket URCs for the gnss/imu TCP bridge (ucxapp_stream.c; stubs on STM32). */
void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock);
void ucxapp_stream_on_closed(int32_t sock);
void ucxapp_stream_on_data(int32_t sock);
void ucxapp_stream_on_sps_data(void);

/* Incoming TCP connections for sock.accept (ucxapp_cmds_sock.c). */
void ucxapp_sock_on_incoming(int32_t sock, int32_t listenSock);
void ucxapp_sock_clear_incoming(void);

/* Wi-Fi station state for other packs; false when not connected (ssid and ip are then empty). */
bool ucxapp_wifi_get_status(uCxHandle_t *ucx, char *ssid, size_t ssidSize, char *ip, size_t ipSize);
/* "ssid  rssi dBm  ch N\nip" (AT calls; command thread only); false when not connected. */
bool ucxapp_wifi_dash(uCxHandle_t *ucx, char *out, size_t size);

/* GATT server helpers shared with the HID pack (ucxapp_cmds_gatt.c). */
int ucxapp_gatt_server_ensure(ucxapp_t *app, const char *key);   /* defines a predefined profile once, e.g. "bas" */
void ucxapp_gatt_ensure_advertising(ucxapp_t *app);
bool ucxapp_hid_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
bool ucxapp_hid_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
bool ucxapp_ft_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
bool ucxapp_notify_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
bool ucxapp_ft_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
/* Socket URCs for ft over TCP (ucxapp_cmds_ft.c). */
void ucxapp_ft_on_sock_data(int32_t sock);
void ucxapp_ft_on_sock_closed(int32_t sock);
void ucxapp_ft_on_sock_incoming(int32_t sock, int32_t listenSock);
bool ucxapp_ancs_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len);   /* true if consumed */
/* Value handle of a 128-bit characteristic on a connected peer (discovers once per connection). */
bool ucxapp_gatt_client_find128(ucxapp_t *app, int32_t conn, const uint8_t uuid[16], int32_t *handle);
void ucxapp_gatt_server_read(int32_t conn, int32_t handle);
bool ucxapp_gatt_notify_nus(ucxapp_t *app, const uint8_t *data, size_t len);   /* Nordic UART TX on the newest connection */
/* Defines a service with one notify (tx) and one write (rx) characteristic; once per module restart. */
int ucxapp_gatt_server_custom(ucxapp_t *app, const uint8_t svc[16], const uint8_t rx[16], const uint8_t tx[16]);
/* Port and baud of a cellular modem in transparent data mode (cell.connect); false when there is none. */
bool ucxapp_cell_data_link(char *port, size_t size, int32_t *baud);
bool ucxapp_gatt_notify_custom(ucxapp_t *app, const uint8_t *data, size_t len);
/* Short dashboard fragments for the TUI and web status line; empty when inactive. */
size_t ucxapp_gatt_summary(char *out, size_t size);
size_t ucxapp_gnss_summary(char *out, size_t size);
size_t ucxapp_imu_summary(char *out, size_t size);
/* One dash line body; false when that device is not open. */
bool ucxapp_gnss_dash(char *out, size_t size);
bool ucxapp_cell_dash(char *out, size_t size);
bool ucxapp_imu_dash(char *out, size_t size);

/* QR rendering (ucxapp_cmds_qr.c): any text, or a Wi-Fi join payload (security WPA, WEP or nopass). */
bool ucxapp_qr_print(const char *text);
bool ucxapp_qr_print_wifi(const char *ssid, const char *password, const char *security, bool hidden);

/* Bluetooth security (ucxapp_cmds_bt.c): URC sinks that reply themselves, and the Just Works defaults. */
void ucxapp_bt_bond_status(const uBtLeAddress_t *addr, int32_t status);
void ucxapp_bt_user_confirmation(uCxHandle_t *ucx, const uBtLeAddress_t *addr, int32_t value);
void ucxapp_bt_passkey_display(const uBtLeAddress_t *addr, int32_t passkey);
void ucxapp_bt_passkey_request(const uBtLeAddress_t *addr);
void ucxapp_bt_security_defaults(ucxapp_t *app);   /* once per run */

/* GATT URC sinks (ucxapp_cmds_gatt.c); the host's URC handlers forward to these. */
void ucxapp_gatt_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len);
void ucxapp_gatt_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len);

#endif /* UCXAPP_CMD_H */
