/* mdns.*: the module's mDNS responder (NORA-W36 firmware 3.4.1 or later): advertise <hostname>.local and DNS-SD
   services with TXT records on the Wi-Fi link, as Matter, HomeKit, printers and web dashboards are found.
   The module answers queries itself; this app only registers what it should say. */
#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_mdns.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define MDNS_UP_WAIT_S     5
#define MDNS_MAX_SERVICES  8      /* the module holds 3; room for firmware that allows more */
#define MDNS_TYPE_MAX      64

typedef struct {
    int32_t id;
    char type[MDNS_TYPE_MAX];
    int32_t protocol;
    int32_t port;
    char instance[64];
} mdnsService_t;

static int failed(const char *what, int32_t r)
{
    ucxapp_print("ERROR: %s failed (%d%s)\n", what, (int)r, ucxapp_errname(r));
    return UCXAPP_ERR_FAILED;
}

static const char *protoName(int32_t protocol)
{
    return protocol == U_MDNS_PROTOCOL_UDP ? "_udp" : "_tcp";
}

/* "_http._tcp" or "_matterc._udp" into the bare type the module takes plus its protocol; false without a suffix. */
static bool splitType(const char *s, char *type, size_t size, uMdnsProtocol_t *protocol)
{
    const char *dot = strrchr(s, '.');
    size_t n;

    if (dot == NULL || dot == s) {
        return false;
    }
    if (strcmp(dot, "._tcp") == 0) {
        *protocol = U_MDNS_PROTOCOL_TCP;
    } else if (strcmp(dot, "._udp") == 0) {
        *protocol = U_MDNS_PROTOCOL_UDP;
    } else {
        return false;
    }
    n = (size_t)(dot - s);
    if (n >= size) {
        return false;
    }
    memcpy(type, s, n);
    type[n] = '\0';
    return true;
}

/* Reads the service list into out (at most max); returns the count, or -1 with the error printed. */
static int listServices(ucxapp_t *app, mdnsService_t *out, int max)
{
    uCxMdnsServiceList_t s;
    int n = 0;
    int32_t r;

    uCxMdnsServiceListBegin(app->ucx);
    while (uCxMdnsServiceListGetNext(app->ucx, &s)) {
        if (n < max) {
            out[n].id = s.service_id;
            snprintf(out[n].type, sizeof(out[n].type), "%s", s.service_type != NULL ? s.service_type : "");
            out[n].protocol = s.protocol;
            out[n].port = s.port;
            snprintf(out[n].instance, sizeof(out[n].instance), "%s", s.instance_name != NULL ? s.instance_name : "");
        }
        n++;
    }
    r = uCxEnd(app->ucx);
    if (r != 0) {
        failed("listing the services", r);
        return -1;
    }
    return n < max ? n : max;
}

static void printTxtRecords(ucxapp_t *app, int32_t id)
{
    uCxMdnsTxtRecordList_t t;

    uCxMdnsTxtRecordListBegin(app->ucx, id);
    while (uCxMdnsTxtRecordListGetNext(app->ucx, &t)) {
        ucxapp_print("      %s=%s\n", t.txt_key != NULL ? t.txt_key : "", t.txt_value != NULL ? t.txt_value : "");
    }
    uCxEnd(app->ucx);
}

static void printServices(ucxapp_t *app)
{
    mdnsService_t list[MDNS_MAX_SERVICES];
    int n = listServices(app, list, MDNS_MAX_SERVICES);

    for (int i = 0; i < n; i++) {
        ucxapp_print("%2d  %s%s%s.%s port %d\n", (int)list[i].id, list[i].instance,
                     list[i].instance[0] != '\0' ? "." : "", list[i].type, protoName(list[i].protocol),
                     (int)list[i].port);
        printTxtRecords(app, list[i].id);
    }
    if (n >= 0) {
        ucxapp_print("%d service(s)\n", n);
    }
}

static int cmdMdnsStatus(ucxapp_t *app, int argc, char *argv[])
{
    uEnabled_t on = U_ENABLED_NO;
    const char *host = NULL;
    int32_t r;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxMdnsGetEnabled(app->ucx, &on);
    if (r != 0) {
        failed("reading the mDNS state", r);
        ucxapp_print("mDNS needs NORA-W36 firmware 3.4.1 or later (fw.update)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("mDNS responder: %s\n", on == U_ENABLED_YES ? "on" : "off");
    if (uCxMdnsGetHostnameBegin(app->ucx, &host) && host != NULL && host[0] != '\0') {
        ucxapp_print("Host name:      %s.local\n", host);
    } else {
        ucxapp_print("Host name:      (the Wi-Fi host name)\n");
    }
    uCxEnd(app->ucx);
    printServices(app);
    return UCXAPP_OK;
}

/* mdns.on waits for +UEMDU so a script knows the names answer; the setting stays if the network is not up yet. */
static int cmdMdnsOn(ucxapp_t *app, int argc, char *argv[])
{
    const char *host = NULL;
    int32_t waitS = MDNS_UP_WAIT_S, r;
    int next = 1;

    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 1 && !ucxapp_parse_int(argv[1], &waitS)) {
        host = argv[1];
        next = 2;
        waitS = MDNS_UP_WAIT_S;
    }
    if (argc > next && (!ucxapp_parse_int(argv[next], &waitS) || waitS < 0 || argc > next + 1)) {
        return UCXAPP_ERR_USAGE;
    }
    if (host != NULL) {
        r = uCxMdnsSetHostname(app->ucx, host);
        if (r != 0) {
            return failed("setting the host name", r);
        }
    }
    ucxapp_events_clear(URC_FLAG_MDNS_UP);
    r = uCxMdnsSetEnabled(app->ucx, U_ENABLED_YES);
    if (r != 0) {
        failed("enabling mDNS", r);
        ucxapp_print("mDNS needs Wi-Fi up (wifi.connect or wifi.ap.enable) and firmware 3.4.1 or later\n");
        return UCXAPP_ERR_FAILED;
    }
    if (waitS > 0 && !ucxapp_events_wait(URC_FLAG_MDNS_UP, (uint32_t)waitS)) {
        ucxapp_print("mDNS enabled; no up event in %d s, it answers once Wi-Fi has an address\n", (int)waitS);
        return UCXAPP_OK;
    }
    if (uCxMdnsGetHostnameBegin(app->ucx, &host) && host != NULL && host[0] != '\0') {
        ucxapp_print("mDNS up: %s.local\n", host);
    } else {
        ucxapp_print("mDNS up\n");
    }
    uCxEnd(app->ucx);
    return UCXAPP_OK;
}

static int cmdMdnsOff(ucxapp_t *app, int argc, char *argv[])
{
    int32_t r;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxMdnsSetEnabled(app->ucx, U_ENABLED_NO);
    if (r != 0) {
        return failed("disabling mDNS", r);
    }
    ucxapp_print("mDNS responder off\n");
    return UCXAPP_OK;
}

static int cmdMdnsHostname(ucxapp_t *app, int argc, char *argv[])
{
    const char *host = NULL;
    int32_t r;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        r = uCxMdnsSetHostname(app->ucx, argv[1]);
        if (r != 0) {
            return failed("setting the host name", r);
        }
        ucxapp_print("Host name %s.local\n", argv[1]);
        return UCXAPP_OK;
    }
    bool ok = uCxMdnsGetHostnameBegin(app->ucx, &host) && host != NULL;
    if (ok) {
        ucxapp_print("%s.local\n", host);
    }
    r = uCxEnd(app->ucx);
    if (!ok || r != 0) {
        return failed("reading the host name", r);
    }
    return UCXAPP_OK;
}

/* mdns.add <_type._tcp|_type._udp> <port> [instance] [key=value ...]: the instance is any argument without '='. */
static int cmdMdnsAdd(ucxapp_t *app, int argc, char *argv[])
{
    char type[MDNS_TYPE_MAX];
    uMdnsProtocol_t protocol;
    const char *instance = NULL;
    int32_t port, id = -1, r;
    int first = 3;

    if (argc < 3 || !splitType(argv[1], type, sizeof(type), &protocol) ||
        !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 3 && strchr(argv[3], '=') == NULL) {
        instance = argv[3];
        first = 4;
    }
    for (int i = first; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq == NULL || eq == argv[i]) {
            return UCXAPP_ERR_USAGE;
        }
    }
    r = instance != NULL ? uCxMdnsServiceAdd4(app->ucx, type, protocol, port, instance, &id)
                         : uCxMdnsServiceAdd3(app->ucx, type, protocol, port, &id);
    if (r != 0) {
        failed("adding the service", r);
        ucxapp_print("the module holds 3 services; mdns.status lists them, mdns.remove frees one\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Service %d added: %s%s%s.%s port %d\n", (int)id, instance != NULL ? instance : "",
                 instance != NULL ? "." : "", type, protoName(protocol), (int)port);
    for (int i = first; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        *eq = '\0';
        r = uCxMdnsTxtRecordAdd(app->ucx, id, argv[i], eq + 1);
        if (r != 0) {
            ucxapp_print("ERROR: TXT record %s failed (%d%s); the service stays, 10 records per service\n",
                         argv[i], (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("      %s=%s\n", argv[i], eq + 1);
    }
    return UCXAPP_OK;
}

static int cmdMdnsRemove(ucxapp_t *app, int argc, char *argv[])
{
    int32_t id, r;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "all") == 0) {
        mdnsService_t list[MDNS_MAX_SERVICES];
        int n = listServices(app, list, MDNS_MAX_SERVICES);

        if (n < 0) {
            return UCXAPP_ERR_FAILED;
        }
        for (int i = 0; i < n; i++) {
            r = uCxMdnsServiceRemove(app->ucx, list[i].id);
            if (r != 0) {
                return failed("removing the service", r);
            }
        }
        ucxapp_print("%d service(s) removed\n", n);
        return UCXAPP_OK;
    }
    if (!ucxapp_parse_int(argv[1], &id) || id < 0) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxMdnsServiceRemove(app->ucx, id);
    if (r != 0) {
        return failed("removing the service", r);
    }
    ucxapp_print("Service %d removed\n", (int)id);
    return UCXAPP_OK;
}

static int cmdMdnsTxt(ucxapp_t *app, int argc, char *argv[])
{
    int32_t id, r;

    if (argc < 3 || !ucxapp_parse_int(argv[1], &id) || id < 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        r = uCxMdnsTxtRecordClear(app->ucx, id);
        return r != 0 ? failed("clearing the TXT records", r)
                      : (ucxapp_print("TXT records of service %d cleared\n", (int)id), UCXAPP_OK);
    }
    if (argc == 4 && strcmp(argv[2], "remove") == 0) {
        r = uCxMdnsTxtRecordRemove(app->ucx, id, argv[3]);
        return r != 0 ? failed("removing the TXT record", r)
                      : (ucxapp_print("TXT %s removed from service %d\n", argv[3], (int)id), UCXAPP_OK);
    }
    if (argc == 4) {
        r = uCxMdnsTxtRecordAdd(app->ucx, id, argv[2], argv[3]);
        return r != 0 ? failed("adding the TXT record", r)
                      : (ucxapp_print("TXT %s=%s on service %d\n", argv[2], argv[3], (int)id), UCXAPP_OK);
    }
    return UCXAPP_ERR_USAGE;
}

static const ucxapp_cmd_t gMdnsCmds[] = {
    { "mdns.status",   "",                        "mDNS responder on or off, host name and the advertised services with their TXT records", UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsStatus },
    { "mdns.on",       "[hostname] [wait_s=5]",   "Enable the responder (Wi-Fi must be up), optionally with a host name, and wait until it answers as <hostname>.local", UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsOn },
    { "mdns.off",      "",                        "Disable the responder",                                            UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsOff },
    { "mdns.hostname", "[name]",                  "Show or set the name advertised as <name>.local (default: the Wi-Fi host name)", UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsHostname },
    { "mdns.add",      "<_type._tcp|_type._udp> <port> [instance] [key=value ...]", "Advertise a DNS-SD service (up to 3) with optional instance name and TXT records; prints its id", UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsAdd },
    { "mdns.remove",   "<id>|all",                "Stop advertising one service, or all of them",                    UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsRemove },
    { "mdns.txt",      "<id> <key> <value> | <id> remove <key> | <id> clear", "Add or replace a TXT record on a service (up to 10), remove one, or clear them", UCXAPP_CMD_NEEDS_DEVICE, cmdMdnsTxt },
};

void ucxapp_register_mdns_cmds(void)
{
    ucxapp_cmd_register(gMdnsCmds, sizeof(gMdnsCmds) / sizeof(gMdnsCmds[0]));
}
