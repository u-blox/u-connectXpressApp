"""cell.* and app.time against virtual modems: error replies, other makers, odd answers and the network clock paths."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.app


def _run(exe, cwd, port, *commands, timeout=60):
    """Every command through --shell (--exec stops at the first failure); the output without prompts."""
    out = subprocess.run([str(exe), port, "--shell"], input="\n".join(commands) + "\n", capture_output=True,
                         text=True, timeout=timeout, errors="replace", cwd=cwd)
    return re.sub(r"^(ucxapp> )+", "", out.stdout, flags=re.M)


def _cell(exe, cwd, steps, commands=None, urcs=None, timeout=60):
    """Runs cell.open on a fresh virtual SARA-R5 (with replies changed), then the steps; (stdout, AT log)."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    modem = Vucx("cell.yaml")
    modem.commands.update(commands or {})
    modem.urcs.update(urcs or {})
    port = modem.start()
    try:
        text = _run(exe, cwd, "COM255", f"cell.open {port}", *[s.format(port=port) for s in steps], timeout=timeout)
    finally:
        modem.stop()
    return text, modem.log


# ---- cell.* ----

def test_cell_status_with_odd_answers(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.status", "cell.signal"], commands={
        "AT+CPIN?": ["ERROR"],
        "AT+CSQ": ["+CSQ: 99,99", "OK"],
        "AT+CESQ": ["+CESQ: 99,99,255,255,255,255", "OK"],
        "AT+CEREG?": ["+CEREG: 0,2", "OK"],
        "AT+CREG?": ["+CREG: 0,5", "OK"],
        "AT+CGPADDR*": ['+CGPADDR: 1,', "OK"],
    })
    assert "SIM:       not ready or missing" in text, text
    assert "Signal:    unknown" in text, text
    assert re.search(r"^Network:\s+registered \(roaming\)$", text, re.M), text
    assert re.search(r"^IP:\s+none$", text, re.M), text
    assert "no signal (not on a network yet" in text, text


def test_cell_at_apn_and_pin_errors(app_exe, tmp_path):
    text, log = _cell(app_exe, tmp_path, [
        "cell.at AT+BAD", "cell.at AT+COPS=0", "cell.apn web ipv6", "cell.apn web ipv4v6", "cell.apn web x",
        "cell.pin 1234", "cell.pin 4321"], commands={
        "AT+BAD": ["ERROR"],
        "AT+CGDCONT=*": [["OK"], ["ERROR"]],
        "AT+CPIN?": ["+CPIN: SIM PIN", "OK"],
        "AT+CPIN=*": [["OK"], ["ERROR"]],
    })
    assert 'AT+CGDCONT=1,"IPV6","web"' in log and 'AT+CGDCONT=1,"IPV4V6","web"' in log, log
    assert "APN set to web (IPV6)" in text and "ERROR: APN not accepted" in text, text
    assert "usage: cell.apn" in text, text
    assert "SIM unlocked" in text and "ERROR: PIN not accepted" in text, text


def test_cell_attach_failures(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.attach off", "cell.attach on", "cell.attach"], commands={
        "AT+CGATT=0": ["ERROR"],
        "AT+CGATT=1": ["ERROR"],
        "AT+CGATT?": ["OK"],
    })
    assert "ERROR: detach failed" in text, text
    assert "ERROR: attach failed (check SIM" in text, text


def test_cell_attach_without_an_address(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.attach on"], commands={"AT+CGPADDR*": ['+CGPADDR: 1,"0.0.0.0"', "OK"]})
    assert "attached, but context 1 has no IP address" in text, text


@pytest.mark.parametrize("pdp, upsd", [("IPV4V6", "AT+UPSD=0,0,2"), ("IPV6", "AT+UPSD=0,0,1")])
def test_cell_attach_internal_stack_fails(app_exe, tmp_path, pdp, upsd):
    text, log = _cell(app_exe, tmp_path, ["cell.attach on"], commands={
        "AT+UPSND=0,8": ["+UPSND: 0,8,0", "OK"],
        "AT+CGDCONT?": [f'+CGDCONT: 1,"{pdp}","internet","10.0.0.5",0,0', "OK"],
        "AT+UPSDA=0,3": ["ERROR"],
    })
    assert upsd in log, log
    assert "the internal IP stack did not start" in text, text


def test_cell_operator_radio_rat_and_profile_errors(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, [
        "cell.operator", "cell.operator", "cell.operator", "cell.operator", "cell.operator scan",
        "cell.operator 24001", "cell.operator auto",
        "cell.radio", "cell.radio", "cell.radio off", "cell.radio reboot",
        "cell.rat ltem", "cell.rat 2g", "cell.rat", "cell.profile 7", "cell.profile dt"], commands={
        "AT+COPS?": [["ERROR"], ['+COPS: 1,0,"Manual net"', "OK"], ["+COPS: 2", "OK"], ["+COPS: 4", "OK"],
                     ["+COPS: 0", "OK"]],
        "AT+COPS=?": ["ERROR"],
        "AT+COPS=0": ["OK"],  # exact entry: the fnmatch pattern above also matches "AT+COPS=0"
        "AT+COPS=1,2,*": ["ERROR"],
        "AT+CFUN?": [["+CFUN: 0", "OK"], ["+CFUN: 7", "OK"]],
        "AT+CFUN=4": ["ERROR"],
        "AT+CFUN=16": ["ERROR"],
        "AT+CFUN=0": [["ERROR"], ["OK"], ["OK"], ["OK"]],
        "AT+URAT=*": ["ERROR"],
        "AT+URAT?": ["+URAT: 7,3", "OK"],
        "AT+UMNOPROF=7": ["ERROR"],
    })
    assert "ERROR: no operator information" in text, text
    assert re.search(r"^Operator:\s+Manual net\n(?:Access:.*\n)?Selection:\s+manual$", text, re.M), text
    assert re.search(r"^Selection:\s+off the network$", text, re.M), text
    assert re.search(r"^Selection:\s+\?$", text, re.M), text
    assert "ERROR: scan failed" in text, text
    assert "ERROR: the network did not accept the modem" in text, text
    assert "selection set; registration continues in the background" in text, text
    assert "radio: off (minimum)" in text and "radio: 7" in text, text
    assert "ERROR: the modem refused" in text and "ERROR: reboot not accepted" in text, text
    assert "ERROR: the modem did not leave the network (AT+CFUN=0)" in text, text
    assert "ERROR: the modem does not support that combination" in text, text
    assert re.search(r"^RAT:\s+ltem 3$", text, re.M), text
    assert "ERROR: profile 7 not accepted by this modem" in text, text
    assert "ERROR: reboot not accepted" in text, text


def test_cell_info_with_missing_answers(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.info"], commands={"AT+CIMI": ["OK"], "AT+CGMR": ["ERROR"]})
    assert re.search(r"^IMSI:\s+-$", text, re.M) and re.search(r"^Firmware:\s+-$", text, re.M), text


def test_cell_ping_on_other_makers(app_exe, tmp_path):
    qping = ['+QPING: 0,"1.2.3.4",32,20,255'] * 4 + ["+QPING: 0,4,4,0,20,20,20"]
    text, log = _cell(app_exe, tmp_path, ["cell.ping example.com"],
                      commands={"ATI": ["Quectel", "BG95-M3", "OK"], "AT+CGMI": ["Quectel", "OK"]},
                      urcs={"AT+QPING=*": qping})
    assert 'AT+QPING=1,"example.com",4,4' in log, log
    assert "+QPING: 0,4,4,0,20,20,20" in text, text
    text, _ = _cell(app_exe, tmp_path, ["cell.ping example.com"],
                    commands={"ATI": ["Telit", "OK"], "AT+CGMI": ["Telit", "OK"]})
    assert "ERROR: ping is not mapped for 'Telit" in text, text
    text, _ = _cell(app_exe, tmp_path, ["cell.ping example.com"], commands={"AT+UPING=*": ["ERROR"]})
    assert "ERROR: ping refused (cell.attach on first)" in text, text


VENDORS = {
    "Quectel": ({"AT+QIOPEN=*": ["CONNECT"]}, 'AT+QIOPEN=1,0,"UDP","h.example",7,0,2', "AT+QICLOSE=0"),
    "SIMCOM": ({"AT+CIPOPEN=*": ["CONNECT"]}, 'AT+CIPOPEN=0,"UDP","h.example",7', "AT+NETCLOSE"),
    "Telit": ({"AT#SD=*": ["CONNECT"]}, 'AT#SD=1,1,7,"h.example",0,0,1', "AT#SH=1"),
    "Nordic": ({}, 'AT#XCONNECT="h.example",7', "AT#XSOCKET=0"),
}


@pytest.mark.parametrize("maker", VENDORS)
def test_cell_connect_and_disconnect_per_maker(app_exe, tmp_path, maker):
    extra, opened, closed = VENDORS[maker]
    text, log = _cell(app_exe, tmp_path, ["cell.connect h.example 7 udp", "cell.status", "cell.disconnect"],
                      commands={"ATI": [maker, "OK"], "AT+CGMI": [maker, "OK"], **extra})
    assert opened in log and closed in log, log
    assert "connected to h.example:7" in text and "disconnected, command mode on" in text, text


@pytest.mark.parametrize("maker, refuse", [("Quectel", "AT+QIOPEN=*"), ("SIMCOM", "AT+CIPOPEN=*"),
                                           ("Telit", "AT#SD=*"), ("Nordic", "AT#XSOCKET=1*")])
def test_cell_connect_refused_per_maker(app_exe, tmp_path, maker, refuse):
    text, _ = _cell(app_exe, tmp_path, ["cell.connect h.example 7"],
                    commands={"ATI": [maker, "OK"], "AT+CGMI": [maker, "OK"], refuse: ["ERROR"]})
    assert "ERROR: modem refused the socket" in text, text


def test_cell_connect_failures(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.connect h.example 7"],
                    commands={"ATI": ["Nordic", "OK"], "AT+CGMI": ["Nordic", "OK"], "AT#XCONNECT=*": ["ERROR"]})
    assert "ERROR: the connection was not established" in text, text
    text, _ = _cell(app_exe, tmp_path, ["cell.connect h.example 7"],
                    commands={"ATI": ["Acme", "OK"], "AT+CGMI": ["Acme", "OK"]})
    assert "ERROR: cell.connect is not mapped for 'Acme'" in text, text
    text, log = _cell(app_exe, tmp_path, ["cell.connect h.example 7", "cell.connect h.example 7 tcp"],
                      commands={"AT+USOCO=*": [["ERROR"], ["OK"]], "AT+USODL=*": ["ERROR"]})
    assert "ERROR: the connection was not established" in text, text
    assert "ERROR: modem refused direct link mode" in text, text
    assert "AT+USOCL=0" in log, log
    text, _ = _cell(app_exe, tmp_path, ["cell.connect h.example 7"], commands={"AT+USOCR=*": ["ERROR"]})
    assert "ERROR: modem refused the socket" in text, text


def test_cell_open_detect_close_states(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.open", "cell.open {port}", "cell.open COM254", "cell.detect",
                                        "cell.watch 1", "cell.close", "cell.close", "cell.detect virt:cell"],
                    commands={"AT+COPS?": ['+COPS: 0,0,"vucx telecom"', "OK"],
                              "AT+CESQ": ["+CESQ: 99,99,255,255,255,255", "OK"]})
    assert text.count("cellular module already open on") == 2, text
    assert "ERROR: already open on" in text, text
    assert "open now:" in text and "1 AT port(s) found" in text, text
    assert "MODEM" in text and "SIG   RSSI -73 dBm" in text, text
    assert "ERROR: no cellular module open (use cell.open)" in text, text
    assert "virt:cell" in text, text


def test_cell_card_without_signal_and_detect_while_linked(app_exe, tmp_path):
    text, _ = _cell(app_exe, tmp_path, ["cell.watch 1", "cell.connect caster.local 2101", "cell.detect",
                                        "cell.open COM254", "cell.watch 1", "cell.disconnect"],
                    commands={"AT+CSQ": ["+CSQ: 99,99", "OK"], "AT+CESQ": ["+CESQ: 99,99,255,255,255,255", "OK"]})
    assert "SIG   none" in text, text
    assert "data link to caster.local:2101 now" in text and "1 AT port(s) found" in text, text
    assert "ERROR: the cellular module is in use:" in text, text
    assert "USER  none yet" in text and "network values from" in text, text


# ---- app.time ----

def _nora(fail=(), replies=None, commands=None):
    """A virtual NORA-W36 whose network refuses the socket kinds in fail and answers requests starting with a key of
    replies with the next reply (HTTP header and body)."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.commands.update(commands or {})
    handle, serve = module.net.handle, module.net._serve

    def net_handle(cmd, data):
        if any(cmd.startswith(p) for p in fail):
            return ["ERROR"], []
        return handle(cmd, data)

    def net_serve(s, data, ip="192.168.1.10", port=None):
        for prefix, queue in (replies or {}).items():
            if bytes(data).startswith(prefix) and queue:
                s["rx"].append((ip, port or s["port"], queue.pop(0)))
                return
        serve(s, data, ip, port)

    module.net.handle, module.net._serve = net_handle, net_serve
    return module


GEO = (b'HTTP/1.1 200 OK\r\n\r\n{"status":"success","country":"Sweden","city":"Malmo","lat":55.6,"lon":13.0,'
       b'"timezone":"Europe/Stockholm","offset":3600,"query":"192.0.2.7"}')


def test_time_sync_ntp_http_and_geo(app_exe, tmp_path):
    module = _nora(replies={b"GET /json": [GEO, GEO]})
    try:
        text = _run(app_exe, tmp_path, module.start(), "app.time sync ntp", "app.time sync http",
                    "app.time sync http www.example.com", "app.time sync", "app.time geo", "app.time zone",
                    "app.time sync ntp", "app.time sync module", "app.time")
    finally:
        module.stop()
    assert "from ntp pool.ntp.org" in text and "from http www.google.com" in text, text
    assert "from http www.example.com" in text, text
    assert "public IP 192.0.2.7" in text and "place     Malmo, Sweden" in text, text
    assert text.count("zone      Europe/Stockholm, UTC+01:00 now") == 2, text   # geo, then the refresh after sync
    assert "refreshed by app.time sync" in text and "from module clock" in text, text
    assert "Europe/Stockholm\nplace   Malmo, Sweden (ip)" in text, text


def test_time_geo_answers_without_a_place(app_exe, tmp_path):
    nocity = (b'HTTP/1.1 200 OK\r\n\r\n{"status":"success","lat":-33.5,"lon":151.25,"timezone":"X","offset":-19800}')
    fail = b'HTTP/1.1 200 OK\r\n\r\n{"status":"fail","message":"private range"}'
    junk = b'HTTP/1.1 200 OK\r\n\r\n{"status":"success","lat":99,"lon":0,"offset":0}'
    module = _nora(replies={b"GET /json": [nocity, fail, junk], b"HEAD ": [b"HTTP/1.1 200 OK\r\n\r\n",
                                                                           b"HTTP/1.1 200 OK\r\nDate: Sun, 06 Foo "
                                                                           b"1994 08:49:37 GMT\r\n\r\n"]})
    try:
        text = _run(app_exe, tmp_path, module.start(), "app.time geo", "app.time", "app.time geo", "app.time geo",
                    "app.time sync http", "app.time sync http")
    finally:
        module.stop()
    assert "zone      X, UTC-05:30 now" in text and "-33.50000, 151.25000 (ip)" in text, text
    assert "ERROR: ip-api.com gave no place: private range" in text, text
    assert "ERROR: ip-api.com gave no place\n" in text, text
    assert text.count("ERROR: no HTTP Date header from www.google.com") == 2, text


def test_time_network_and_module_failures(app_exe, tmp_path):
    module = _nora(fail=("AT+USOCR=17", "AT+USOCR=6"),
                   commands={"AT+USYTU?": ["ERROR"], "AT+USYTU=*": ["ERROR"]})
    try:
        text = _run(app_exe, tmp_path, module.start(), "app.time sync", "app.time sync ntp", "app.time geo",
                    "app.time sync module", "app.time push", "app.time sync bogus", "app.time sync module h",
                    "app.time sync ntp a b")
    finally:
        module.stop()
    assert "note: no SNTP reply from pool.ntp.org" in text, text
    assert "ERROR: no SNTP reply from pool.ntp.org" in text, text
    assert "ERROR: no HTTP Date header from www.google.com" in text, text
    assert "ERROR: no answer from ip-api.com" in text, text
    assert "ERROR: cannot read the module clock (AT+USYTU?)" in text, text
    assert "ERROR: cannot set the module clock" in text, text
    assert text.count("usage: app.time") == 3, text


def test_time_offline_usage_and_zone_errors(app_exe, tmp_path):
    text = _run(app_exe, tmp_path, "COM255", "app.time sync gnss", "app.time set nonsense", "app.time zone +1:x",
                "app.time zone 1 2", "app.time zone -03:30", "app.time zone", "app.time zone off", "app.time zone",
                "app.time sync bogus")
    assert "ERROR: no GNSS time" in text, text
    assert "ERROR: zone +1:x: use +HH:MM" in text, text
    assert "zone    manual UTC-03:30, fixed (no daylight saving)" in text, text
    assert "zone none (app.time geo finds it" in text, text
    assert "ERROR: not connected to a module" in text, text
    assert text.count("usage: app.time") >= 3, text
