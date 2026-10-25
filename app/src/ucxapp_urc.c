#include "ucxapp.h"

#include <stdarg.h>
#include <stdio.h>

#include "u_cx_bluetooth.h"
#include "u_cx_diagnostics.h"
#include "u_cx_gatt_client.h"
#include "u_cx_gatt_server.h"
#include "ucxapp_cmd.h"
#include "ucxapp_ui.h"
#include "u_cx_http.h"
#include "u_cx_mqtt.h"
#include "u_cx_socket.h"
#include "u_cx_sps.h"
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

uint32_t ucxapp_module_boot_ms(bool *exact)
{
    *exact = gBootSeen;
    return gBootMs;
}

static void startupUrc(struct uCxHandle *h)
{
    (void)h;
    gBootMs = ucxapp_platform_tick_ms();
    gBootSeen = true;
    evt("device: startup");
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
static void mqttConnectedUrc(struct uCxHandle *h, int32_t id)                          { (void)h; (void)id; evt("mqtt: connected"); ucxapp_events_signal(URC_FLAG_MQTT_CONNECTED); }
static void mqttDataUrc(struct uCxHandle *h, int32_t id, int32_t n)                    { (void)h; (void)id; evt("mqtt: %d bytes available", (int)n); ucxapp_events_signal(URC_FLAG_MQTT_DATA); }
static void mqttDisconnectedUrc(struct uCxHandle *h, int32_t id, int32_t reason)       { (void)h; (void)id; evt("mqtt: disconnected, reason %d", (int)reason); ucxapp_events_signal(URC_FLAG_MQTT_DISCONNECTED); }
static void mqttSubscribedUrc(struct uCxHandle *h, int32_t id, uMqttSubscribeAction_t a)  { (void)h; (void)id; (void)a; evt("mqtt: subscribe completed"); ucxapp_events_signal(URC_FLAG_MQTT_SUBSCRIBED); }
#endif
static void btConnectedUrc(struct uCxHandle *h, int32_t c, uBtLeAddress_t *a)          { (void)h; (void)a; ucxapp_handle_add(UCXAPP_HANDLE_CONN, c); evt("bt: connected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_BT_CONNECTED); }
static void btDisconnectedUrc(struct uCxHandle *h, int32_t c)                          { (void)h; ucxapp_handle_remove(UCXAPP_HANDLE_CONN, c); evt("bt: disconnected, conn %d", (int)c); ucxapp_events_signal(URC_FLAG_BT_DISCONNECTED); }

static void btBondUrc(struct uCxHandle *h, uBtLeAddress_t *a, uBtBondStatus_t s)       { (void)h; evt("bt: bond status %d", (int)s); ucxapp_bt_bond_status(a, (int32_t)s); }
static void btConfirmUrc(struct uCxHandle *h, uBtLeAddress_t *a, int32_t v)            { (void)h; ucxapp_bt_user_confirmation(h, a, v); }
static void btPasskeyShowUrc(struct uCxHandle *h, uBtLeAddress_t *a, int32_t v)        { (void)h; ucxapp_bt_passkey_display(a, v); }
static void btPasskeyAskUrc(struct uCxHandle *h, uBtLeAddress_t *a)                    { (void)h; ucxapp_bt_passkey_request(a); }

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

    uCxBluetoothListConnectionsBegin(ucx);
    while (uCxBluetoothListConnectionsGetNext(ucx, &conn)) {
        ucxapp_handle_add(UCXAPP_HANDLE_CONN, conn.conn_handle);
    }
    uCxEnd(ucx);
}

void ucxapp_urc_register(uCxHandle_t *ucx)
{
    gBootMs = ucxapp_platform_tick_ms();
    gBootSeen = false;
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
    uCxBluetoothRegisterUserConfirmation(ucx, btConfirmUrc);
    uCxBluetoothRegisterPasskeyEntry(ucx, btPasskeyShowUrc);
    uCxBluetoothRegisterPasskeyRequest(ucx, btPasskeyAskUrc);

    uCxGattClientRegisterNotification(ucx, gattNotifyUrc);
    uCxGattClientRegisterIndication(ucx, gattNotifyUrc);
    uCxGattServerRegisterNotification(ucx, gattWriteUrc);
    uCxGattServerRegisterReadAttribute(ucx, gattReadUrc);
}
