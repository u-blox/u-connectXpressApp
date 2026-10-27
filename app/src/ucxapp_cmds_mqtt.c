#include "ucxapp_cmd.h"
#include "ucxapp_stats.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_mqtt.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define MQTT_ID            0
#define MQTT_DEFAULT_HOST  "broker.emqx.io"
#define MQTT_DEFAULT_PORT  1883
#define MQTT_MAX_PAYLOAD   512
#define MQTT_MAX_MESSAGES  10

static bool parseQos(const char *s, uMqttQos_t *qos)
{
    int32_t v;

    if (!ucxapp_parse_int(s, &v) || v < 0 || v > 2) {
        return false;
    }
    *qos = (uMqttQos_t)v;
    return true;
}

/* The module answers ERROR:15 while the previous MQTT operation is still in flight, so retry briefly. */
#define MQTT_BUSY_ERROR    (-15)
#define MQTT_BUSY_RETRIES  10
#define MQTT_BUSY_DELAY_MS 100
#define MQTT_SUBSCRIBE_WAIT_S 5

static int cmdMqttConnect(ucxapp_t *app, int argc, char *argv[])
{
    const char *host = MQTT_DEFAULT_HOST;
    int32_t port = MQTT_DEFAULT_PORT;
    char clientId[32];

    if (argc > 3 || (argc > 1 && argv[1][0] == '\0') ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535))) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 1) {
        host = argv[1];
    }
    /* Brokers drop a client whose ID is already in use, so derive one from the tick clock. */
    snprintf(clientId, sizeof(clientId), "ucxapp-%u", (unsigned)(uPortGetTickTimeMs() % 100000));

    int32_t result = uCxMqttSetConnectionParams4(app->ucx, MQTT_ID, host, port, clientId);
    if (result != 0) {
        ucxapp_print("ERROR: cannot set connection parameters (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_events_clear(URC_FLAG_MQTT_CONNECTED);
    result = uCxMqttConnect(app->ucx, MQTT_ID);
    if (result != 0) {
        ucxapp_print("ERROR: connect failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_events_wait(URC_FLAG_MQTT_CONNECTED, 10)) {
        ucxapp_print("ERROR: timeout waiting for the broker\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("connected to %s:%d as %s\n", host, (int)port, clientId);
    return UCXAPP_OK;
}

static int cmdMqttDisconnect(ucxapp_t *app, int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (uCxMqttDisconnect(app->ucx, MQTT_ID) != 0) {
        ucxapp_print("ERROR: disconnect failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("disconnected\n");
    return UCXAPP_OK;
}

static int cmdMqttSubscribe(ucxapp_t *app, int argc, char *argv[])
{
    uMqttQos_t qos = U_MQTT_QOS_AT_MOST_ONCE;

    if (argc < 2 || argc > 3 || (argc > 2 && !parseQos(argv[2], &qos))) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t result = MQTT_BUSY_ERROR;
    ucxapp_events_clear(URC_FLAG_MQTT_SUBSCRIBED);
    for (int i = 0; result == MQTT_BUSY_ERROR && i < MQTT_BUSY_RETRIES; i++) {
        if (i > 0) {
            U_CX_PORT_SLEEP_MS(MQTT_BUSY_DELAY_MS);
        }
        result = uCxMqttSubscribe4(app->ucx, MQTT_ID, U_MQTT_SUBSCRIBE_ACTION_SUBSCRIBE, argv[1], qos);
    }
    if (result != 0) {
        ucxapp_print("ERROR: subscribe failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    /* Publishing before the broker's subscribe ack gets ERROR:15. */
    if (!ucxapp_events_wait(URC_FLAG_MQTT_SUBSCRIBED, MQTT_SUBSCRIBE_WAIT_S)) {
        ucxapp_print("WARNING: no subscribe confirmation from the broker\n");
    }
    ucxapp_print("subscribed to %s\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdMqttUnsubscribe(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t result = MQTT_BUSY_ERROR;
    for (int i = 0; result == MQTT_BUSY_ERROR && i < MQTT_BUSY_RETRIES; i++) {
        if (i > 0) {
            U_CX_PORT_SLEEP_MS(MQTT_BUSY_DELAY_MS);
        }
        result = uCxMqttSubscribe3(app->ucx, MQTT_ID, U_MQTT_SUBSCRIBE_ACTION_UNSUBSCRIBE, argv[1]);
    }
    if (result != 0) {
        ucxapp_print("ERROR: unsubscribe failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("unsubscribed from %s\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdMqttPublish(ucxapp_t *app, int argc, char *argv[])
{
    char text[MQTT_MAX_PAYLOAD];
    size_t len = 0;

    if (argc < 3) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (len + n + 1 >= sizeof(text)) {
            ucxapp_print("ERROR: message too long (max %d bytes)\n", (int)sizeof(text) - 1);
            return UCXAPP_ERR_FAILED;
        }
        if (i > 2) {
            text[len++] = ' ';
        }
        memcpy(&text[len], argv[i], n);
        len += n;
    }
    /* uCxMqttPublish returns the packet ID (>= 0) on success. */
    int32_t result = MQTT_BUSY_ERROR;
    for (int i = 0; result == MQTT_BUSY_ERROR && i < MQTT_BUSY_RETRIES; i++) {
        if (i > 0) {
            U_CX_PORT_SLEEP_MS(MQTT_BUSY_DELAY_MS);
        }
        result = uCxMqttPublish(app->ucx, MQTT_ID, U_MQTT_QOS_AT_MOST_ONCE, U_MQTT_RETAIN_NO,
                                argv[1], (const uint8_t *)text, (int32_t)len);
    }
    if (result < 0) {
        ucxapp_print("ERROR: publish failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_MQTT, len, 0);
    ucxapp_print("published %d bytes to %s\n", (int)len, argv[1]);
    return UCXAPP_OK;
}

/* Waits for the first message, then drains whatever else is queued. */
static int cmdMqttRead(ucxapp_t *app, int argc, char *argv[])
{
    int32_t waitS = 10;
    uint8_t buf[MQTT_MAX_PAYLOAD + 1];
    int received = 0;

    if (argc > 2 || (argc > 1 && (!ucxapp_parse_int(argv[1], &waitS) || waitS < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_wait(URC_FLAG_MQTT_DATA, (uint32_t)waitS);   /* the read below tells if data arrived */
    while (received < MQTT_MAX_MESSAGES) {
        const char *topic = NULL;
        int32_t n = uCxMqttReadBegin(app->ucx, MQTT_ID, buf, MQTT_MAX_PAYLOAD, &topic);
        if (n < 0 && received > 0) {
            uCxEnd(app->ucx);
            break;   /* queue drained: the module answers ERROR:11 */
        }
        if (n < 0) {
            uCxEnd(app->ucx);
            ucxapp_print("ERROR: read failed (%d%s)\n", (int)n, ucxapp_errname(n));
            return UCXAPP_ERR_FAILED;
        }
        if (n == 0) {
            uCxEnd(app->ucx);
            break;
        }
        ucxapp_stats_add(UCXAPP_STATS_MQTT, 0, (size_t)n);
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] < 0x20 || buf[i] > 0x7e) {
                buf[i] = '.';
            }
        }
        buf[n] = '\0';
        ucxapp_print("%s: %s\n", topic != NULL ? topic : "(unknown)", (const char *)buf);
        uCxEnd(app->ucx);
        received++;
    }
    ucxapp_print("%d message(s)\n", received);
    return received > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gMqttCmds[] = {
    { "mqtt.connect",     "[host] [port]",    "Connect to a broker (default broker.emqx.io:1883, no TLS)", UCXAPP_CMD_NEEDS_DEVICE, cmdMqttConnect },
    { "mqtt.disconnect",  "",                 "Disconnect from the broker",                               UCXAPP_CMD_NEEDS_DEVICE, cmdMqttDisconnect },
    { "mqtt.subscribe",   "<topic> [qos]",    "Subscribe to a topic (wildcards allowed, qos 0-2)",        UCXAPP_CMD_NEEDS_DEVICE, cmdMqttSubscribe },
    { "mqtt.unsubscribe", "<topic>",          "Unsubscribe from a topic",                                 UCXAPP_CMD_NEEDS_DEVICE, cmdMqttUnsubscribe },
    { "mqtt.publish",     "<topic> <text...>", "Publish a message (QoS 0, not retained)",                 UCXAPP_CMD_NEEDS_DEVICE, cmdMqttPublish },
    { "mqtt.read",        "[wait_s]",         "Wait (default 10 s) for messages and print them",          UCXAPP_CMD_NEEDS_DEVICE, cmdMqttRead },
};

void ucxapp_register_mqtt_cmds(void)
{
    ucxapp_cmd_register(gMqttCmds, sizeof(gMqttCmds) / sizeof(gMqttCmds[0]));
}
