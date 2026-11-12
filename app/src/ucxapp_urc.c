#include "ucxapp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_cx_at_client.h"
#include "u_cx_bluetooth.h"
#include "u_cx_channel_sounding.h"
#include "u_cx_channel_sounding_initiator.h"
#include "u_cx_diagnostics.h"
#include "u_cx_gatt_client.h"
#include "u_cx_gatt_server.h"
#include "ucxapp_cmd.h"
#include "ucxapp_ui.h"
#include "u_cx_http.h"
#ifndef UCXAPP_LITE
#include "u_cx_mdns.h"
#endif
#include "u_cx_mqtt.h"
#include "u_cx_socket.h"
#include "u_cx_sps.h"
#ifndef UCXAPP_LITE
#include "u_cx_thread.h"
#endif
#include "u_cx_wifi.h"

/* Handlers raise event flags for the command packs and mirror key URCs to the web Events pane. */

static void evt(const char *fmt, ...)
{
    char text[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (ucxappLogEventsEnabled()) {
        ucxapp_print("[event] %s\n", text);
    }
    ucxapp_ui_event(text);
}

static uint32_t gBootMs;
static bool gBootSeen;
static volatile bool gMqttUp;

bool ucxapp_mqtt_up(void)
{
    return gMqttUp;
}

uint32_t ucxapp_module_boot_ms(bool *exact)
{
    *exact = gBootSeen;
    return gBootMs;
}

static volatile ucxapp_power_t gPower = UCXAPP_POWER_ACTIVE;
static volatile bool gPowerSaveLevel;

void ucxapp_power_set(ucxapp_power_t state)
{
    if (state != gPower) {
        gPower = state;
        evt("power: %s", ucxapp_power_name(state));
    }
}

void ucxapp_power_save_level(bool on)
{
    gPowerSaveLevel = on;
    if (gPower == UCXAPP_POWER_ACTIVE || gPower == UCXAPP_POWER_SAVE) {
        ucxapp_power_set(on ? UCXAPP_POWER_SAVE : UCXAPP_POWER_ACTIVE);
    }
}

ucxapp_power_t ucxapp_power_get(void)
{
    return gPower;
}

void ucxapp_power_awake(void)
{
    ucxapp_power_set(gPowerSaveLevel ? UCXAPP_POWER_SAVE : UCXAPP_POWER_ACTIVE);
}

const char *ucxapp_power_name(ucxapp_power_t state)
{
    static const char *const kNames[] = { "active", "power save", "light sleep", "deep sleep" };
    return (unsigned)state < sizeof(kNames) / sizeof(kNames[0]) ? kNames[state] : "?";
}

static void startupUrc(struct uCxHandle *h)
{
    (void)h;
    gBootMs = ucxapp_platform_tick_ms();
    gBootSeen = true;
    gMqttUp = false;
    ucxapp_sock_set_leftovers(0);   /* a reboot closed every socket */
    evt("device: startup");
    ucxapp_power_set(gPowerSaveLevel ? UCXAPP_POWER_SAVE : UCXAPP_POWER_ACTIVE);
    ucxapp_events_signal(URC_FLAG_STARTUP);
}
static void networkUpUrc(struct uCxHandle *h)                                          { (void)h; evt("net: network up"); ucxapp_events_signal(URC_FLAG_NETWORK_UP); }
static void networkDownUrc(struct uCxHandle *h)                                        { (void)h; evt("net: network down"); ucxapp_events_signal(URC_FLAG_NETWORK_DOWN); }
static void linkUpUrc(struct uCxHandle *h, int32_t wlan, uMacAddress_t *bssid, int32_t ch)
{
    (void)h; (void)wlan; (void)bssid;
    ucxapp_ui_wifi_set(true);
    evt("wifi: link up, ch %d", (int)ch);
    ucxapp_events_signal(URC_FLAG_WIFI_LINK_UP);
}
static void linkDownUrc(struct uCxHandle *h, int32_t wlan, int32_t reason)
{
    (void)h; (void)wlan;
    ucxapp_ui_wifi_set(false);
    evt("wifi: link down (%d)", (int)reason);
    ucxapp_events_signal(URC_FLAG_WIFI_LINK_DOWN);
}
static void sockConnectedUrc(struct uCxHandle *h, int32_t s)                           { (void)h; evt("sock: %d connected", (int)s); ucxapp_events_signal(URC_FLAG_SOCK_CONNECTED); }
static void sockClosedUrc(struct uCxHandle *h, int32_t s)                              { (void)h; ucxapp_handle_remove(UCXAPP_HANDLE_SOCK, s); evt("sock: %d closed", (int)s); ucxapp_web_on_closed(s); ucxapp_stream_on_closed(s); ucxapp_ft_on_sock_closed(s); ucxapp_events_signal(URC_FLAG_SOCK_CLOSED); }
static void sockDataUrc(struct uCxHandle *h, int32_t s, int32_t n)                     { (void)h; ucxapp_web_on_data(s, n); ucxapp_stream_on_data(s); ucxapp_ft_on_sock_data(s); ucxapp_events_signal(URC_FLAG_SOCK_DATA); }
/* NORA-W36 3.2.0 and 3.5.0 send +UESOIC:<listening socket>,<ip>,<new socket> (traced: listen on 0, accept gives
 * "+UESOIC:0,<ip>,1"), the reverse of the AT command reference that the generated parser follows, so the two
 * handles are swapped here. */
static void sockIncomingUrc(struct uCxHandle *h, int32_t listenSock, uSockIpAddress_t *ip, int32_t s) { (void)h; (void)ip; ucxapp_web_on_incoming(s, listenSock); ucxapp_stream_on_incoming(s, listenSock); ucxapp_ft_on_sock_incoming(s, listenSock); ucxapp_sock_on_incoming(s, listenSock); }
static void spsConnectedUrc(struct uCxHandle *h, int32_t c)                            { (void)h; evt("sps: connected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_SPS_CONNECTED); }
static void spsDisconnectedUrc(struct uCxHandle *h, int32_t c)                         { (void)h; evt("sps: disconnected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_SPS_DISCONNECTED); }
static void spsDataUrc(struct uCxHandle *h, int32_t c, int32_t n)                      { (void)h; evt("sps: %d bytes on conn %d", (int)n, (int)c); ucxapp_stream_on_sps_data(); ucxapp_events_signal(URC_FLAG_SPS_DATA); }
#ifndef UCXAPP_LITE
static void httpStatusUrc(struct uCxHandle *h, int32_t id, int32_t code, const char *d){ (void)h; (void)d; evt("http: request %d status %d", (int)id, (int)code); ucxapp_events_signal(URC_FLAG_HTTP_RESPONSE_READY); }
static void httpDisconnectUrc(struct uCxHandle *h, int32_t id)                         { (void)h; evt("http: disconnected, id %d", (int)id); ucxapp_events_signal(URC_FLAG_HTTP_DISCONNECTED); }
static void mqttConnectedUrc(struct uCxHandle *h, int32_t id)                          { (void)h; (void)id; gMqttUp = true; evt("mqtt: connected"); ucxapp_events_signal(URC_FLAG_MQTT_CONNECTED); }
static void mqttDataUrc(struct uCxHandle *h, int32_t id, int32_t n)                    { (void)h; (void)id; evt("mqtt: %d bytes available", (int)n); ucxapp_events_signal(URC_FLAG_MQTT_DATA); }
static void mqttDisconnectedUrc(struct uCxHandle *h, int32_t id, int32_t reason)       { (void)h; (void)id; gMqttUp = false; evt("mqtt: disconnected, reason %d", (int)reason); ucxapp_events_signal(URC_FLAG_MQTT_DISCONNECTED); }
static void mqttSubscribedUrc(struct uCxHandle *h, int32_t id, uMqttSubscribeAction_t a)  { (void)h; (void)id; (void)a; evt("mqtt: subscribe completed"); ucxapp_events_signal(URC_FLAG_MQTT_SUBSCRIBED); }
#endif
static void btConnectedUrc(struct uCxHandle *h, int32_t c, uBtLeAddress_t *a)          { (void)h; (void)a; ucxapp_handle_add(UCXAPP_HANDLE_CONN, c); evt("bt: connected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_BT_CONNECTED); }
static void btDisconnectedUrc(struct uCxHandle *h, int32_t c)                          { (void)h; ucxapp_handle_remove(UCXAPP_HANDLE_CONN, c); evt("bt: disconnected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_BT_DISCONNECTED); }

static void btBondUrc(struct uCxHandle *h, uBtLeAddress_t *a, uBtBondStatus_t s)       { (void)h; evt("bt: bond status %d", (int)s); ucxapp_bt_bond_status(a, (int32_t)s); }
static void btPhyUrc(struct uCxHandle *h, int32_t c, int32_t status, int32_t tx, int32_t rx)
{
    (void)h;
    evt("bt: PHY update conn %d%s, tx %d rx %d", (int)c, status != 0 ? " failed" : "", (int)tx, (int)rx);
    ucxapp_bt_phy_update(c, status, tx, rx);
    ucxapp_events_signal(URC_FLAG_BT_PHY_UPDATE);
}
static void btConfirmUrc(struct uCxHandle *h, uBtLeAddress_t *a, int32_t v)            { (void)h; ucxapp_bt_user_confirmation(h, a, v); }
static void btPasskeyShowUrc(struct uCxHandle *h, uBtLeAddress_t *a, int32_t v)        { (void)h; ucxapp_bt_passkey_display(a, v); }
static void btPasskeyAskUrc(struct uCxHandle *h, uBtLeAddress_t *a)                    { (void)h; ucxapp_bt_passkey_request(a); }
#ifndef UCXAPP_LITE
static void btCsUrc(struct uCxHandle *h, int32_t c, uBtChannelSoundingEvent_t e, uBtHciStatus_t s)
{
    static const char *const kCsEvents[] = { "capabilities exchanged", "security enabled", "procedure enabled",
                                             "procedure disabled" };

    (void)h;
    evt("bt: channel sounding %s, conn %d%s", (unsigned)e < 4 ? kCsEvents[e] : "event", (int)c, s != 0 ? " (failed)" : "");
}
static void btCsDistanceUrc(struct uCxHandle *h, int32_t c, int32_t mm)
{
    (void)h;
    evt("bt: distance %d.%03d m, conn %d", (int)(mm / 1000), (int)((mm < 0 ? -mm : mm) % 1000), (int)c);
    ucxapp_bt_cs_distance(c, mm);
}
static void btCsPctUrc(struct uCxHandle *h, int32_t c, int32_t ant, int32_t n, const uint8_t *d, size_t len)
{
    (void)h; (void)d;
    evt("bt: PCT conn %d antenna %d, %d channels (%u bytes)", (int)c, (int)ant, (int)n, (unsigned)len);
    ucxapp_bt_cs_pct(c, ant, n);
}
#endif

static void gattNotifyUrc(struct uCxHandle *h, int32_t conn, int32_t handle, uByteArray_t *data)
{
    (void)h;
    evt("gatt: notification conn %d handle %d, %d bytes", (int)conn, (int)handle, (int)data->length);
    ucxapp_gatt_notification(conn, handle, data->pData, data->length);
}

static void gattWriteUrc(struct uCxHandle *h, int32_t conn, int32_t handle, uByteArray_t *data, uGattServerOptions_t options)
{
    (void)h; (void)options;
    evt("gatt: write on conn %d handle %d, %d bytes", (int)conn, (int)handle, (int)data->length);
    ucxapp_gatt_server_write(conn, handle, data->pData, data->length);
}

static void gattReadUrc(struct uCxHandle *h, int32_t conn, int32_t handle)
{
    (void)h;
    ucxapp_gatt_server_read(conn, handle);
}

static void pingCompleteUrc(struct uCxHandle *h, int32_t sent, int32_t received, int32_t lossRate, int32_t avgMs)
{
    (void)h;
    (void)lossRate;
    evt("ping: %d/%d replies, avg %d ms", (int)received, (int)sent, (int)avgMs);
    gUcxappPingSent = sent;
    gUcxappPingReceived = received;
    gUcxappPingAvgMs = avgMs;
    ucxapp_events_signal(URC_FLAG_PING_COMPLETE);
}

void ucxapp_adopt_connections(uCxHandle_t *ucx)
{
    uCxBtListConnections_t conn;
    uCxSocketListStatus_t sock;
    uint32_t leftovers = 0;

    uCxBluetoothListConnectionsBegin(ucx);
    while (uCxBluetoothListConnectionsGetNext(ucx, &conn)) {
        ucxapp_handle_add(UCXAPP_HANDLE_CONN, conn.conn_handle);
    }
    uCxEnd(ucx);
    uCxSocketListStatusBegin(ucx);
    while (uCxSocketListStatusGetNext(ucx, &sock)) {
        if (sock.protocol == U_SOCKET_PROTOCOL_UDP && sock.status == 1 && sock.socket_handle >= 0 &&
            sock.socket_handle < 32) {
            leftovers |= 1u << sock.socket_handle;
        }
    }
    uCxEnd(ucx);
    ucxapp_sock_set_leftovers(leftovers);
}

#ifndef UCXAPP_LITE
static int32_t gJoinerResult = -1;

int32_t ucxapp_thread_joiner_result(void)
{
    return gJoinerResult;
}

static void threadUpUrc(struct uCxHandle *h, uThreadDeviceRole_t r)          { (void)h; evt("thread: up, %s", ucxapp_thread_role_name(r)); ucxapp_ui_thread_changed(); ucxapp_events_signal(URC_FLAG_THREAD_UP); }
static void threadDownUrc(struct uCxHandle *h)                              { (void)h; evt("thread: down"); ucxapp_ui_thread_changed(); }
static void threadRoleUrc(struct uCxHandle *h, uThreadDeviceRole_t r)        { (void)h; evt("thread: role %s", ucxapp_thread_role_name(r)); ucxapp_ui_thread_changed(); }
static void threadSrpUrc(struct uCxHandle *h, int32_t id, uThreadSrpState_t s) { (void)h; evt("thread: srp service %d state %d", (int)id, (int)s); }
static void threadJoinerUrc(struct uCxHandle *h, uThreadJoinerResult_t r)   { (void)h; gJoinerResult = (int32_t)r; evt("thread: joiner result %d", (int)r); ucxapp_events_signal(URC_FLAG_THREAD_JOINER); }

static void mdnsUpUrc(struct uCxHandle *h)                                  { (void)h; evt("mdns: up"); ucxapp_events_signal(URC_FLAG_MDNS_UP); }
static void mdnsDownUrc(struct uCxHandle *h)                                { (void)h; evt("mdns: down"); ucxapp_events_signal(URC_FLAG_MDNS_DOWN); }

/* +UEMDCD is newer than u-connectClient, so it is picked off the AT client's URC callback before the client
   parses the line (which it changes in place); every URC then goes on to the client unchanged. */
static uUrcCallback_t gNextUrc;
static void *gNextUrcTag;
static bool gMdnsConflictSeen;
static int32_t gMdnsConflictType;
static char gMdnsConflictPrev[64], gMdnsConflictNew[64];

/* Copies the next comma-separated field (quotes removed) of *p into out and moves *p past it. */
static void urcField(const char **p, char *out, size_t size)
{
    const char *s = *p;
    bool quoted = *s == '"';
    size_t n = 0;

    if (quoted) {
        s++;
    }
    while (*s != '\0' && (quoted ? *s != '"' : *s != ',')) {
        if (n + 1 < size) {
            out[n++] = *s;
        }
        s++;
    }
    out[n] = '\0';
    if (quoted && *s == '"') {
        s++;
    }
    if (*s == ',') {
        s++;
    }
    *p = s;
}

static void mdnsConflictLine(const char *params)
{
    char type[8];

    urcField(&params, type, sizeof(type));
    urcField(&params, gMdnsConflictPrev, sizeof(gMdnsConflictPrev));
    urcField(&params, gMdnsConflictNew, sizeof(gMdnsConflictNew));
    gMdnsConflictType = atoi(type);
    gMdnsConflictSeen = true;
    evt("mdns: %s '%s' in use, renamed to '%s'", gMdnsConflictType == 0 ? "hostname" : "service",
        gMdnsConflictPrev, gMdnsConflictNew);
}

static void urcTap(struct uCxAtClient *pClient, void *pTag, char *pLine, size_t lineLength,
                   uint8_t *pBinaryData, size_t binaryDataLen)
{
    static const char kConflict[] = "+UEMDCD:";
    if (lineLength >= sizeof(kConflict) - 1 && strncmp(pLine, kConflict, sizeof(kConflict) - 1) == 0) {
        mdnsConflictLine(pLine + sizeof(kConflict) - 1);
    }
    if (gNextUrc != NULL) {
        gNextUrc(pClient, pTag, pLine, lineLength, pBinaryData, binaryDataLen);
    }
}
#endif

bool ucxapp_mdns_conflict(int32_t *type, const char **previous, const char **renamed)
{
#ifndef UCXAPP_LITE
    *type = gMdnsConflictType;
    *previous = gMdnsConflictPrev;
    *renamed = gMdnsConflictNew;
    return gMdnsConflictSeen;
#else
    (void)type;
    (void)previous;
    (void)renamed;
    return false;
#endif
}

void ucxapp_urc_register(uCxHandle_t *ucx)
{
    gBootMs = ucxapp_platform_tick_ms();
    gBootSeen = false;
    gPower = UCXAPP_POWER_ACTIVE;
    gPowerSaveLevel = false;
    ucx->callbacks.STARTUP = startupUrc;

    uCxWifiRegisterLinkUp(ucx, linkUpUrc);
    uCxWifiRegisterLinkDown(ucx, linkDownUrc);
    uCxWifiRegisterStationNetworkUp(ucx, networkUpUrc);
    uCxWifiRegisterStationNetworkDown(ucx, networkDownUrc);

    uCxSocketRegisterConnect(ucx, sockConnectedUrc);
    uCxSocketRegisterClosed(ucx, sockClosedUrc);
    uCxSocketRegisterDataAvailable(ucx, sockDataUrc);
    uCxSocketRegisterIncomingConnection(ucx, sockIncomingUrc);

    uCxSpsRegisterConnect(ucx, spsConnectedUrc);
    uCxSpsRegisterDisconnect(ucx, spsDisconnectedUrc);
    uCxSpsRegisterDataAvailable(ucx, spsDataUrc);

    uCxDiagnosticsRegisterPingComplete(ucx, pingCompleteUrc);

#ifndef UCXAPP_LITE
    uCxHttpRegisterRequestStatus(ucx, httpStatusUrc);
    uCxHttpRegisterDisconnect(ucx, httpDisconnectUrc);

    uCxMqttRegisterConnect(ucx, mqttConnectedUrc);
    uCxMqttRegisterDisconnect(ucx, mqttDisconnectedUrc);
    uCxMqttRegisterDataAvailable(ucx, mqttDataUrc);
    uCxMqttRegisterSubscribeCompleted(ucx, mqttSubscribedUrc);
#endif

    uCxBluetoothRegisterConnect(ucx, btConnectedUrc);
    uCxBluetoothRegisterDisconnect(ucx, btDisconnectedUrc);
    uCxBluetoothRegisterBondStatus(ucx, btBondUrc);
    uCxBluetoothRegisterPhyUpdate(ucx, btPhyUrc);
    uCxBluetoothRegisterUserConfirmation(ucx, btConfirmUrc);
    uCxBluetoothRegisterPasskeyEntry(ucx, btPasskeyShowUrc);
    uCxBluetoothRegisterPasskeyRequest(ucx, btPasskeyAskUrc);
#ifndef UCXAPP_LITE
    uCxChannelSoundingRegisterCsStatus(ucx, btCsUrc);
    uCxChannelSoundingRegisterDistance(ucx, btCsDistanceUrc);
    uCxChannelSoundingRegisterPct(ucx, btCsPctUrc);

    uCxThreadRegisterNetworkUp(ucx, threadUpUrc);
    uCxThreadRegisterNetworkDown(ucx, threadDownUrc);
    uCxThreadRegisterRoleChanged(ucx, threadRoleUrc);
    uCxThreadRegisterSrpServiceState(ucx, threadSrpUrc);
    uCxThreadRegisterJoinerResult(ucx, threadJoinerUrc);

    uCxMdnsRegisterMdnsUp(ucx, mdnsUpUrc);
    uCxMdnsRegisterMdnsDown(ucx, mdnsDownUrc);
    gMdnsConflictSeen = false;
    if (ucx->pAtClient->urcCallback != urcTap) {
        gNextUrc = ucx->pAtClient->urcCallback;
        gNextUrcTag = ucx->pAtClient->pUrcCallbackTag;
        uCxAtClientSetUrcCallback(ucx->pAtClient, urcTap, gNextUrcTag);
    }
#endif

    uCxGattClientRegisterNotification(ucx, gattNotifyUrc);
    uCxGattClientRegisterIndication(ucx, gattNotifyUrc);
    uCxGattServerRegisterNotification(ucx, gattWriteUrc);
    uCxGattServerRegisterReadAttribute(ucx, gattReadUrc);
}
