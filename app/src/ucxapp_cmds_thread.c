/* thread.*: Thread (IEEE 802.15.4 mesh) on modules with Thread firmware (NORA-B26, ALMA-W1).
   Join a network from a border router's dataset, credentials or a commissioner (joiner),
   then look at the mesh: role, addresses, neighbours, routers, counters, SRP services and DNS. */
#include "ucxapp_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_at_params.h"
#include "u_cx_diagnostics.h"
#include "u_cx_thread.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define THREAD_DATASET_MAX  254   /* the Thread specification caps an operational dataset at 254 bytes */
#define THREAD_UP_WAIT_S    30
#define THREAD_JOIN_WAIT_S  120

const char *ucxapp_thread_role_name(int32_t role)
{
    static const char *const kRoles[] = { "disabled", "detached", "child", "router", "leader" };

    return role >= 0 && role < (int32_t)(sizeof(kRoles) / sizeof(kRoles[0])) ? kRoles[role] : "unknown";
}

static void printHex(const char *label, const uByteArray_t *b)
{
    ucxapp_print("%s", label);
    for (size_t i = 0; i < b->length; i++) {
        ucxapp_print("%02x", b->pData[i]);
    }
    ucxapp_print("\n");
}

static void hexText(const uByteArray_t *b, char *out, size_t size)
{
    size_t n = 0;

    out[0] = '\0';
    for (size_t i = 0; i < b->length && n + 3 <= size; i++) {
        n += (size_t)snprintf(out + n, size - n, "%02x", b->pData[i]);
    }
}

/* Hex text (optional 0x, no separators) to bytes; false on bad digits, odd length or a size other than want. */
static bool parseHex(const char *s, uint8_t *out, size_t max, size_t *len, size_t want)
{
    size_t n;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    n = strlen(s);
    if (n == 0 || n % 2 != 0 || n / 2 > max || (want != 0 && n / 2 != want)) {
        return false;
    }
    for (size_t i = 0; i < n; i += 2) {
        unsigned v;
        if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1]) || sscanf(s + i, "%2x", &v) != 1) {
            return false;
        }
        out[i / 2] = (uint8_t)v;
    }
    *len = n / 2;
    return true;
}

static int failed(const char *what, int32_t r)
{
    ucxapp_print("ERROR: %s failed (%d%s)\n", what, (int)r, ucxapp_errname(r));
    return UCXAPP_ERR_FAILED;
}

static bool printStatusLine(ucxapp_t *app)
{
    uCxThreadGetStatus_t st;
    char partition[24], ext[24];

    if (!uCxThreadGetStatusBegin(app->ucx, &st)) {
        uCxEnd(app->ucx);
        return false;
    }
    hexText(&st.partition_id, partition, sizeof(partition));
    hexText(&st.ext_addr, ext, sizeof(ext));
    ucxapp_print("Role:          %s\n", ucxapp_thread_role_name(st.device_role));
    ucxapp_print("RLOC16:        0x%04x\n", (unsigned)st.rloc16);
    ucxapp_print("Partition:     %s\n", partition[0] != '\0' ? partition : "-");
    ucxapp_print("Ext address:   %s\n", ext[0] != '\0' ? ext : "-");
    uCxEnd(app->ucx);
    return true;
}

static int cmdThreadStatus(ucxapp_t *app, int argc, char *argv[])
{
    uEnabled_t on = U_ENABLED_NO;
    uCxThreadGetNetworkCredentials_t nc;
    int32_t r;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxThreadGetEnabled(app->ucx, &on);
    if (r != 0) {
        return failed("reading the Thread state", r);
    }
    ucxapp_print("Thread:        %s\n", on == U_ENABLED_YES ? "on" : "off");
    if (uCxThreadGetNetworkCredentialsBegin(app->ucx, &nc) && nc.network_name != NULL && nc.network_name[0] != '\0') {
        ucxapp_print("Network:       %s\n", nc.network_name);
        ucxapp_print("Channel:       %d\n", (int)nc.channel);
        ucxapp_print("PAN ID:        0x%04x\n", (unsigned)nc.pan_id);
    } else {
        ucxapp_print("Network:       none (thread.network or thread.dataset)\n");
    }
    uCxEnd(app->ucx);
    if (on == U_ENABLED_YES) {
        printStatusLine(app);
    }
    return UCXAPP_OK;
}

/* thread.start waits for +UETHU so a script can go on with the mesh up. */
static int cmdThreadStart(ucxapp_t *app, int argc, char *argv[])
{
    int32_t waitS = THREAD_UP_WAIT_S, r;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &waitS) || waitS < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_clear(URC_FLAG_THREAD_UP);
    r = uCxThreadSetEnabled(app->ucx, U_ENABLED_YES);
    if (r != 0) {
        return failed("starting Thread", r);
    }
    if (waitS == 0) {
        ucxapp_print("Thread starting\n");
        return UCXAPP_OK;
    }
    ucxapp_print("Attaching to the Thread network (up to %d s)...\n", (int)waitS);
    if (!ucxapp_events_wait(URC_FLAG_THREAD_UP, (uint32_t)waitS)) {
        ucxapp_print("ERROR: not attached after %d s; check the network with thread.network and thread.scan\n", (int)waitS);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Thread up\n");
    printStatusLine(app);
    return UCXAPP_OK;
}

static int cmdThreadStop(ucxapp_t *app, int argc, char *argv[])
{
    int32_t r;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxThreadSetEnabled(app->ucx, U_ENABLED_NO);
    if (r != 0) {
        return failed("stopping Thread", r);
    }
    ucxapp_print("Thread off\n");
    return UCXAPP_OK;
}

static int cmdThreadNetwork(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t extPan[8], key[16];
    size_t extLen = 0, keyLen = 0;
    int32_t channel, panId, r;

    if (argc == 1) {
        uCxThreadGetNetworkCredentials_t nc;
        if (!uCxThreadGetNetworkCredentialsBegin(app->ucx, &nc)) {
            uCxEnd(app->ucx);
            ucxapp_print("No network set: thread.network new, or thread.network <name> <channel> <pan_id> <ext_pan_id> <key>\n");
            return UCXAPP_OK;
        }
        ucxapp_print("Network:       %s\n", nc.network_name != NULL ? nc.network_name : "");
        ucxapp_print("Channel:       %d\n", (int)nc.channel);
        ucxapp_print("PAN ID:        0x%04x\n", (unsigned)nc.pan_id);
        printHex("Ext PAN ID:    ", &nc.ext_pan_id);
        printHex("Network key:   ", &nc.network_key);
        uCxEnd(app->ucx);
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "new") == 0) {
        r = uCxThreadDatasetNew(app->ucx);
        if (r != 0) {
            return failed("creating a new network", r);
        }
        ucxapp_print("New Thread network with random credentials; thread.start makes this module its leader\n");
        return UCXAPP_OK;
    }
    if (argc != 6 || argv[1][0] == '\0' || strlen(argv[1]) > 16 ||
        !ucxapp_parse_int(argv[2], &channel) || channel < 11 || channel > 26 ||
        !ucxapp_parse_int(argv[3], &panId) || panId < 0 || panId > 0xfffe ||
        !parseHex(argv[4], extPan, sizeof(extPan), &extLen, sizeof(extPan)) ||
        !parseHex(argv[5], key, sizeof(key), &keyLen, sizeof(key))) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxThreadSetNetworkCredentials(app->ucx, argv[1], channel, panId, extPan, (int32_t)extLen, key, (int32_t)keyLen);
    if (r != 0) {
        return failed("setting the network credentials", r);
    }
    ucxapp_print("Network %s on channel %d set; thread.start joins it\n", argv[1], (int)channel);
    return UCXAPP_OK;
}

/* The active operational dataset as TLV hex: the form a border router prints (ot-ctl dataset active -x). */
static int cmdThreadDataset(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t tlv[THREAD_DATASET_MAX];
    size_t len = 0;
    int32_t r;

    if (argc == 1) {
        uByteArray_t ds;
        if (!uCxThreadGetDatasetBegin(app->ucx, &ds) || ds.length == 0) {
            uCxEnd(app->ucx);
            ucxapp_print("No active dataset: thread.network new, or thread.dataset <hex> from the border router\n");
            return UCXAPP_OK;
        }
        printHex("", &ds);
        uCxEnd(app->ucx);
        return UCXAPP_OK;
    }
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "clear") == 0) {
        r = uCxThreadDatasetClear(app->ucx);
        if (r != 0) {
            return failed("clearing the dataset", r);
        }
        ucxapp_print("Dataset cleared\n");
        return UCXAPP_OK;
    }
    if (!parseHex(argv[1], tlv, sizeof(tlv), &len, 0)) {
        ucxapp_print("ERROR: the dataset is hex text of at most %d bytes (from ot-ctl dataset active -x)\n", THREAD_DATASET_MAX);
        return UCXAPP_ERR_USAGE;
    }
    r = uCxThreadSetDataset(app->ucx, tlv, (int32_t)len);
    if (r != 0) {
        return failed("setting the dataset", r);
    }
    ucxapp_print("Dataset set (%u bytes); thread.start joins the network\n", (unsigned)len);
    return UCXAPP_OK;
}

static int cmdThreadType(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kTypes[] = { "router", "end", "sleepy" };
    int32_t poll = 0, r;
    int type = -1;

    if (argc == 1) {
        uCxThreadGetDeviceType_t dt;
        r = uCxThreadGetDeviceType(app->ucx, &dt);
        if (r != 0) {
            return failed("reading the device type", r);
        }
        if (dt.device_type == U_THREAD_DEVICE_TYPE_SLEEPY_END_DEVICE) {
            ucxapp_print("Type:          sleepy (polls its parent every %d ms)\n", (int)dt.poll_period);
        } else {
            ucxapp_print("Type:          %s\n", dt.device_type == U_THREAD_DEVICE_TYPE_ROUTER ? "router (can route for others)" : "end (never routes)");
        }
        return UCXAPP_OK;
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(argv[1], kTypes[i]) == 0) {
            type = i;
        }
    }
    if (type < 0 || argc > 3 || (argc == 3 && (type != U_THREAD_DEVICE_TYPE_SLEEPY_END_DEVICE ||
                                               !ucxapp_parse_int(argv[2], &poll) || poll < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    r = argc == 3 ? uCxThreadSetDeviceTypePollPeriod(app->ucx, (uThreadDeviceType_t)type, poll)
                  : uCxThreadSetDeviceType(app->ucx, (uThreadDeviceType_t)type);
    if (r != 0) {
        return failed("setting the device type", r);
    }
    ucxapp_print("Type %s set\n", kTypes[type]);
    return UCXAPP_OK;
}

static int cmdThreadScan(ucxapp_t *app, int argc, char *argv[])
{
    uCxThreadScan_t s;
    int n = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("Scanning the 802.15.4 channels...\n");
    uCxThreadScanBegin(app->ucx);
    while (uCxThreadScanGetNext(app->ucx, &s)) {
        char ext[24];
        hexText(&s.ext_pan_id, ext, sizeof(ext));
        ucxapp_print("%2d  %-16s ch%-2d  pan 0x%04x  ext %s  %4d dBm  lqi %3d%s\n", ++n,
                     s.network_name != NULL ? s.network_name : "", (int)s.channel, (unsigned)s.pan_id, ext,
                     (int)s.rssi, (int)s.lqi, s.joinable ? "  joinable" : "");
    }
    {
        int32_t r = uCxEnd(app->ucx);
        if (r != 0) {
            return failed("scan", r);
        }
    }
    ucxapp_print("%d network(s)\n", n);
    return UCXAPP_OK;
}

/* Joiner: a commissioner (border router or phone app) admits the module with its PSKd. */
static int cmdThreadJoin(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kResults[] = { "joined", "wrong PSKd (security)", "no commissioner found",
                                            "timed out", "failed" };
    int32_t waitS = THREAD_JOIN_WAIT_S, r;
    int32_t result;
    size_t n;

    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        r = uCxThreadJoinerStop(app->ucx);
        return r != 0 ? failed("stopping the joiner", r) : (ucxapp_print("Joiner stopped\n"), UCXAPP_OK);
    }
    if (argc < 2 || argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &waitS) || waitS < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    n = strlen(argv[1]);
    for (size_t i = 0; i < n; i++) {
        argv[1][i] = (char)toupper((unsigned char)argv[1][i]);
        if (!isalnum((unsigned char)argv[1][i]) || strchr("IOQZ", argv[1][i]) != NULL) {
            n = 0;
        }
    }
    if (n < 6 || n > 32) {
        ucxapp_print("ERROR: the PSKd is 6-32 letters and digits, without I, O, Q and Z\n");
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_clear(URC_FLAG_THREAD_JOINER);
    r = uCxThreadJoinerStart(app->ucx, argv[1]);
    if (r != 0) {
        return failed("starting the joiner", r);
    }
    ucxapp_print("Joining with PSKd %s; admit it on the commissioner (up to %d s)...\n", argv[1], (int)waitS);
    if (!ucxapp_events_wait(URC_FLAG_THREAD_JOINER, (uint32_t)waitS)) {
        uCxThreadJoinerStop(app->ucx);
        ucxapp_print("ERROR: no answer from a commissioner after %d s\n", (int)waitS);
        return UCXAPP_ERR_FAILED;
    }
    result = ucxapp_thread_joiner_result();
    if (result != U_THREAD_JOINER_RESULT_SUCCESS) {
        ucxapp_print("ERROR: join %s\n", result >= 0 && result < 5 ? kResults[result] : "failed");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Joined; thread.start attaches to the network\n");
    return UCXAPP_OK;
}

static int cmdThreadAddr(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kOrigin[] = { "thread", "slaac", "dhcpv6", "manual" };
    uCxThreadListAddresses_t a;
    int n = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxThreadListAddressesBegin(app->ucx);
    while (uCxThreadListAddressesGetNext(app->ucx, &a)) {
        char ip[48] = "";
        uCxIpAddressToString(&a.ipv6_addr, ip, sizeof(ip));
        ucxapp_print("%-40s %s\n", ip, a.addr_origin >= 0 && a.addr_origin < 4 ? kOrigin[a.addr_origin] : "?");
        n++;
    }
    uCxEnd(app->ucx);
    if (n == 0) {
        ucxapp_print("No addresses (thread.start first)\n");
    }
    return UCXAPP_OK;
}

static int cmdThreadNeighbors(ucxapp_t *app, int argc, char *argv[])
{
    uCxThreadListNeighbors_t nb;
    int n = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxThreadListNeighborsBegin(app->ucx);
    while (uCxThreadListNeighborsGetNext(app->ucx, &nb)) {
        char ext[24];
        hexText(&nb.ext_addr, ext, sizeof(ext));
        ucxapp_print("%-16s rloc 0x%04x  %-6s %4d dBm  lq %d\n", ext, (unsigned)nb.rloc16,
                     nb.neighbor_role == U_THREAD_NEIGHBOR_ROLE_ROUTER ? "router" : "child", (int)nb.rssi, (int)nb.link_quality);
        n++;
    }
    uCxEnd(app->ucx);
    ucxapp_print("%d neighbour(s)\n", n);
    return UCXAPP_OK;
}

static int cmdThreadRouters(ucxapp_t *app, int argc, char *argv[])
{
    uCxThreadListRouters_t rt;
    int n = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxThreadListRoutersBegin(app->ucx);
    while (uCxThreadListRoutersGetNext(app->ucx, &rt)) {
        char ext[24];
        hexText(&rt.ext_addr, ext, sizeof(ext));
        ucxapp_print("id %-2d rloc 0x%04x  %-16s next hop %-2d cost %d  lq %d/%d  age %d s%s\n", (int)rt.router_id,
                     (unsigned)rt.rloc16, ext, (int)rt.next_hop, (int)rt.path_cost, (int)rt.link_quality,
                     (int)rt.link_quality_out, (int)rt.age, rt.link_established ? "  linked" : "");
        n++;
    }
    uCxEnd(app->ucx);
    ucxapp_print("%d router(s)\n", n);
    return UCXAPP_OK;
}

static int cmdThreadLeader(ucxapp_t *app, int argc, char *argv[])
{
    uCxThreadGetLeaderData_t ld;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!uCxThreadGetLeaderDataBegin(app->ucx, &ld)) {
        int32_t r = uCxEnd(app->ucx);
        return failed("reading the leader data (attached?)", r);
    }
    printHex("Partition:     ", &ld.partition_id);
    ucxapp_print("Leader router: %d\n", (int)ld.leader_router_id);
    ucxapp_print("Weighting:     %d\n", (int)ld.weighting);
    ucxapp_print("Data version:  %d (stable %d)\n", (int)ld.data_version, (int)ld.stable_data_version);
    uCxEnd(app->ucx);
    return UCXAPP_OK;
}

/* Only the counters that say something about the link; ids follow uThreadCounterId_t. */
static const char *counterName(int32_t id)
{
    static const struct {
        int32_t id;
        const char *name;
    } kNames[] = {
        { U_THREAD_COUNTER_ID_ATTACH_ATTEMPTS, "attach attempts" },
        { U_THREAD_COUNTER_ID_PARTITION_ID_CHANGES, "partition changes" },
        { U_THREAD_COUNTER_ID_PARENT_CHANGES, "parent changes" },
        { U_THREAD_COUNTER_ID_TX_TOTAL, "tx frames" },
        { U_THREAD_COUNTER_ID_TX_RETRY, "tx retries" },
        { U_THREAD_COUNTER_ID_TX_ERR_CCA, "tx busy channel (CCA)" },
        { U_THREAD_COUNTER_ID_RX_TOTAL, "rx frames" },
        { U_THREAD_COUNTER_ID_RX_ERR_SEC, "rx security errors" },
        { U_THREAD_COUNTER_ID_RX_ERR_FCS, "rx FCS errors" },
    };

    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].id == id) {
            return kNames[i].name;
        }
    }
    return NULL;
}

static int cmdThreadCounters(ucxapp_t *app, int argc, char *argv[])
{
    uCxThreadListCounters_t c;
    bool all = false;

    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        int32_t r = uCxThreadResetCounters(app->ucx);
        return r != 0 ? failed("resetting the counters", r) : (ucxapp_print("Counters reset\n"), UCXAPP_OK);
    }
    if (argc > 2 || (argc == 2 && !(all = strcmp(argv[1], "all") == 0))) {
        return UCXAPP_ERR_USAGE;
    }
    uCxThreadListCountersBegin(app->ucx);
    while (uCxThreadListCountersGetNext(app->ucx, &c)) {
        const char *name = counterName(c.counter_id);
        if (name != NULL) {
            ucxapp_print("%-24s %d\n", name, (int)c.counter_value);
        } else if (all) {
            ucxapp_print("counter %-16d %d\n", (int)c.counter_id, (int)c.counter_value);
        }
    }
    uCxEnd(app->ucx);
    return UCXAPP_OK;
}

static const char *srpStateName(int32_t s)
{
    static const char *const kStates[] = { "registering", "registered", "removing", "removed", "failed" };

    return s >= 0 && s < 5 ? kStates[s] : "unknown";
}

/* SRP: register this module's services with the border router so the rest of the network finds them. */
static int cmdThreadSrp(ucxapp_t *app, int argc, char *argv[])
{
    int32_t r, id, port;

    if (argc == 1) {
        uEnabled_t on = U_ENABLED_NO;
        const char *host = NULL;
        uCxThreadSrpServiceList_t s;
        int n = 0;

        r = uCxThreadSrpGetEnabled(app->ucx, &on);
        if (r != 0) {
            return failed("reading the SRP client", r);
        }
        ucxapp_print("SRP client:    %s\n", on == U_ENABLED_YES ? "on" : "off");
        if (uCxThreadSrpGetHostnameBegin(app->ucx, &host) && host != NULL) {
            ucxapp_print("Host name:     %s\n", host[0] != '\0' ? host : "-");
        }
        uCxEnd(app->ucx);
        uCxThreadSrpServiceListBegin(app->ucx);
        while (uCxThreadSrpServiceListGetNext(app->ucx, &s)) {
            ucxapp_print("%2d  %s.%s port %d  %s\n", (int)s.service_id, s.instance_name != NULL ? s.instance_name : "",
                         s.service_type != NULL ? s.service_type : "", (int)s.port, srpStateName(s.srp_state));
            n++;
        }
        uCxEnd(app->ucx);
        ucxapp_print("%d service(s)\n", n);
        return UCXAPP_OK;
    }
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        r = uCxThreadSrpSetEnabled(app->ucx, argv[1][1] == 'n' ? U_ENABLED_YES : U_ENABLED_NO);
        return r != 0 ? failed("setting the SRP client", r) : (ucxapp_print("SRP client %s\n", argv[1]), UCXAPP_OK);
    }
    if (argc == 3 && strcmp(argv[1], "host") == 0) {
        r = uCxThreadSrpSetHostname(app->ucx, argv[2]);
        return r != 0 ? failed("setting the host name", r) : (ucxapp_print("Host name %s\n", argv[2]), UCXAPP_OK);
    }
    if (argc == 5 && strcmp(argv[1], "add") == 0 && ucxapp_parse_int(argv[3], &port) && port >= 1 && port <= 65535) {
        r = uCxThreadSrpServiceAdd(app->ucx, argv[2], port, argv[4], &id);
        return r != 0 ? failed("adding the service", r) : (ucxapp_print("Service %d added\n", (int)id), UCXAPP_OK);
    }
    if (argc == 3 && strcmp(argv[1], "remove") == 0 && ucxapp_parse_int(argv[2], &id)) {
        r = uCxThreadSrpServiceRemove(app->ucx, id);
        return r != 0 ? failed("removing the service", r) : (ucxapp_print("Service %d removed\n", (int)id), UCXAPP_OK);
    }
    if (argc == 5 && strcmp(argv[1], "txt") == 0 && ucxapp_parse_int(argv[2], &id)) {
        r = uCxThreadSrpTxtRecordAdd(app->ucx, id, argv[3], argv[4]);
        return r != 0 ? failed("adding the TXT record", r) : (ucxapp_print("TXT %s=%s on service %d\n", argv[3], argv[4], (int)id), UCXAPP_OK);
    }
    return UCXAPP_ERR_USAGE;
}

/* DNS over the Thread network: browse service instances, resolve one, or a host name to IPv6. */
static int cmdThreadDns(ucxapp_t *app, int argc, char *argv[])
{
    int n = 0;

    if (argc == 3 && strcmp(argv[1], "browse") == 0) {
        const char *name = NULL;
        uCxThreadDnsBrowseBegin(app->ucx, argv[2]);
        while (uCxThreadDnsBrowseGetNext(app->ucx, &name)) {
            ucxapp_print("%s\n", name != NULL ? name : "");
            n++;
        }
    } else if (argc == 4 && strcmp(argv[1], "resolve") == 0) {
        uCxThreadDnsResolve_t res;
        if (uCxThreadDnsResolveBegin(app->ucx, argv[2], argv[3], &res)) {
            char ip[48] = "";
            uCxIpAddressToString(&res.ipv6_addr, ip, sizeof(ip));
            ucxapp_print("%s port %d  %s\n", res.dns_hostname != NULL ? res.dns_hostname : "", (int)res.port, ip);
            n++;
        }
    } else if (argc == 3 && strcmp(argv[1], "host") == 0) {
        uSockIpAddress_t addr;
        uCxThreadDnsResolveHostBegin(app->ucx, argv[2]);
        while (uCxThreadDnsResolveHostGetNext(app->ucx, &addr)) {
            char ip[48] = "";
            uCxIpAddressToString(&addr, ip, sizeof(ip));
            ucxapp_print("%s\n", ip);
            n++;
        }
    } else {
        return UCXAPP_ERR_USAGE;
    }
    {
        int32_t r = uCxEnd(app->ucx);
        if (r != 0 && n == 0) {
            return failed("DNS lookup", r);
        }
    }
    ucxapp_print("%d result(s)\n", n);
    return n > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* ICMPv6 echo over Thread. "leader" pings the partition leader's anycast locator (mesh-local prefix + 0:ff:fe00:fc00). */
static int cmdThreadPing(ucxapp_t *app, int argc, char *argv[])
{
    int32_t count = 4;
    char target[48];

    if (argc < 2 || argc > 3 ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &count) || count < 1 || count > 10))) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "leader") == 0) {
        uCxThreadListAddresses_t a;
        char prefix[48] = "";

        uCxThreadListAddressesBegin(app->ucx);
        while (uCxThreadListAddressesGetNext(app->ucx, &a)) {
            char ip[48] = "";
            uCxIpAddressToString(&a.ipv6_addr, ip, sizeof(ip));
            const char *start = ip + (ip[0] == '[');
            const char *rloc = strstr(start, ":0000:00ff:fe00:");
            /* the RLOC is <mesh-local /64>:0000:00ff:fe00:<rloc16> */
            if (prefix[0] == '\0' && rloc != NULL && rloc - start < (int)sizeof(prefix) - 1) {
                snprintf(prefix, sizeof(prefix), "%.*s", (int)(rloc - start), start);
            }
        }
        uCxEnd(app->ucx);
        if (prefix[0] == '\0') {
            ucxapp_print("ERROR: no mesh-local address; thread.start first\n");
            return UCXAPP_ERR_FAILED;
        }
        snprintf(target, sizeof(target), "%s:0:ff:fe00:fc00", prefix);
    } else {
        snprintf(target, sizeof(target), "%s", argv[1]);
    }
    ucxapp_events_clear(URC_FLAG_PING_COMPLETE);
    int32_t r = uCxDiagnosticsPing2(app->ucx, target, count);
    if (r != 0) {
        return failed("starting the ping", r);
    }
    if (!ucxapp_events_wait(URC_FLAG_PING_COMPLETE, (uint32_t)count * 2 + 5)) {
        ucxapp_print("ERROR: ping timeout\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s: sent %d, received %d, average %d ms\n", target,
                 (int)gUcxappPingSent, (int)gUcxappPingReceived, (int)gUcxappPingAvgMs);
    return gUcxappPingReceived > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gThreadCmds[] = {
    { "thread.status",    "",                 "Thread on or off, network, role, RLOC16 and partition",            UCXAPP_CMD_NEEDS_DEVICE, cmdThreadStatus },
    { "thread.start",     "[wait_s=30]",      "Turn Thread on and wait until it is attached (0: do not wait)",     UCXAPP_CMD_NEEDS_DEVICE, cmdThreadStart },
    { "thread.stop",      "",                 "Turn Thread off",                                                   UCXAPP_CMD_NEEDS_DEVICE, cmdThreadStop },
    { "thread.network",   "[new|<name> <channel> <pan_id> <ext_pan_id> <network_key>]", "Show the network credentials, make new random ones, or set them (channel 11-26, ext_pan_id 16 hex digits, key 32)", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadNetwork },
    { "thread.dataset",   "[<hex>|clear]",    "Show or set the active operational dataset as TLV hex (border router: ot-ctl dataset active -x)", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadDataset },
    { "thread.type",      "[router|end|sleepy [poll_ms]]", "Show or set the device type: router (routes for others), end device, or sleepy end device polling its parent", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadType },
    { "thread.scan",      "",                 "Scan for Thread networks nearby",                                   UCXAPP_CMD_NEEDS_DEVICE, cmdThreadScan },
    { "thread.join",      "<pskd> [wait_s=120]|stop", "Join through a commissioner with this PSKd (joiner), or stop joining", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadJoin },
    { "thread.addr",      "",                 "This module's IPv6 addresses on the Thread network",               UCXAPP_CMD_NEEDS_DEVICE, cmdThreadAddr },
    { "thread.neighbors", "",                 "Neighbours: extended address, RLOC16, role, RSSI and link quality", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadNeighbors },
    { "thread.routers",   "",                 "Router table: ids, next hops, path costs and link quality",         UCXAPP_CMD_NEEDS_DEVICE, cmdThreadRouters },
    { "thread.leader",    "",                 "The partition's leader data",                                       UCXAPP_CMD_NEEDS_DEVICE, cmdThreadLeader },
    { "thread.counters",  "[all|reset]",      "MAC and MLE counters (attach attempts, retries, errors); all lists every counter", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadCounters },
    { "thread.srp",       "[on|off|host <name>|add <_type._udp> <port> <instance>|remove <id>|txt <id> <key> <value>]", "SRP client: register services with the border router so others find them over DNS-SD", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadSrp },
    { "thread.ping",      "<ipv6>|leader [count=4]", "ICMPv6 ping over Thread (count 1-10); leader pings the partition leader, an off-mesh host goes through the border router (IPv4 via its NAT64 prefix)", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadPing },
    { "thread.dns",       "browse <_type._udp>|resolve <instance> <_type._udp>|host <name>", "DNS-SD over Thread: browse services, resolve an instance, or a host name to IPv6", UCXAPP_CMD_NEEDS_DEVICE, cmdThreadDns },
};

void ucxapp_register_thread_cmds(void)
{
    ucxapp_cmd_register(gThreadCmds, sizeof(gThreadCmds) / sizeof(gThreadCmds[0]));
}
