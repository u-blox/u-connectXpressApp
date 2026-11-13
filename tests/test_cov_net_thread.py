"""thread.*, conn.status, coap.get, mqtt.*, sock.* and the certificate files against vucx profiles that answer OK,
ERROR or odd replies, and against hand-made PKCS#12, PKCS#7 and PEM files (offline coverage)."""

import base64
import fnmatch
import hashlib
import re
import subprocess

import pytest

pytestmark = pytest.mark.app

THREAD = "nora-b26-thread.yaml"
WIFI = "wifi.yaml"
NORA = "nora-w36.yaml"
ERROR = ["ERROR"]
REFUSED = "(-1: U_ERROR_COMMON_BSD_ERROR)"   # how a plain ERROR answer shows in a message
MQTT_URCS = {"AT+UMQC=*": ["+UEMQC:0"], "AT+UMQS=*": ["+UEMQSC:0,0"]}


def _profile(base, answers=None, urcs=None, state=None):
    """A stock profile that says OK to what it does not know, with some answers, URCs or state setters replaced
    (an empty URC list removes the URC). The replacements come first, so they win over the stock patterns."""
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load(base)
    profile["default"] = ["OK"]
    for section, new in (("commands", answers), ("urcs", urcs), ("state", state)):
        stock = profile.get(section) or {}
        profile[section] = {**(new or {}), **{key: value for key, value in stock.items() if key not in (new or {})}}
    return profile


def _run(exe, profile, cwd, args, text=None, timeout=40):
    """The app on a virtual module with this profile; returns (result, module) with the module stopped."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx(profile)
    port = module.start()
    stdin = {"input": text} if text is not None else {"stdin": subprocess.DEVNULL}
    try:
        out = subprocess.run([str(exe), port, *args], capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=timeout, cwd=cwd, **stdin)
    except subprocess.TimeoutExpired as e:
        seen = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"hung after {timeout} s:\n{seen[-3000:]}\nlast AT: {module.log[-15:]}")
    finally:
        module.stop()
    return out, module


def _exec(exe, profile, cwd, *commands, timeout=40):
    """Runs the commands with --exec: the app stops at the first one that fails and exits with 1."""
    return _run(exe, profile, cwd, [arg for command in commands for arg in ("--exec", command)], timeout=timeout)


def _shell(exe, profile, cwd, commands, timeout=40):
    """Types the commands into the command shell as UTF-8: every one runs, whatever the one before did."""
    return _run(exe, profile, cwd, ["--shell"], text="\n".join(commands) + "\n", timeout=timeout)


def _offline(exe, cwd, *commands):
    """Commands that need no module."""
    args = [arg for command in commands for arg in ("--exec", command)]
    return subprocess.run([str(exe), "--offline", *args], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=40, cwd=cwd, stdin=subprocess.DEVNULL)


def _store(tmp_path, name, data):
    """A file in the app's storage folder (the app runs in tmp_path); returns the name to give the app."""
    path = tmp_path / "storage" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return name


def _asked(module, answers):
    """Every command the test gave an answer for reached the module."""
    for pattern in answers:
        assert any(fnmatch.fnmatchcase(sent, pattern) for sent in module.log), (pattern, module.log)


# ---- thread.*: a NORA-B26 with Thread firmware ----

NETWORK = "thread.network lab 15 0x1234 0xdead00beef00cafe 00112233445566778899aabbccddeeff"


def test_thread_commands_send_their_at_command_and_say_what_they_did(app_exe, tmp_path):
    """Stop, dataset clear, typed-in credentials (0x allowed), start without waiting, joiner stop, SRP remove and
    TXT, every counter, and a router or end device type each map to one AT command and one line of text."""
    profile = _profile(THREAD, {"AT+UTHDT?": [["+UTHDT:0,0", "OK"], ["+UTHDT:1,0", "OK"]]})
    out, module = _shell(app_exe, profile, tmp_path, [
        "thread.stop", "thread.dataset clear", NETWORK, "thread.start 0", "thread.join stop", "thread.srp remove 1",
        "thread.srp txt 1 temp 21.5", "thread.counters all", "thread.type", "thread.type"])
    for text in ("Thread off", "Dataset cleared", "Network lab on channel 15 set; thread.start joins it",
                 "Thread starting", "Joiner stopped", "Service 1 removed", "TXT temp=21.5 on service 1",
                 "Type:          router (can route for others)", "Type:          end (never routes)"):
        assert text in out.stdout, f"{text!r} missing:\n{out.stdout[-3000:]}"
    assert re.search(r"^counter 30 +7$", out.stdout, re.M), out.stdout[-3000:]
    for sent in ("AT+UTHE=0", "AT+UTHDSC", 'AT+UTHNC="lab",15,4660,DEAD00BEEF00CAFE,00112233445566778899AABBCCDDEEFF',
                 "AT+UTHE=1", "AT+UTHJSP", "AT+UTHSSR=1", 'AT+UTHSTRA=1,"temp","21.5"'):
        assert sent in module.log, (sent, module.log)


PINGS = {
    "leader": ("thread.ping leader 2", "+UEDGPC:2,2,0,7", 'AT+UDGP="fd00:0000:0000:0000:0:ff:fe00:fc00",2',
               "fd00:0000:0000:0000:0:ff:fe00:fc00: sent 2, received 2, average 7 ms", 0),
    "address": ("thread.ping fd00::1", "+UEDGPC:4,3,25,12", 'AT+UDGP="fd00::1",4',
                "fd00::1: sent 4, received 3, average 12 ms", 0),
    "lost": ("thread.ping fd00::1 1", "+UEDGPC:1,0,100,0", 'AT+UDGP="fd00::1",1',
             "fd00::1: sent 1, received 0, average 0 ms", 1),
}


@pytest.mark.parametrize("case", PINGS)
def test_thread_ping_reports_the_replies(app_exe, tmp_path, case):
    """thread.ping pings the address it is given, or the leader's anycast locator built from the mesh-local prefix
    of the module's own RLOC address, prints the counts the module reports and fails when no reply came back."""
    command, urc, sent, text, code = PINGS[case]
    out, module = _exec(app_exe, _profile(THREAD, urcs={"AT+UDGP=*": [urc]}), tmp_path, command)
    assert sent in module.log, module.log
    assert text in out.stdout and out.returncode == code, (out.returncode, out.stdout)


# command, answers of the module, URCs of the module, the message the user gets
THREAD_ERRORS = {
    "start": ("thread.start", {"AT+UTHE=1": ERROR}, None, f"ERROR: starting Thread failed {REFUSED}"),
    "stop": ("thread.stop", {"AT+UTHE=0": ERROR}, None, f"ERROR: stopping Thread failed {REFUSED}"),
    "network-new": ("thread.network new", {"AT+UTHDSN": ERROR}, None,
                    f"ERROR: creating a new network failed {REFUSED}"),
    "network-set": (NETWORK, {"AT+UTHNC=*": ERROR}, None, f"ERROR: setting the network credentials failed {REFUSED}"),
    "dataset-clear": ("thread.dataset clear", {"AT+UTHDSC": ERROR}, None,
                      f"ERROR: clearing the dataset failed {REFUSED}"),
    "dataset-set": ("thread.dataset 0e08000000000001", {"AT+UTHDS=*": ERROR}, None,
                    f"ERROR: setting the dataset failed {REFUSED}"),
    "type": ("thread.type", {"AT+UTHDT?": ERROR}, None, f"ERROR: reading the device type failed {REFUSED}"),
    "type-code": ("thread.type", {"AT+UTHDT?": ["ERROR:5"]}, None,
                  "ERROR: reading the device type failed (-5: U_ERROR_COMMON_INVALID_PARAMETER)"),
    "type-set": ("thread.type end", {"AT+UTHDT=*": ERROR}, None, f"ERROR: setting the device type failed {REFUSED}"),
    "scan": ("thread.scan", {"AT+UTHSC": ERROR}, None, f"ERROR: scan failed {REFUSED}"),
    "join-stop": ("thread.join stop", {"AT+UTHJSP": ERROR}, None, f"ERROR: stopping the joiner failed {REFUSED}"),
    "join-start": ("thread.join j01nme", {"AT+UTHJS=*": ERROR}, None, f"ERROR: starting the joiner failed {REFUSED}"),
    "leader": ("thread.leader", {"AT+UTHLD?": ERROR}, None,
               f"ERROR: reading the leader data (attached?) failed {REFUSED}"),
    "srp": ("thread.srp", {"AT+UTHSE?": ERROR}, None, f"ERROR: reading the SRP client failed {REFUSED}"),
    "dns": ("thread.dns browse _sensor._udp", {"AT+UTHDB=*": ERROR}, None, f"ERROR: DNS lookup failed {REFUSED}"),
    "ping": ("thread.ping fd00::1", {"AT+UDGP=*": ERROR}, None, f"ERROR: starting the ping failed {REFUSED}"),
    "ping-leader": ("thread.ping leader", {"AT+UTHA?": ["OK"]}, None,
                    "ERROR: no mesh-local address; thread.start first"),
    "start-timeout": ("thread.start 1", {}, {"AT+UTHE=1": []},
                      "ERROR: not attached after 1 s; check the network with thread.network and thread.scan"),
    "join-timeout": ("thread.join j01nme 1", {"AT+UTHJSP": ["OK"]}, {"AT+UTHJS=*": []},
                     "ERROR: no answer from a commissioner after 1 s"),
    "join-refused": ("thread.join j01nme 5", {}, {"AT+UTHJS=*": ["+UETHJ:1"]}, "ERROR: join wrong PSKd (security)"),
    "join-nobody": ("thread.join j01nme 5", {}, {"AT+UTHJS=*": ["+UETHJ:2"]}, "ERROR: join no commissioner found"),
    "ping-timeout": ("thread.ping fd00::1 1", {}, {"AT+UDGP=*": []}, "ERROR: ping timeout"),
}


@pytest.mark.parametrize("case", THREAD_ERRORS)
def test_thread_command_says_what_failed(app_exe, tmp_path, case):
    """A Thread command the module refuses, or whose event never comes, ends with its own error line and exit
    code 1; the line carries the module's error code with its name, also when a query is answered ERROR:<code>
    instead of its response line; a joiner nobody admitted is stopped again."""
    command, answers, urcs, message = THREAD_ERRORS[case]
    out, module = _exec(app_exe, _profile(THREAD, answers, urcs), tmp_path, command)
    assert message + "\n" in out.stdout and out.returncode == 1, (out.returncode, out.stdout)
    _asked(module, answers)


def test_a_query_without_any_answer_reports_the_timeout(app_exe, tmp_path):
    """Only a command the module does not answer at all (10 s) fails with the timeout code -65536, named and
    logged once; an ERROR answer never does."""
    out, module = _exec(app_exe, _profile(THREAD, {"AT+UTHDT?": [""]}), tmp_path, "thread.type")
    assert "ERROR: reading the device type failed (-65536: no answer from the module)\n" in out.stdout, out.stdout
    assert out.returncode == 1 and out.stdout.count("Command timeout") == 1, out.stdout
    assert module.log.count("AT+UTHDT?") == 1, module.log


def test_thread_status_says_when_the_module_stops_answering(app_exe, tmp_path):
    """The first AT+UTHE? tells the app the module has Thread; when the next one fails thread.status says so."""
    profile = _profile(THREAD, {"AT+UTHE? [probed=yes]": ERROR}, state={"AT+UTHE?": {"probed": "yes"}})
    out, module = _exec(app_exe, profile, tmp_path, "thread.status")
    assert f"ERROR: reading the Thread state failed {REFUSED}\n" in out.stdout and out.returncode == 1, out.stdout
    assert module.log.count("AT+UTHE?") == 2, module.log


THREAD_NOTES = {
    "status": ("thread.status", {"AT+UTHNC?": ERROR}, "Network:       none (thread.network or thread.dataset)"),
    "network": ("thread.network", {"AT+UTHNC?": ERROR},
                "No network set: thread.network new, or thread.network <name> <channel> <pan_id> <ext_pan_id> <key>"),
    "dataset": ("thread.dataset", {"AT+UTHDS?": ERROR},
                "No active dataset: thread.network new, or thread.dataset <hex> from the border router"),
    "addr": ("thread.addr", {"AT+UTHA?": ["OK"]}, "No addresses (thread.start first)"),
}


@pytest.mark.parametrize("case", THREAD_NOTES)
def test_thread_says_what_to_do_when_nothing_is_set(app_exe, tmp_path, case):
    """A module without network, dataset or addresses is not an error: the command names the next step."""
    command, answers, text = THREAD_NOTES[case]
    out, module = _exec(app_exe, _profile(THREAD, answers), tmp_path, command)
    assert text in out.stdout and out.returncode == 0, (out.returncode, out.stdout)
    _asked(module, answers)


def test_thread_status_without_a_role_shows_state_and_network_only(app_exe, tmp_path):
    """Thread on but AT+UTHS? refused: thread.status still shows the state and the network, and no role lines."""
    profile = _profile(THREAD, {"AT+UTHE?": ["+UTHE:1", "OK"], "AT+UTHS?": ERROR})
    out, module = _exec(app_exe, profile, tmp_path, "thread.status")
    assert out.returncode == 0 and "AT+UTHS?" in module.log, (out.stdout, module.log)
    assert re.search(r"^Thread: +on$", out.stdout, re.M) and re.search(r"^Network: +vucx-mesh$", out.stdout, re.M)
    assert "Role:" not in out.stdout and "RLOC16:" not in out.stdout, out.stdout


@pytest.mark.parametrize("command", [
    "thread.status now", "thread.start -1", "thread.stop now", "thread.dataset a b", "thread.dataset abc",
    "thread.scan now", "thread.join", "thread.join j0inme", "thread.addr now", "thread.neighbors now",
    "thread.routers now", "thread.leader now", "thread.counters some", "thread.dns lookup host", "thread.ping",
    "thread.ping leader 11", "thread.network lab 15 0x1234 dead00beef00cafe 0011",
    "thread.network lab 15 0x1234 dead 00112233445566778899aabbccddeeff",
])
def test_thread_usage_errors_send_nothing(app_exe, tmp_path, command):
    """A wrong argument (too many, odd hex, a PSKd with I or O, a short key) prints the usage and the module only
    gets the AT+UTHE? that tells the app it has Thread."""
    out, module = _exec(app_exe, _profile(THREAD), tmp_path, command)
    assert f"usage: {command.split()[0]}" in out.stdout and out.returncode != 0, (out.returncode, out.stdout)
    assert [c for c in module.log if c.startswith(("AT+UTH", "AT+UDGP"))] == ["AT+UTHE?"], module.log


def test_dev_summary_lists_thread_as_a_radio_while_it_is_off(app_exe, tmp_path):
    """Thread firmware with Thread off is a radio in dev.summary, without a role line."""
    out, _ = _exec(app_exe, _profile(THREAD), tmp_path, "dev.summary")
    assert out.returncode == 0 and re.search(r"^Radios: +Bluetooth LE, Thread$", out.stdout, re.M), out.stdout
    assert not re.search(r"^Thread:", out.stdout, re.M), out.stdout


# ---- conn.status ----

def test_conn_status_shows_the_thread_role_and_neighbours(app_exe, tmp_path):
    """An attached Thread module gets a t| line with role and RLOC16 and an n| line per neighbour with its
    RLOC16, role (child or router, as thread.neighbors names it), RSSI and link quality, and the same as rows of
    the table."""
    neighbours = ["+UTHNL:8877665544332211,2048,0,-60,3", "+UTHNL:1122334455667788,1024,1,-55,2", "OK"]
    out, module = _exec(app_exe, _profile(THREAD, {"AT+UTHNL?": neighbours}), tmp_path, "thread.start 5",
                        "conn.status raw", "conn.status", "thread.neighbors")
    assert out.returncode == 0 and module.log.count("AT+UTHNL?") == 3, (out.stdout, module.log)
    for text in ("t|leader|0400\nn|0800|child|-60|3\nn|0400|router|-55|2\n",
                 "  thread  leader     rloc16 0400\n    nbr   0800 child     -60 dBm  lq 3\n"
                 "    nbr   0400 router    -55 dBm  lq 2\n",
                 "8877665544332211 rloc 0x0800  child   -60 dBm  lq 3\n"
                 "1122334455667788 rloc 0x0400  router  -55 dBm  lq 2\n2 neighbour(s)\n"):
        assert text in out.stdout, f"{text!r} missing:\n{out.stdout}"


def test_conn_status_lists_access_point_socket_peer_broker_and_gnss_receiver(app_exe, tmp_path):
    """An access point with two stations, a connected socket with its far end, the MQTT broker with the last
    topics and a GNSS receiver on the host each get their line in conn.status raw, and a row in the table."""
    from harness.vucx import Feeder

    profile = _profile(NORA, {
        "AT+UWAPNST=0": ["+UWAPNST:0,192.168.0.1", "OK"],
        "AT+UWAPCS?": ["+UWAPCS:AABBCCDDEE01", "+UWAPCS:AABBCCDDEE02", "OK"],
        "AT+USOPA=0": ["+USOPA:0,93.184.216.34,80", "OK"],
        "AT+UMQCP=0": ['+UMQCP:0,"broker.local",1883,"plant7",""', "OK"]})
    gnss = Feeder("$GNGLL,5546.1234,N,01301.5678,E,123519.00,A,A*73\r\n", 0.2)
    port = gnss.start()
    try:
        out, module = _exec(app_exe, profile, tmp_path, "sock.open tcp", "sock.connect 0 example.com 80",
                            "mqtt.connect broker.local id=plant7", "mqtt.subscribe a/# 1", "mqtt.publish a/b hi",
                            f"gnss.open {port} 9600", "conn.status raw", "conn.status")
    finally:
        gnss.stop()
    assert out.returncode == 0, out.stdout[-3000:]
    for line in ("a|vucxap|6|192.168.0.1|2|AA:BB:CC:DD:EE:01,AA:BB:CC:DD:EE:02", "s|0|tcp|conn|93.184.216.34:80",
                 "q|broker.local:1883|plant7|a/#|a/b", f"g|{port}|",
                 "  ap      vucxap               ch 6   192.168.0.1, 2 station(s) AA:BB:CC:DD:EE:01,AA:BB:CC:DD:EE:02",
                 "  sock 0  tcp conn   93.184.216.34:80", f"  gnss    {port} ",
                 "  mqtt    broker.local:1883 as plant7, subscribed to a/#, publishing to a/b\n"):
        assert line in out.stdout, f"{line!r} missing:\n{out.stdout[-3000:]}"
    _asked(module, ["AT+UWAPNST=0", "AT+UWAPCP?", "AT+UWAPCS?", "AT+USOPA=0", "AT+UMQCP=0"])


MQTT_ROWS = {
    "connected": (["mqtt.connect broker.local id=plant7"], "  mqtt    broker.local:1883 as plant7\n"),
    "published": (["mqtt.connect broker.local id=plant7", "mqtt.publish a/b hi"],
                  "  mqtt    broker.local:1883 as plant7, publishing to a/b\n"),
    "unsubscribed": (["mqtt.connect broker.local id=plant7", "mqtt.subscribe a/# 1", "mqtt.unsubscribe a/#"],
                     "  mqtt    broker.local:1883 as plant7\n"),
}


@pytest.mark.parametrize("case", MQTT_ROWS)
def test_conn_status_table_leaves_out_the_mqtt_topics_there_are_none_of(app_exe, tmp_path, case):
    """The mqtt row names broker and client id, and a topic only once one was subscribed to (and not given up
    again) or published to."""
    commands, row = MQTT_ROWS[case]
    profile = _profile(NORA, {"AT+UMQCP=0": ['+UMQCP:0,"broker.local",1883,"plant7",""', "OK"]})
    out, _ = _exec(app_exe, profile, tmp_path, *commands, "conn.status")
    assert out.returncode == 0 and row in out.stdout, out.stdout


def test_conn_status_keeps_the_broker_line_when_its_settings_cannot_be_read(app_exe, tmp_path):
    """Connected to a broker but AT+UMQCP=0 gives nothing: the q| line has empty broker and client fields and
    still the topic subscribed to, and the table row calls it just "broker"."""
    out, module = _exec(app_exe, _profile(NORA), tmp_path, "mqtt.connect broker.local", "mqtt.subscribe a/# 1",
                        "conn.status raw", "conn.status")
    assert out.returncode == 0 and module.log.count("AT+UMQCP=0") == 2, (out.stdout, module.log)
    assert re.search(r"^q\|\|\|a/#\|$", out.stdout, re.M), out.stdout
    assert "  mqtt    broker, subscribed to a/#\n" in out.stdout, out.stdout


# ---- coap.get: the datagrams a module hands over, as binary answers to AT+USORFB ----

COAP_ACK = "\x60\x45\x4b\x1d"   # version 1, acknowledgement, 2.05 Content, the message id coap.get uses


def _datagram(data):
    return ["+USORFB:0,192.168.1.10,5683\x01" + data, "OK"]


def test_coap_get_skips_datagrams_that_are_not_its_answer(app_exe, tmp_path):
    """Nothing to read yet, a short datagram, another CoAP version, a token longer than 8, another message id, a
    token cut off, a reserved option nibble and an option of 269 bytes that runs past the end are all passed
    over; the answer with options of extended number and of 15 bytes (13 + 2) is printed with its payload,
    unprintable bytes as dots."""
    answer = COAP_ACK + "\xd1\x00a" + "\xe0\x00\x00" + "\x0d\x02" + "b" * 15 + "\xff21.5\x07C"
    others = ["", "\x60\x45", "\x20\x45\x4b\x1d", "\x69\x45\x4b\x1d", "\x60\x45\x00\x01", "\x64\x45\x4b\x1d",
              COAP_ACK + "\xf0", COAP_ACK + "\x0e\x00\x00"]
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,13", "OK"],
                              "AT+USORFB=*": [_datagram(data) for data in others + [answer]]})
    out, module = _exec(app_exe, profile, tmp_path, "coap.get 192.168.1.10 5683 /temp/now")
    assert out.returncode == 0, out.stdout
    assert "coap 2.05 from 192.168.1.10:5683/temp/now\n21.5.C\n" in out.stdout, out.stdout
    assert module.payloads == [("AT+USOWB=0", b"\x40\x01\x4b\x1d\xb4temp\x03now")], module.payloads
    assert module.log.count("AT+USORFB=0,256") == len(others) + 1 and "AT+USOCL=0" in module.log, module.log


def test_coap_get_prints_an_answer_without_payload(app_exe, tmp_path):
    """Without a path the request has no Uri-Path option, and a 2.04 answer with no payload is one code line."""
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,4", "OK"], "AT+USORFB=*": _datagram("\x60\x44\x4b\x1d")})
    out, module = _exec(app_exe, profile, tmp_path, "coap.get 192.168.1.10 5683")
    assert out.returncode == 0 and "coap 2.04 from 192.168.1.10:5683/\n\n" in out.stdout, out.stdout
    assert module.payloads == [("AT+USOWB=0", b"\x40\x01\x4b\x1d")], module.payloads


@pytest.mark.parametrize("path", ["temp", "/temp"])
def test_coap_get_reads_past_an_option_of_20_bytes(app_exe, tmp_path, path):
    """An option length of 13 means 13 plus the next byte: the payload after a 20 byte option is found. The
    answer is shown as host:port/path whether or not the path was typed with its slash, and the request is the
    same."""
    answer = COAP_ACK + "\x8d\x07" + "abcdefghijklmnopqrst" + "\xffok"
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,9", "OK"], "AT+USORFB=*": _datagram(answer)})
    out, module = _exec(app_exe, profile, tmp_path, f"coap.get 192.168.1.10 5683 {path}")
    assert out.returncode == 0 and "coap 2.05 from 192.168.1.10:5683/temp\nok\n" in out.stdout, out.stdout
    assert module.payloads == [("AT+USOWB=0", b"\x40\x01\x4b\x1d\xb4temp")], module.payloads
    assert module.log.count("AT+USORFB=0,256") == 1, module.log


COAP_CUT = {
    "length-13": "\x8d",              # the byte that completes the length is missing
    "length-14": "\x0e\x01",          # one of the two length bytes is missing
    "value": "\x8d\x07abc\xffok",     # 20 bytes announced, the datagram ends before them
    "number-13": "\xd0",              # the byte that completes the option number is missing
}


@pytest.mark.parametrize("case", COAP_CUT)
def test_coap_get_drops_an_answer_cut_off_inside_an_option(app_exe, tmp_path, case):
    """An answer that ends inside an extended option length, number or value is not taken for a reply (and
    nothing is read past its end): coap.get asks three times and fails with no CoAP reply."""
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,9", "OK"], "AT+USORFB=*": _datagram(COAP_ACK + COAP_CUT[case])})
    out, module = _exec(app_exe, profile, tmp_path, "coap.get 192.168.1.10 5683 temp")
    assert out.stdout.endswith("ERROR: no CoAP reply from 192.168.1.10:5683/temp\n"), out.stdout
    assert out.returncode == 1 and "coap 2.05" not in out.stdout, (out.returncode, out.stdout)
    assert module.payloads == [("AT+USOWB=0", b"\x40\x01\x4b\x1d\xb4temp")] * 3, module.payloads


COAP_ERRORS = {
    "port": ("coap.get 192.168.1.10 0", {}, "usage: coap.get <host> <port> [path]"),
    "segment": ("coap.get 192.168.1.10 5683 /averyveryverylongsegment", {},
                "ERROR: path too long, or a segment is longer than 12 bytes"),
    "socket": ("coap.get 192.168.1.10 5683 /temp", {"AT+USOCR=*": ERROR}, "ERROR: failed to create a UDP socket"),
    "connect": ("coap.get 192.168.1.10 5683 /temp", {"AT+USOC=*": ERROR, "AT+USOCL=0": ["OK"]},
                "ERROR: connect to 192.168.1.10:5683 failed"),
    "write": ("coap.get 192.168.1.10 5683 temp", {"AT+USOWB=*": ERROR, "AT+USOCL=0": ["OK"]},
              "ERROR: no CoAP reply from 192.168.1.10:5683/temp"),
}


@pytest.mark.parametrize("case", COAP_ERRORS)
def test_coap_get_says_what_failed(app_exe, tmp_path, case):
    """A bad port or path is refused before a socket is made; a socket that cannot be made, connected or written
    to is reported, and a socket that was made is closed again."""
    command, answers, message = COAP_ERRORS[case]
    out, module = _exec(app_exe, _profile(WIFI, answers), tmp_path, command)
    assert message in out.stdout and out.returncode != 0, (out.returncode, out.stdout)
    _asked(module, answers)
    if not answers:
        assert not any(sent.startswith("AT+USOCR") for sent in module.log), module.log


def test_coap_get_sends_the_request_three_times_before_giving_up(app_exe, tmp_path):
    """No answer: the confirmable request goes out three times, 2 s apart, then coap.get fails."""
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,9", "OK"], "AT+USORFB=*": ERROR})
    out, module = _exec(app_exe, profile, tmp_path, "coap.get 192.168.1.10 5683 /temp")
    assert "ERROR: no CoAP reply from 192.168.1.10:5683/temp" in out.stdout and out.returncode == 1, out.stdout
    assert module.payloads == [("AT+USOWB=0", b"\x40\x01\x4b\x1d\xb4temp")] * 3, module.payloads


# ---- mqtt.* ----

def test_mqtt_connect_with_only_a_user_name_or_only_a_ca(app_exe, tmp_path):
    """user= without pass= logs in with the name alone; ca= without a client certificate verifies the server and
    moves the default port to 8883."""
    out, module = _exec(app_exe, _profile(WIFI, urcs=MQTT_URCS), tmp_path,
                        "mqtt.connect broker.local id=plant7 user=me", "mqtt.connect broker.local id=plant7 ca=myca")
    assert out.returncode == 0, out.stdout
    assert "connected to broker.local:1883 as plant7, user me\n" in out.stdout, out.stdout
    assert "connected to broker.local:8883 as plant7, TLS\n" in out.stdout, out.stdout
    sent = [c for c in module.log if c.startswith("AT+UMQ")]
    assert sent == ['AT+UMQCP=0,"broker.local",1883,"plant7","me"', "AT+UMQTLS=0,0", "AT+UMQC=0",
                    'AT+UMQCP=0,"broker.local",8883,"plant7"', 'AT+UMQTLS=0,3,"myca"', "AT+UMQC=0"], sent


WILLS = {
    "retained": ('+UMQLWT:0,"dev/status","offline",1,1', "last will: 'offline' on dev/status (qos 1, retained)"),
    "plain": ('+UMQLWT:0,"dev/status","gone",0,0', "last will: 'gone' on dev/status (qos 0)"),
    "none": ('+UMQLWT:0,"","",0,0', "no last will"),
}


@pytest.mark.parametrize("case", WILLS)
def test_mqtt_will_shows_what_the_module_stores(app_exe, tmp_path, case):
    """mqtt.will without arguments prints the stored will with its QoS and retain flag, or that there is none."""
    stored, text = WILLS[case]
    out, _ = _exec(app_exe, _profile(WIFI, {"AT+UMQLWT=0": [stored, "OK"]}), tmp_path, "mqtt.will")
    assert out.returncode == 0 and text + "\n" in out.stdout, out.stdout


def test_mqtt_retries_while_the_module_is_busy(app_exe, tmp_path):
    """ERROR:15 (the previous MQTT operation is still in flight) is tried again instead of failing: subscribe,
    unsubscribe and publish each send their command a second time and then succeed."""
    busy_then = [["ERROR:15"], ["OK"]]
    profile = _profile(WIFI, {"AT+UMQS=*": busy_then, "AT+UMQPB=*": [["ERROR:15"], ["+UMQPB:0,1", "OK"]]}, MQTT_URCS)
    out, module = _exec(app_exe, profile, tmp_path, "mqtt.subscribe a/# 1", "mqtt.unsubscribe a/#",
                        "mqtt.publish a/b hello")
    assert out.returncode == 0, out.stdout
    for text in ("subscribed to a/#\n", "unsubscribed from a/#\n", "published 5 bytes to a/b (qos 0)\n"):
        assert text in out.stdout, out.stdout
    sent = [c for c in module.log if c.startswith(("AT+UMQS=", "AT+UMQPB="))]
    assert sent == ['AT+UMQS=0,0,"a/#",1'] * 2 + ['AT+UMQS=0,1,"a/#"'] * 2 + ['AT+UMQPB=0,0,0,"a/b"'] * 2, sent
    assert [data for _, data in module.payloads] == [b"hello", b"hello"], module.payloads


def test_mqtt_publish_gives_up_after_ten_busy_answers(app_exe, tmp_path):
    """A module that stays busy is asked ten times, 100 ms apart, then the publish fails with the busy code."""
    out, module = _exec(app_exe, _profile(WIFI, {"AT+UMQPB=*": ["ERROR:15"]}), tmp_path, "mqtt.publish a/b hello")
    assert out.returncode == 1, out.stdout
    assert "ERROR: publish failed (-15: U_ERROR_COMMON_OPERATION_IN_PROGRESS)\n" in out.stdout, out.stdout
    assert module.log.count('AT+UMQPB=0,0,0,"a/b"') == 10, module.log


def test_mqtt_publish_sends_a_stored_file_as_it_is(app_exe, tmp_path):
    """file= publishes the bytes of a stored file, binary included, with the QoS and retain options before it."""
    data = b"file payload\x00\x01\xff"
    _store(tmp_path, "msg.bin", data)
    profile = _profile(WIFI, {"AT+UMQPB=*": ["+UMQPB:0,1", "OK"]})
    out, module = _exec(app_exe, profile, tmp_path, "mqtt.publish a/b qos=1 retain file=msg.bin")
    assert out.returncode == 0 and "published 15 bytes to a/b (qos 1, retained)" in out.stdout, out.stdout
    assert module.payloads == [('AT+UMQPB=0,1,1,"a/b"', data)], module.payloads


def test_mqtt_read_shows_unprintable_bytes_as_dots_and_stops_at_an_empty_message(app_exe, tmp_path):
    """A message with control bytes is printed with dots in their place; an empty read ends the queue."""
    profile = _profile(WIFI, {"AT+UMQRB=*": [['+UMQRB:0,"a/b"\x01h\x07i\xff', "OK"], ['+UMQRB:0,"a/b"\x01', "OK"]]})
    out, module = _exec(app_exe, profile, tmp_path, "mqtt.read 0")
    assert out.returncode == 0 and "a/b: h.i.\n1 message(s)\n" in out.stdout, out.stdout
    assert module.log.count("AT+UMQRB=0") == 2, module.log


def test_mqtt_subscribe_warns_when_the_broker_does_not_confirm(app_exe, tmp_path):
    """Without +UEMQSC within 5 s the subscription is kept, with a warning that it was not confirmed."""
    out, _ = _exec(app_exe, _profile(WIFI), tmp_path, "mqtt.subscribe a/#")
    assert out.returncode == 0, out.stdout
    assert "WARNING: no subscribe confirmation from the broker\nsubscribed to a/#\n" in out.stdout, out.stdout


MQTT_ERRORS = {
    "params": ("mqtt.connect broker.local", {"AT+UMQCP=*": ERROR},
               f"ERROR: cannot set connection parameters {REFUSED}"),
    "tls": ("mqtt.connect broker.local ca=myca", {"AT+UMQTLS=*": ERROR},
            f"ERROR: cannot set TLS {REFUSED}; certificates must be on the module (cert.list)"),
    "connect": ("mqtt.connect broker.local", {"AT+UMQC=*": ERROR}, f"ERROR: connect failed {REFUSED}"),
    "will": ("mqtt.will dev/status offline", {"AT+UMQLWT=*": ERROR}, f"ERROR: cannot set the last will {REFUSED}"),
    "disconnect": ("mqtt.disconnect", {"AT+UMQDC=*": ERROR}, "ERROR: disconnect failed"),
    "subscribe": ("mqtt.subscribe a/#", {"AT+UMQS=*": ERROR}, f"ERROR: subscribe failed {REFUSED}"),
    "unsubscribe": ("mqtt.unsubscribe a/#", {"AT+UMQS=*": ERROR}, f"ERROR: unsubscribe failed {REFUSED}"),
    "publish": ("mqtt.publish a/b hello", {"AT+UMQPB=*": ERROR}, f"ERROR: publish failed {REFUSED}"),
    "read": ("mqtt.read 0", {"AT+UMQRB=*": ERROR}, f"ERROR: read failed {REFUSED}"),
    "read-empty": ("mqtt.read 0", {"AT+UMQRB=*": ['+UMQRB:0,"a/b"\x01', "OK"]}, "0 message(s)"),
    "file-missing": ("mqtt.publish a/b file=nofile.bin", {}, "ERROR: cannot read nofile.bin"),
    "file-big": ("mqtt.publish a/b file=big.bin", {}, "ERROR: big.bin is 5001 bytes (the module takes max 5000)"),
    "subscribe-usage": ("mqtt.subscribe", {}, "usage: mqtt.subscribe <topic> [qos=0]"),
    "unsubscribe-usage": ("mqtt.unsubscribe", {}, "usage: mqtt.unsubscribe <topic>"),
    "publish-nothing": ("mqtt.publish a/b", {}, "usage: mqtt.publish"),
    "publish-file-and-text": ("mqtt.publish a/b file=msg.bin hello", {}, "usage: mqtt.publish"),
    "read-usage": ("mqtt.read soon", {}, "usage: mqtt.read [wait_s=10]"),
}


@pytest.mark.parametrize("case", MQTT_ERRORS)
def test_mqtt_command_says_what_failed(app_exe, tmp_path, case):
    """What the module refuses is reported with the step that failed; a file that is missing or over 5000 bytes
    and a wrong argument are refused before anything is sent to the module."""
    command, answers, message = MQTT_ERRORS[case]
    _store(tmp_path, "big.bin", bytes(5001))
    _store(tmp_path, "msg.bin", b"hello")
    out, module = _exec(app_exe, _profile(WIFI, answers, MQTT_URCS), tmp_path, command)
    assert message in out.stdout and out.returncode != 0, (out.returncode, out.stdout)
    _asked(module, answers)
    if not answers:
        assert not any(sent.startswith("AT+UMQ") for sent in module.log), module.log


# ---- sock.* ----

# commands (the last one fails), answers of the module, the message the user gets
SOCK_ERRORS = {
    "open": (["sock.open tcp"], {"AT+USOCR=*": ERROR}, "ERROR: failed to create socket"),
    "connect": (["sock.open tcp", "sock.connect 0 example.com 80"], {"AT+USOC=*": ERROR},
                f"ERROR: connect failed {REFUSED}"),
    "tls": (["sock.open tcp", "sock.tls 0 ca"], {"AT+USOTLS=*": ERROR}, f"ERROR: TLS setup failed {REFUSED}"),
    "send": (["sock.open tcp", "sock.send 0 hi"], {"AT+USOWB=*": ERROR}, f"ERROR: send failed {REFUSED}"),
    "read": (["sock.open tcp", "sock.read 0 64 0"], {"AT+USORB=*": ERROR}, f"ERROR: read failed {REFUSED}"),
    "read-code": (["sock.open tcp", "sock.read 0 64 0"], {"AT+USORB=*": ["ERROR:11"]},
                  "ERROR: read failed (-11: U_ERROR_COMMON_NOT_FOUND)"),
    "bind": (["sock.open udp", "sock.bind 0 5000"], {"AT+USOB=*": ERROR}, "ERROR: bind failed"),
    "listen": (["sock.open tcp", "sock.listen 0 8000"], {"AT+USOL=*": ERROR},
               "ERROR: listen failed (bind the socket first, or give a port)"),
    "accept": (["sock.open tcp", "sock.listen 0 8000", "sock.accept 0"], {"AT+USOL=*": ["OK"]},
               "ERROR: no incoming connection"),
    "resolve": (["sock.open udp", "sock.sendto 0 nohost 5000 hi"], {"AT+USOH=*": ERROR},
                "ERROR: cannot resolve nohost"),
    "sendto": (["sock.open udp", "sock.sendto 0 192.168.1.10 5000 hi"], {"AT+USOSTB=*": ERROR},
               f"ERROR: sendto failed {REFUSED}"),
    "recvfrom": (["sock.open udp", "sock.recvfrom 0 64 0"], {"AT+USORFB=*": ERROR},
                 f"ERROR: receive failed {REFUSED}"),
    "join": (["sock.open udp", "sock.mcast 0 join 224.0.0.251"], {"AT+USOMG=*": ERROR},
             f"ERROR: joining 224.0.0.251 failed {REFUSED}; the socket must be a UDP socket bound with sock.bind"),
    "leave": (["sock.open udp", "sock.mcast 0 leave ff02::fb"], {"AT+USOMG=*": ERROR},
              f"ERROR: leaving ff02::fb failed {REFUSED}; the socket must be a UDP socket bound with sock.bind"),
    "close": (["sock.open tcp", "sock.close 0"], {"AT+USOCL=*": ERROR}, "ERROR: failed to close socket 0"),
    "close-all": (["sock.close all"], {"AT+USOCL=*": ERROR}, "closed 0 socket(s)"),
    "echo-socket": (["sock.echo tcp 7000 1"], {"AT+USOCR=*": ERROR}, "ERROR: failed to create socket"),
    "echo-listen": (["sock.echo tcp 7000 1"], {"AT+USOL=*": ERROR, "AT+USOCL=0": ["OK"]},
                    "ERROR: cannot open port 7000"),
    "echo-bind": (["sock.echo udp 7000 1"], {"AT+USOB=*": ERROR, "AT+USOCL=0": ["OK"]},
                  "ERROR: cannot open port 7000"),
}


@pytest.mark.parametrize("case", SOCK_ERRORS)
def test_sock_command_says_what_failed(app_exe, tmp_path, case):
    """Each socket command reports the step the module refused, with the module's error code and its name where
    the message has one, and exits with 1; sock.close all counts only the sockets that did close, and sock.echo
    closes the socket it could not bind or listen on."""
    commands, answers, message = SOCK_ERRORS[case]
    out, module = _exec(app_exe, _profile(WIFI, answers), tmp_path, *commands)
    assert message in out.stdout and out.returncode == 1, (out.returncode, out.stdout)
    _asked(module, answers)


@pytest.mark.parametrize("command", [
    "sock.open", "sock.open raw", "sock.connect 0 example.com 0", "sock.tls 0 ca client", "sock.send",
    "sock.read 0 0", "sock.bind 0 0", "sock.listen 0 0", "sock.accept soon", "sock.sendto 0 192.168.1.10 0 hi",
    "sock.recvfrom 0 0", "sock.close 1 2", "sock.mcast 0 flood 224.0.0.1", "sock.mcast 0 join 224.0.0.1 now",
    "sock.mcast 0 join ff02::1::2", "sock.mcast 0 join ff02:1:2:3:4:5:6::7", "sock.echo tcp 0",
])
def test_sock_usage_errors_send_nothing(app_exe, tmp_path, command):
    """With a socket open, a wrong argument (port 0, a length of 0, a word too many, a group with two gaps or
    too many fields) prints the usage and nothing more reaches the module."""
    out, module = _exec(app_exe, _profile(WIFI), tmp_path, "sock.open udp", command)
    assert f"usage: {command.split()[0]}" in out.stdout and out.returncode != 0, (out.returncode, out.stdout)
    after = module.log[module.log.index("AT+USOCR=17") + 1:]
    assert not [c for c in after if c.startswith("AT+USO")], module.log


def test_sock_tls_with_a_client_certificate(app_exe, tmp_path):
    """Three names after the handle set the CA, the client certificate and its key in one AT+USOTLS."""
    out, module = _exec(app_exe, _profile(WIFI), tmp_path, "sock.open tcp", "sock.tls 0 ca client key")
    assert out.returncode == 0 and "socket 0 uses TLS with ca ca, client client\n" in out.stdout, out.stdout
    assert 'AT+USOTLS=0,3,"ca","client","key"' in module.log, module.log


def test_sock_send_joins_the_words_and_converts_escapes(app_exe, tmp_path):
    r"""The words after the handle are joined with one space; \r, \n and \\ become the characters they name."""
    profile = _profile(WIFI, {"AT+USOWB=*": ["+USOWB:0,11", "OK"]})
    out, module = _exec(app_exe, profile, tmp_path, "sock.open tcp", r"sock.send 0 a\r\nb\\c   d \x")
    assert out.returncode == 0 and "sent 11 bytes\n" in out.stdout, out.stdout
    assert module.payloads == [("AT+USOWB=0", b"a\r\nb\\c d \\x")], module.payloads


def test_sock_send_waits_for_a_full_send_buffer(app_exe, tmp_path):
    """A write the module takes 0 bytes of (its send buffer is full) is repeated until the data is gone."""
    profile = _profile(WIFI, {"AT+USOWB=*": [["+USOWB:0,0", "OK"], ["+USOWB:0,3", "OK"], ["+USOWB:0,2", "OK"]]})
    out, module = _exec(app_exe, profile, tmp_path, "sock.open tcp", "sock.send 0 hello")
    assert out.returncode == 0 and "sent 5 bytes\n" in out.stdout, out.stdout
    assert [data for _, data in module.payloads] == [b"hello", b"hello", b"lo"], module.payloads


def test_udp_read_falls_back_to_1000_bytes_when_the_module_rejects_1460(app_exe, tmp_path):
    """NORA-W36 3.2 refuses a 1460 byte read with error 36 (integer out of range): the read is repeated with
    1000 and the datagram is shown with its sender, unprintable bytes as dots."""
    profile = _profile(WIFI, {"AT+USORFB=*,1460": ERROR, "AT+USYEC?": ["+USYEC:36", "OK"],
                              "AT+USORFB=*,1000": ["+USORFB:0,192.168.1.10,5000\x01hello\x07", "OK"]})
    out, module = _exec(app_exe, profile, tmp_path, "sock.open udp", "sock.recvfrom 0 1460 0")
    assert out.returncode == 0, out.stdout
    assert "received 6 bytes from 192.168.1.10:5000: hello.\n" in out.stdout, out.stdout
    reads = [c for c in module.log if c.startswith(("AT+USORFB", "AT+USYEC"))]
    assert reads == ["AT+USORFB=0,1460", "AT+USYEC?", "AT+USORFB=0,1000"], module.log


def test_sock_mcast_takes_a_full_address_and_a_group_ending_in_a_gap(app_exe, tmp_path):
    """An IPv6 group written out in eight fields is passed on as it is; one ending in :: is filled with zeros."""
    out, module = _exec(app_exe, _profile(WIFI), tmp_path, "sock.open udp", "sock.bind 0 5353",
                        "sock.mcast 0 join ff02:0:0:0:0:0:0:fb", "sock.mcast 0 leave ff02::")
    assert out.returncode == 0, out.stdout
    assert "socket 0 joined ff02:0:0:0:0:0:0:fb\n" in out.stdout and "socket 0 left ff02::\n" in out.stdout
    groups = [c for c in module.log if c.startswith("AT+USOMG=")]
    assert groups == ["AT+USOMG=0,1,[ff02:0000:0000:0000:0000:0000:0000:00fb]",
                      "AT+USOMG=0,0,[ff02:0000:0000:0000:0000:0000:0000:0000]"], groups


# command, answers, URCs, what is printed, AT commands closing what was opened
ECHOES = {
    "other-listener": ("sock.echo tcp 7000 1", {}, {"AT+USOL=*": ["+UESOIC:5,192.168.1.10,9"]},
                       "TCP echo on port 7000 for 1 s\nechoed 0 bytes\n", ["AT+USOCL=0"]),
    "tcp-write-fails": ("sock.echo tcp 7000 5", {"AT+USORB=*": ["+USORB:1\x01ping", "OK"], "AT+USOWB=*": ERROR},
                        {"AT+USOL=*": ["+UESOIC:0,192.168.1.10,1"]},
                        "TCP echo on port 7000 for 5 s\nclient connected\nechoed 0 bytes\n",
                        ["AT+USOCL=1", "AT+USOCL=0"]),
    "udp-reply-fails": ("sock.echo udp 7000 5", {"AT+USORFB=*": ["+USORFB:0,192.168.1.10,5000\x01ping", "OK"],
                                                "AT+USOSTB=*": ERROR}, {},
                        "UDP echo on port 7000 for 5 s\nechoed 0 bytes\n", ["AT+USOCL=0"]),
}


@pytest.mark.parametrize("case", ECHOES)
def test_sock_echo_ends_cleanly(app_exe, tmp_path, case):
    """sock.echo leaves a connection to another listener alone (no read, no write), and stops after the first
    read when the client cannot be written to instead of going on for its seconds; it counts only what was
    echoed and closes its sockets."""
    command, answers, urcs, text, closed = ECHOES[case]
    out, module = _exec(app_exe, _profile(WIFI, answers, urcs), tmp_path, command)
    assert out.returncode == 0 and text in out.stdout, (out.returncode, out.stdout)
    assert [c for c in module.log if c.startswith("AT+USOCL")] == closed, module.log
    reads = [c for c in module.log if c.startswith(("AT+USORB", "AT+USORFB"))]
    writes = [c for c in module.log if c.startswith(("AT+USOWB", "AT+USOSTB"))]
    assert len(reads) == len(writes) == (1 if answers else 0), module.log


# ---- certificate files: cert.import on a module, utils.cert.info on hand-made files ----

PASSWORD = "p\u00e9\u20ac\U0001f600"   # throwaway; one character each of 1, 2, 3 and 4 UTF-8 bytes


@pytest.fixture(scope="module")
def pki():
    """A root, an intermediate and a device certificate with the device's EC key, all made for this run only."""
    pytest.importorskip("cryptography")
    import datetime

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    def cert(subject, key, issuer, issuer_key, ca):
        now = datetime.datetime.now(datetime.timezone.utc)
        names = [x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]) for cn in (subject, issuer)]
        return (x509.CertificateBuilder().subject_name(names[0]).issuer_name(names[1])
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
                .sign(issuer_key, hashes.SHA256()))

    keys = [ec.generate_private_key(ec.SECP256R1()) for _ in range(3)]
    root = cert("Test Root", keys[0], "Test Root", keys[0], True)
    inter = cert("Test Intermediate", keys[1], "Test Root", keys[0], True)
    leaf = cert("device-01", keys[2], "Test Intermediate", keys[1], False)
    pem, der = serialization.Encoding.PEM, serialization.Encoding.DER
    return {"root": root, "inter": inter, "leaf": leaf, "key": keys[2],
            "root_pem": root.public_bytes(pem), "root_der": root.public_bytes(der),
            "leaf_pem": leaf.public_bytes(pem), "leaf_der": leaf.public_bytes(der),
            "key_pem": keys[2].private_bytes(pem, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())}


def _p12(pki, password=None):
    """Key, device certificate, intermediate and root as PKCS#12, AES encrypted when a password is given."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.serialization import pkcs12

    how = serialization.BestAvailableEncryption(password.encode()) if password else serialization.NoEncryption()
    return pkcs12.serialize_key_and_certificates(b"device", pki["key"], pki["leaf"], [pki["root"], pki["inter"]],
                                                 how)


def _uploads(module):
    return [sent for sent in module.log if sent.startswith("AT+USECUB=")]


def test_cert_import_uploads_the_client_chain_the_key_and_the_ca(app_exe, tmp_path, pki):
    """cert.import of a PKCS#12 file in a folder, opened with a password of 1 to 4 byte UTF-8 characters, uploads
    the client chain under the file's name, the key as <name>-key and the root as <name>-ca, each as PEM, and
    checks the fingerprints the module reports."""
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization

    _store(tmp_path, "certs/dev.p12", _p12(pki, PASSWORD))
    out, module = _shell(app_exe, _profile(WIFI), tmp_path, [f'cert.import certs/dev.p12 "" {PASSWORD}'])
    assert "ERROR" not in out.stdout and "warning" not in out.stdout, out.stdout[-3000:]
    for text in ("PKCS#12: 3 certificates and a private key", "uploaded client dev (", "uploaded key dev-key (",
                 "uploaded ca dev-ca (", "client certificate dev, key dev-key, CA dev-ca",
                 "fingerprint verified: " + hashlib.sha256(pki["leaf_der"]).hexdigest().upper(),
                 "fingerprint verified: " + hashlib.sha256(pki["root_der"]).hexdigest().upper()):
        assert text in out.stdout, f"{text!r} missing:\n{out.stdout[-3000:]}"
    assert _uploads(module) == ['AT+USECUB=1,"dev"', 'AT+USECUB=2,"dev-key"', 'AT+USECUB=0,"dev-ca"'], module.log
    sent = dict(module.payloads)
    assert x509.load_pem_x509_certificates(sent['AT+USECUB=1,"dev"']) == [pki["leaf"], pki["inter"]]
    assert x509.load_pem_x509_certificates(sent['AT+USECUB=0,"dev-ca"']) == [pki["root"]]
    key = serialization.load_pem_private_key(sent['AT+USECUB=2,"dev-key"'], None)
    assert key.public_key().public_numbers() == pki["key"].public_key().public_numbers()


def test_cert_import_of_a_lone_certificate_uploads_it_as_a_ca(app_exe, tmp_path, pki):
    """A DER certificate without a key goes up as a CA certificate named after the file; nothing else is sent."""
    _store(tmp_path, "root.cer", pki["root_der"])
    out, module = _exec(app_exe, _profile(WIFI), tmp_path, "cert.import root.cer")
    assert out.returncode == 0 and "DER certificate: 1 certificate\nuploaded ca root (" in out.stdout, out.stdout
    assert "client certificate" not in out.stdout, out.stdout
    assert _uploads(module) == ['AT+USECUB=0,"root"'], module.log
    assert module.payloads == [('AT+USECUB=0,"root"', pki["root_pem"])], module.payloads


def test_cert_import_stops_at_the_upload_that_fails(app_exe, tmp_path, pki):
    """When the module refuses the client certificate, the key and the CA are not uploaded and the command
    fails with the upload error."""
    _store(tmp_path, "client.p12", _p12(pki))
    out, module = _exec(app_exe, _profile(WIFI, {"AT+USECUB=*": ERROR}), tmp_path, "cert.import client.p12 plant")
    assert out.returncode == 1, out.stdout
    assert (f"ERROR: upload failed {REFUSED}; check PEM format, size and that the name is unused\n"
            in out.stdout), out.stdout
    assert _uploads(module) == ['AT+USECUB=1,"plant"'], module.log


IMPORT_REFUSALS = {
    "usage": ("cert.import", "usage: cert.import <file> [name] [pw]"),
    "name": ("cert.import client.p12 " + "n" * 29,
             f"ERROR: name '{'n' * 29}' is too long (max 28 characters, so that {'n' * 29}-key fits)"),
    "missing": ("cert.import missing.p12", "ERROR: cannot read missing.p12"),
    "junk": ("cert.import notes.txt", "ERROR: not a certificate file (PEM, DER, PKCS#7 or PKCS#12)"),
}


@pytest.mark.parametrize("case", IMPORT_REFUSALS)
def test_cert_import_refuses_before_uploading(app_exe, tmp_path, case):
    """No file argument, a name too long for its -key suffix, a missing file and a file that is no certificate
    are refused with their own message and nothing is uploaded."""
    command, message = IMPORT_REFUSALS[case]
    _store(tmp_path, "notes.txt", b"hello")
    out, module = _exec(app_exe, _profile(WIFI), tmp_path, command)
    assert message in out.stdout and out.returncode != 0, (out.returncode, out.stdout)
    assert _uploads(module) == [], module.log


# Just enough DER to build PKCS#12 and PKCS#7 files that are wrong in one place.

def _der(tag, *parts):
    body = b"".join(parts)
    size = len(body)
    octets = (size.bit_length() + 7) // 8
    length = bytes([size]) if size < 128 else bytes([0x80 + octets]) + size.to_bytes(octets, "big")
    return bytes([tag]) + length + body


def _seq(*parts):
    return _der(0x30, *parts)


def _oid(encoded):
    return _der(0x06, bytes.fromhex(encoded))


def _int(value):
    return _der(0x02, bytes([value]))


def _oct(data=b""):
    return _der(0x04, data)


def _ctx(*parts):
    return _der(0xA0, *parts)


NULL = b"\x05\x00"
DATA, SIGNED = "2A864886F70D010701", "2A864886F70D010702"                    # PKCS#7 content types
KEY_BAG, SHROUDED, CERT_BAG, SAFE_BAG = ("2A864886F70D010C0A01" + n for n in ("01", "02", "03", "06"))
X509 = "2A864886F70D01091601"
PBKDF2, PBES2, PBMAC1 = ("2A864886F70D0105" + n for n in ("0C", "0D", "0E"))
HMAC_SHA256, AES256_CBC = "2A864886F70D0209", "60864801650304012A"
SHA1, MD5 = "2B0E03021A", "2A864886F70D0205"


def _pfx(*infos, mac=b""):
    """A PKCS#12 file whose AuthenticatedSafe holds these ContentInfos, without a MAC unless one is given."""
    return _seq(_int(3), _seq(_oid(DATA), _ctx(_oct(_seq(*infos)))), mac)


def _bags(*bags):
    """A ContentInfo of type data: a SafeContents with these bags."""
    return _seq(_oid(DATA), _ctx(_oct(_seq(*bags))))


def _bag(kind, value):
    return _seq(_oid(kind), _ctx(value))


def _cert_bag(der):
    return _bag(CERT_BAG, _seq(_oid(X509), _ctx(_oct(der))))


def _mac(digest_oid, digest, *salt_and_iterations):
    return _seq(_seq(_seq(_oid(digest_oid), NULL), _oct(digest)), *salt_and_iterations)


def _pbes2(cipher):
    """PBES2 as an AlgorithmIdentifier: PBKDF2 with HMAC-SHA256, a fixed salt, one iteration, then the cipher."""
    kdf = _seq(_oid(PBKDF2), _seq(_oct(b"saltsalt"), _int(1), _seq(_oid(HMAC_SHA256), NULL)))
    return _seq(_oid(PBES2), _seq(kdf, _seq(_oid(cipher), _oct(bytes(16)))))


def _pem(label, der):
    return f"-----BEGIN {label}-----\n{base64.b64encode(der).decode()}\n-----END {label}-----\n".encode()


SALT = (_oct(b"salt"), _int(1))
BROKEN_FILES = {
    "empty": (b"", "ERROR: empty.bin is empty"),
    # PKCS#12: the container, its bags, then the MAC
    "p12-ber": (_seq(_int(3), _seq(_oid(DATA))),
                "ERROR: malformed PKCS#12 file (BER-encoded content; re-export it with OpenSSL)"),
    "p12-content": (_seq(_int(3), _seq(_oid(DATA), _ctx(_oct(_int(1))))), "ERROR: malformed PKCS#12 content"),
    "p12-info": (_pfx(_int(1)), "ERROR: malformed PKCS#12 content"),
    "p12-info-type": (_pfx(_seq(_int(1))), "ERROR: malformed PKCS#12 content"),
    "p12-nothing": (_pfx(), "ERROR: no certificate or private key in p12-nothing.bin"),
    "p12-safe": (_pfx(_seq(_oid(DATA), _ctx(_oct(_int(1))))), "ERROR: malformed PKCS#12 SafeContents"),
    "p12-data": (_pfx(_seq(_oid(DATA), _ctx(_int(1)))), "ERROR: malformed PKCS#12 content\n"),
    "p12-bag": (_pfx(_bags(_int(1))), "ERROR: malformed PKCS#12 bag"),
    "p12-bag-type": (_pfx(_bags(_seq(_int(1)))), "ERROR: malformed PKCS#12 bag"),
    "p12-key": (_pfx(_bags(_bag(KEY_BAG, _seq(_int(1))))), "ERROR: cannot read the private key (-0x"),
    "p12-key-bag": (_pfx(_bags(_bag(SHROUDED, _int(1)))), "ERROR: malformed PKCS#12 key bag"),
    "p12-key-cipher": (_pfx(_bags(_bag(SHROUDED, _seq(_seq(_oid(SHA1), NULL), _oct(bytes(32)))))),
                       "ERROR: unsupported encryption in the file (only PBES2 with AES)"),
    "p12-key-password": (_pfx(_bags(_bag(SHROUDED, _seq(_pbes2(AES256_CBC), _oct(bytes(32)))))),
                         "ERROR: wrong password\n"),
    "p12-key-pbes2": (_pfx(_bags(_bag(SHROUDED, _seq(_pbes2(SHA1), _oct(bytes(32)))))),
                      "); the file may use a cipher mbedTLS does not have"),
    "p12-cert": (_pfx(_bags(_bag(CERT_BAG, _int(1)))), "ERROR: malformed PKCS#12 certificate bag"),
    "p12-cert-value": (_pfx(_bags(_bag(CERT_BAG, _seq(_oid(X509), _int(1))))),
                       "ERROR: malformed PKCS#12 certificate bag"),
    "p12-cert-junk": (_pfx(_bags(_cert_bag(b"junk"))), "warning: skipped a certificate that does not parse (-0x"),
    "p12-nested": (_pfx(_bags(_bag(SAFE_BAG, _int(1)))), "ERROR: malformed PKCS#12 SafeContents"),
    "mac": (_pfx(mac=_int(1)), "ERROR: malformed PKCS#12 MAC\n"),
    "mac-digest": (_pfx(mac=_seq(_seq(_int(1)))), "ERROR: malformed PKCS#12 MAC\n"),
    "mac-salt": (_pfx(mac=_mac(SHA1, bytes(20))), "ERROR: malformed PKCS#12 MAC salt"),
    "mac-iterations": (_pfx(mac=_mac(SHA1, bytes(20), _oct(b"salt"), _int(0))),
                       "ERROR: malformed PKCS#12 MAC iterations"),
    "mac-pbmac1": (_pfx(mac=_mac(PBMAC1, bytes(20), *SALT)),
                   "ERROR: PBMAC1 integrity (openssl -pbmac1_pbkdf2) is not supported; re-export without it"),
    "mac-hash": (_pfx(mac=_mac(MD5, bytes(16), *SALT)), "ERROR: unsupported PKCS#12 MAC hash"),
    "mac-length": (_pfx(mac=_mac(SHA1, bytes(5), *SALT)), "ERROR: malformed PKCS#12 MAC\n"),
    "mac-wrong": (_pfx(mac=_mac(SHA1, bytes(20), *SALT)),
                  "ERROR: wrong password (the PKCS#12 integrity check failed)"),
    # PKCS#7
    "p7": (_seq(_oid(SIGNED)), "ERROR: malformed PKCS#7 file"),
    "p7-version": (_seq(_oid(SIGNED), _ctx(_seq(_oct()))), "ERROR: malformed PKCS#7 file"),
    "p7-content": (_seq(_oid(SIGNED), _ctx(_seq(_int(1), _der(0x31), _int(1)))), "ERROR: malformed PKCS#7 file"),
    "p7-certs": (_seq(_oid(SIGNED), _ctx(_seq(_int(1), _der(0x31), _seq(_oid(DATA)), _ctx(_int(1))))),
                 "ERROR: malformed PKCS#7 certificate list"),
    # PEM
    "pem-end": (b"-----BEGIN CERTIFICATE-----\nAAAA\n",
                "ERROR: PEM block -----BEGIN CERTIFICATE----- has no end line"),
    "pem-base64": (b"-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n",
                   "ERROR: malformed PEM block (bad base64)"),
    "pem-p7": (_pem("PKCS7", _seq(_oid(SIGNED))), "ERROR: malformed PKCS#7 file"),
    "pem-crl": (_pem("X509 CRL", b"not needed"), "note: skipped a X509 CRL block"),
    "pem-key": (_pem("PRIVATE KEY", b"junk"), "ERROR: cannot read the private key (-0x"),
    "pem-des": (b"-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: DES-EDE3-CBC,0123456789ABCDEF\n"
                b"\nAAAA\n-----END RSA PRIVATE KEY-----\n",
                "ERROR: the private key uses an unsupported encryption (only AES); re-encrypt it with openssl pkcs8"),
    # an encrypted PKCS#8 key as DER
    "der-key-password": (_seq(_pbes2(AES256_CBC), _oct(bytes(32))), "ERROR: wrong password for the private key"),
}


@pytest.mark.parametrize("case", BROKEN_FILES)
def test_cert_info_names_what_is_wrong_with_a_file(app_exe, tmp_path, case):
    """utils.cert.info on a file that is empty, cut off, of an unsupported kind or opened with the wrong password
    fails with a line that names the part it could not read."""
    content, message = BROKEN_FILES[case]
    name = _store(tmp_path, f"{case}.bin", content)
    out = _offline(app_exe, tmp_path, f"utils.cert.info {name} x")
    assert message in out.stdout and out.returncode == 1, (out.returncode, out.stdout)
    assert "format   " not in out.stdout, out.stdout


# file content from the pki fixture, the warning or note, the summary line that still follows, certificates listed
ODD_FILES = {
    "two-keys": (lambda pki: pki["key_pem"] + pki["key_pem"] + pki["leaf_pem"],
                 "warning: more than one private key; keeping the first",
                 "format   PEM, 1 certificate, private key", 1),
    "other-key": (lambda pki: pki["key_pem"] + pki["root_pem"], "warning: no certificate matches the private key",
                  "format   PEM, 1 certificate, private key", 1),
    "17-certs": (lambda pki: pki["root_pem"] * 17, "warning: more than 16 certificates; the rest are left out",
                 "format   PEM, 17 certificates", 16),
    "nested-bag": (lambda pki: _pfx(_bags(_bag(SAFE_BAG, _seq(_cert_bag(pki["root_der"]))))),
                   "note: the file has no integrity MAC", "format   PKCS#12, 1 certificate", 1),
}


@pytest.mark.parametrize("case", ODD_FILES)
def test_cert_info_warns_and_still_shows_the_file(app_exe, tmp_path, pki, case):
    """A second private key, a key no certificate matches, more than 16 certificates and a PKCS#12 file without
    a MAC (its certificate in a nested bag) are told to the user and the certificates are still listed."""
    content, note, summary, listed = ODD_FILES[case]
    name = _store(tmp_path, f"{case}.bin", content(pki))
    out = _offline(app_exe, tmp_path, f"utils.cert.info {name}")
    assert out.returncode == 0 and note in out.stdout and summary in out.stdout, (out.returncode, out.stdout)
    assert len(re.findall(r"^cert \d+ +CN=", out.stdout, re.M)) == listed, out.stdout


def test_cert_convert_says_which_file_it_could_not_write(app_exe, tmp_path, pki):
    """An output name in a folder that does not exist fails with the file that could not be written."""
    name = _store(tmp_path, "root.cer", pki["root_der"])
    out = _offline(app_exe, tmp_path, f"utils.cert.convert {name} nodir/out")
    assert out.returncode == 1 and "ERROR: cannot write nodir/out.crt" in out.stdout, out.stdout
    assert "upload all of it" not in out.stdout and not (tmp_path / "storage" / "nodir").exists(), out.stdout
