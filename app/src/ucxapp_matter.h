#ifndef UCXAPP_MATTER_H
#define UCXAPP_MATTER_H

/* The interface between ucxapp (the host) and its Matter plug-in (ucxmatter.dll, libucxmatter.so).
 * Plain C, so the plug-in can come from another compiler: the Matter SDK needs GCC or clang, ucxapp on Windows is
 * MSVC. The plug-in never opens the UART; it sends AT commands through the host's AT client and gets a copy of
 * every URC, so the host's commands, trace and web page keep working while the node runs. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCXAPP_MATTER_ABI 4

/* The symbol the host looks up in the plug-in. */
#define UCXAPP_MATTER_ENTRY "ucxmatter_plugin"

/* Radios of the connected module. */
#define UCXAPP_MATTER_RADIO_BLE     0x01
#define UCXAPP_MATTER_RADIO_WIFI    0x02
#define UCXAPP_MATTER_RADIO_THREAD  0x04

#define UCXAPP_MATTER_LOG_ERROR   0
#define UCXAPP_MATTER_LOG_INFO    1
#define UCXAPP_MATTER_LOG_DETAIL  2

/* Results of start() and command(): the UCXAPP_ERR_* values of ucxapp_cmd.h. */
#define UCXAPP_MATTER_OK                0
#define UCXAPP_MATTER_ERR_USAGE       (-101)
#define UCXAPP_MATTER_ERR_FAILED      (-103)
#define UCXAPP_MATTER_ERR_UNSUPPORTED (-104)
/* start() only: the module refused the Matter GATT service. Its GATT server cannot remove a service, so each
   start adds one until the module reboots; the app reboots it and starts once more. */
#define UCXAPP_MATTER_ERR_GATT_FULL   (-120)

struct uCxAtClient;

/* u-connectClient's AT client (u_cx_at_client.h) as function pointers; client is always host->client. */
typedef struct {
    int32_t (*execSimpleCmd)(struct uCxAtClient *client, const char *cmd);
    int32_t (*execSimpleCmdF)(struct uCxAtClient *client, const char *cmd, const char *paramFmt, ...);
    void (*cmdBeginF)(struct uCxAtClient *client, const char *cmd, const char *paramFmt, ...);
    char *(*cmdGetRspParamLine)(struct uCxAtClient *client, const char *expectedRsp, uint8_t *binaryBuf,
                                uint16_t *binaryBufLength);
    int32_t (*cmdGetRspParamsF)(struct uCxAtClient *client, const char *expectedRsp, uint8_t *binaryBuf,
                                uint16_t *binaryBufLength, const char *paramFmt, ...);
    int32_t (*cmdEnd)(struct uCxAtClient *client);
    int32_t (*setCommandTimeout)(struct uCxAtClient *client, int32_t timeoutMs, bool permanent);
    int32_t (*getLastIoError)(struct uCxAtClient *client);
} ucxapp_matter_at_t;

/* What the host gives the plug-in; valid from start() until stop() returns. Every function may be called from
 * any thread of the plug-in. */
typedef struct {
    uint32_t abi;       /* UCXAPP_MATTER_ABI of the host */
    uint32_t size;      /* sizeof(ucxapp_matter_host_t) of the host */
    struct uCxAtClient *client;
    const ucxapp_matter_at_t *at;
    uint32_t radios;    /* UCXAPP_MATTER_RADIO_* */
    void (*print)(const char *text);            /* command output, as typed text */
    void (*log)(int level, const char *line);   /* one line without newline; shown up to the level of matter.log */
    void (*event)(const char *line);            /* one line for the Events pane */
    void (*qr)(const char *payload);            /* draws the payload as a QR code */
    /* Named blobs (a to z, 0 to 9, at most 16 characters) in the host's secure store, sealed on disk.
     * "factory" is what the node is made with (its setup codes): matter.reset removes every blob but that one. */
    bool (*storeSave)(const char *name, const void *data, size_t length);
    int32_t (*storeLoad)(const char *name, void *buffer, size_t size);   /* length stored, -1 if there is none */
    bool (*storeRemove)(const char *name);
    uint32_t (*tickMs)(void);
} ucxapp_matter_host_t;

/* What the plug-in gives the host. */
typedef struct {
    uint32_t abi;
    uint32_t size;
    const char *name;   /* shown by matter.status, e.g. "Matter 1.6.0.0" */
    /* Starts the node; argv holds the words after matter.start (the endpoints to build, none for the default). */
    int (*start)(const ucxapp_matter_host_t *host, int argc, const char *const argv[]);
    void (*stop)(void);
    /* A private copy of one URC line ("+UEWSNU:0", may be changed in place) on the AT client's thread: queue it
     * and return, AT commands belong on the plug-in's own thread. True when the URC is the node's alone (a write
     * to its GATT characteristic, data on its socket): the app's own handlers then do not see it. */
    bool (*urc)(char *line, size_t length, const uint8_t *binary, size_t binaryLength);
    /* matter.<argv[0]> with its arguments (status, endpoints, set, qr, open, reset [all]); prints with
     * host->print. */
    int (*command)(int argc, const char *const argv[]);
    /* The node for the app's live card (matter.card, the web page): lines "TAG   value" with a tag of 2 to 5
     * capitals, '\n' between them; returns the length. An endpoint is "EP    <id> <name> = <value>  [<what
     * matter.set takes>]". From any thread between start() and stop(): it sends no AT command and does not wait
     * for the node's own thread. */
    size_t (*card)(char *out, size_t size);
    /* What the plug-in is made of, for app.sbom and in its form: for each component the lines
     * "<name> <version>  [<licence>]", "  <what it is used for>", "  <https source>" and an empty one. Read
     * without start(). */
    const char *sbom;
} ucxapp_matter_plugin_t;

typedef const ucxapp_matter_plugin_t *(*ucxapp_matter_entry_t)(void);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_MATTER_H */
