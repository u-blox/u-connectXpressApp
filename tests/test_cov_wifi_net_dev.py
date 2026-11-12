"""wifi.*, net.*, http.* and dev.* against vucx profiles that answer OK, ERROR or odd replies (offline coverage)."""

import os
import subprocess

import pytest

pytestmark = pytest.mark.app

NORA = {"manufacturer": "u-blox", "model": "NORA-W36", "version": "3.5.0-0", "serial": "VUCX0123456789"}
CRLF = "\\r\\n"   # the profiles send header escapes as text, as the module does


def _env():
    env = dict(os.environ)
    env.pop("UCX_REGDOMAIN", None)
    return env


def _shell(exe, profile, commands, cwd, timeout=80):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx(profile)
    port = module.start()
    try:
        out = subprocess.run([str(exe), port, "--shell"], input="\n".join(commands) + "\n", capture_output=True,
                             text=True, timeout=timeout, errors="replace", cwd=cwd, env=_env())
    except subprocess.TimeoutExpired as e:
        text = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"hung after {timeout} s:\n{text[-3000:]}\nlast AT: {module.log[-15:]}")
    finally:
        module.stop()
    return out.stdout, module.log


def _yaml_profile(tmp_path, data):
    import yaml

    path = tmp_path / "profile.yaml"
    path.write_text(yaml.safe_dump(data, allow_unicode=True), encoding="utf-8")
    return str(path)


def _store(tmp_path, name, text):
    (tmp_path / "storage").mkdir(exist_ok=True)
    (tmp_path / "storage" / name).write_text(text, encoding="utf-8")


# ---- a module that answers ERROR to nearly everything (only the setup commands work, no identity) ----

def test_silent_module_error_paths(app_exe, tmp_path):
    out, log = _shell(app_exe, None, [
        "dev.summary", "dev.caps", "dev.stack", "dev.crash", "dev.reboot", "dev.factory", "dev.factory confirm",
        "dev.status", "dev.diagnose", "dev.power", "dev.power auto off", "dev.power auto 5", "dev.power save 1 100",
        "dev.power sleep", "dev.power deep", "dev.power deep pin",
        "wifi.regdomain", "wifi.regdomain fcc", "wifi.disconnect", "wifi.ap.disable", "wifi.ap.enable ap secret123",
        "wifi.connect someap secret123", "wifi.eap someap ca cli key id", "wifi.peap someap user pw ca",
        "net.ping", "net.ping host 0", "net.ping host 1", "net.dns", "net.dns host",
        "http.get", "http.get http://", "http.get http://h x", "http.get http://h",
        "http.get https://h:8443/x ca=myca", "http.get https://h/x ca=myca cert=c key=k", "http.get https://h/x",
        "http.get https://h cert=c", "http.get https://h ca=",
        "http.download", "http.download http:// f.bin", "http.download http://h/x f.bin",
        "http.post", "http.post http://h/x a=b", "wifi.portal", "wifi.portal check", "wifi.portal bogus",
        "wifi.portal login noequals", "wifi.portal login http://h/p a=b",
        "net.iperf", "net.iperf stop", "net.iperf client", "net.iperf client notanip", "net.iperf bogus",
        "net.iperf server udp 99999", "net.iperf client 1.2.3.4 tcp 0", "net.iperf server tcp 5001",
        "net.iperf client 1.2.3.4 udp 1 5001",
        "net.ntp", "net.ntp on bogus", "net.ntp on auto", "net.ntp on", "net.ntp off", "net.ntp server 2 x",
        "net.ntp server 0 -", "net.ntp bogus",
    ], tmp_path)
    for text in ("ERROR: no answer to the model query", "ERROR: task stack info not supported",
                 "ERROR: crash info not supported", "ERROR: reboot failed", "ERROR: factory reset failed",
                 "no power save level on this module", "ERROR: set power save failed",
                 "ERROR: set the power save level failed", "ERROR: light sleep failed", "ERROR: deep sleep failed",
                 "ERROR: cannot read the regulatory domain", "ERROR: module rejected the regulatory domain",
                 "Not connected", "ERROR: failed to deactivate AP", "ERROR: failed to configure the access point",
                 "ERROR: failed to set connection parameters", "ERROR: failed to start ping",
                 "ERROR: cannot resolve host", "ERROR: cannot configure HTTP session",
                 "no answer from http://connectivitycheck.gstatic.com/generate_204", "ERROR: nothing to stop",
                 "ERROR: give the server as an IP address", "ERROR: iperf failed to start",
                 "ERROR: cannot read NTP client status", "ERROR: cannot enable NTP", "ERROR: cannot disable NTP",
                 "ERROR: cannot set NTP server 0", "ERROR: invalid URL"):
        assert text in out, f"{text!r} missing:\n{out[-4000:]}"
    assert "AT+USYFR" in log, log[-20:]


# ---- a NORA-W36 that is connected but refuses most Wi-Fi commands ----

STUBBORN = {
    "identity": NORA,
    "commands": {
        "AT+UWSST=3": ["+UWSST:3,2", "OK"],
        "AT+UWRD?": ["+UWRD:99", "OK"],
        "AT+UWSCP=*": ["OK"],
        "AT+UWSSW=*": [["OK"], ["ERROR"]],
        "AT+UWSIP=*": [["ERROR"], ["+UWSIP:0,0", "OK"]],
        "AT+UWAPS?": ["+UWAPS:0", "OK"],
        "AT+UWAPNST?": ["+UWAPNST:7,1.2.3.4", "OK"],
        "AT+USYTS": ['+USYTS:"idle",1024,512,50', '+USYTS:"bad"', "+USYTS:noquote,12,x", "OK"],
        "AT+USYCI?": ["OK"],
        "AT+USYFR": ["OK"],
        "AT&W": ["OK"],
    },
}


def test_stubborn_wifi_module(app_exe, tmp_path):
    out, log = _shell(app_exe, STUBBORN, [
        "wifi.status", "wifi.rssi", "wifi.rssi x", "wifi.rssi 1 0", "wifi.survey 1", "wifi.survey x",
        "wifi.disconnect", "wifi.roaming on", "wifi.roaming", "wifi.roaming maybe", "wifi.roaming on off",
        "wifi.regdomain", "wifi.regdomain fcc", "wifi.regdomain nowhere", "wifi.regdomain 99", "wifi.regdomain a b",
        "wifi.scan", "wifi.ap.enable ap short", "wifi.ap.enable a b c d e", "wifi.ap.status",
        "wifi.ip", "wifi.ip", "wifi.ip dhcp", "wifi.ip 1.2.3.4 255.255.255.0 bad", "wifi.ip 1 2",
        "wifi.ip 1.2.3.4 255.255.255.0 1.2.3.1", "wifi.ip 1.2.3.4 255.255.255.0 1.2.3.1 8.8.8.8",
        "wifi.ip 1.2.3.4 255.255.255.0 1.2.3.1 8.8.8.8 1.1.1.1",
        "wifi.connect someap secret123", "wifi.connect other secret456", "wifi.connect openap",
        "wifi.connect a b c", "wifi.connect " + "x" * 40,
        "wifi.eap", "wifi.eap s ca c k id 9.9", "wifi.eap s ca c k id 1.3", "wifi.peap s u p ca bad",
        "wifi.peap s u p ca 1.2", "wifi.peap s u - ca any", "typedpw",
        "wifi.profile.add", "wifi.profile.add x\ty ssid", "wifi.profile.add n ssid " + "p" * 70,
        "wifi.profile.add lonely nowhere-ssid pw12345678", "wifi.profile.auto",
        "wifi.profile.connect", "wifi.profile.connect nope", "wifi.profile.remove", "wifi.profile.remove nope",
        "dev.stack", "dev.crash", "dev.factory confirm", "dev.power", "dev.power sleep",
        "dev.diagnose", "dev.uptime x", "dev.caps x", "dev.summary x", "dev.stack x", "dev.crash x",
        "dev.diagnose a b",
    ], tmp_path)
    for text in ("wifi: connected", "ERROR: not connected", "ERROR: no signal readings", "ERROR: failed to disconnect",
                 "ERROR: roaming not accepted", "ERROR: cannot read the roaming setting", "regdomain=99",
                 "ERROR: module rejected the regulatory domain", "'nowhere' is not a region", "'99' is not a region",
                 "WARNING: could not set regulatory domain 'fcc'", "0 network(s)",
                 "ERROR: password must be at least 8 characters", "security: mode=", "0 station(s)",
                 "ERROR: could not set IP configuration", "ERROR: invalid IP address 'bad'",
                 "Already connected, disconnecting first", "ERROR: failed to start connection",
                 "ERROR: failed to set WPA security", "ERROR: failed to set open security",
                 "ERROR: failed to set EAP-TLS security", "ERROR: failed to set PEAP security",
                 "ERROR: no known network found", "ERROR: no profile 'nope' (see wifi.profile.list)",
                 "ERROR: no profile 'nope'", "Task", "noquote,12,x", "no crash information stored",
                 "ERROR: reboot failed", "Check wifi", "Result:"):
        assert text in out, f"{text!r} missing:\n{out[-4000:]}"
    assert any(line.startswith("AT+UWSC=") for line in log), log[-30:]


# ---- a working NORA-W36 with a varied scan, profiles, enterprise security, power and diagnostics ----

SCAN = [
    '+UWSSC:AABBCCDDEEF0,"vucx-net",6,-45,16,8,8',
    '+UWSSC:AABBCCDDEEF2,"",11,-55,0,0,0',
    '+UWSSC:AABBCCDDEEF6,"opennet",1,-62,0,0,0',
    '+UWSSC:AABBCCDDEEF3,"wpa3net",36,-65,32,0,0',
    '+UWSSC:AABBCCDDEEF4,"oddnet",200,-75,64,0,0',
    '+UWSSC:AABBCCDDEEF5,"wpanet",1,-85,8,0,0',
    "OK",
]


def test_working_module_wifi_profiles_power_and_diagnose(app_exe, tmp_path):
    _store(tmp_path, "ucxapp.fwlatest", "NORA-W36-9.9.9.zip\n")
    _store(tmp_path, "ucxapp.blelast", "mybulb\n")
    profile = _yaml_profile(tmp_path, {"include": ["nora-w36.yaml"], "commands": {"AT+UWSSC*": SCAN}})
    out, log = _shell(app_exe, profile, [
        "wifi.regdomain 2", "wifi.regdomain europe", 'wifi.regdomain "New Zealand"', "wifi.regdomain -1",
        "wifi.channels", "wifi.channels mixed", "wifi.channels", "wifi.scan", "wifi.scan", "wifi.channels",
        "wifi.channels full", "wifi.channels bogus", "wifi.channels a b",
        "wifi.connect 2", "wifi.connect wpa3net", "wifi.connect 3",
        "wifi.connect vucx-net -", "secret123", "wifi.scan",
        "wifi.profile.add home vucx-net secret123", "wifi.profile.list", "wifi.profile.connect home",
        "wifi.profile.auto", "wifi.connect vucx-net",
        "wifi.profile.add far vucx-net secret123", "wifi.profile.connect far", "wifi.profile.remove far",
        "wifi.profile.add alias vucx-net secret123", "wifi.profile.auto",
        "wifi.eap vucx-net ca cli key id 1.2", "wifi.peap vucx-net user secret123 ca 1.3",
        "wifi.peap vucx-net user - ca any", "secret123",
        "wifi.status", "wifi.rssi 2 1", "wifi.ip", "wifi.ip dhcp", "wifi.ip 192.168.1.9 255.255.255.0 192.168.1.1",
        "wifi.roaming off", "wifi.roaming",
        "wifi.ap.enable vucxap secret123 14", "wifi.ap.status", "wifi.ap.disable",
        "dev.power", "dev.power auto", "dev.power auto off", "dev.power auto 0", "dev.power auto 1 2",
        "dev.power save 1 500", "dev.power save 0", "dev.power save 2", "dev.power sleep",
        "dev.power recover 0", "dev.power recover 100", "dev.power bogus", "dev.power deep nowhere",
        "dev.power deep reset", "dev.power wake", "dev.power deep uart", "dev.power wake", "dev.power deep",
        "dev.power wake",
        "dev.stack", "dev.uptime", "dev.diagnose example.com", "dev.status",
        "net.iperf server tcp 5001", "net.iperf stop", "net.ntp", "net.ntp on manual", "net.ntp on auto",
        "net.ntp off", "net.ntp server 0 pool.ntp.org", "net.ntp server 1 -",
        "wifi.disconnect", "wifi.status", "wifi.survey", "dev.diagnose", "dev.summary",
    ], tmp_path)
    for text in ("regdomain=", "channels: full", "channels: mixed", "Scanning the", "channels: full (channel list",
                 "<hidden>", "WPA3", "Secured", "Open", "Quietest for wifi.ap.enable",
                 "ERROR: network 2 is hidden; give its SSID", "ERROR: 'wpa3net' is secured",
                 "network 3 is 'opennet'", "saved as profile 'vucx-net'", "<- profile 'vucx-net'",
                 "profile 'home' saved", "Already connected to 'vucx-net'", "using saved profile",
                 "profile 'far' removed", "Strongest known network:", "Connected to 'vucx-net'",
                 "  mask 255.255.255.0  gateway 192.168.1.1", "rssi=", "IP configuration set", "roaming=",
                 "power save level 0", "power save on", "power save off",
                 "power save level 1 stored, idle timeout 500 ms",
                 "light sleep on NORA-W36 needs Bluetooth off", "deep sleep: no radio", "awake",
                 "iperf server on port 5001", "iperf stopped", "NTP client", "NTP server 0: pool.ntp.org",
                 "NTP server 1: removed", "Disconnected", "wifi: disconnected",
                 "ERROR: Wi-Fi not connected; join the network first", "NOT RUN",
                 "Update:           firmware 9.9.9 is out", "Last used:"):
        assert text in out, f"{text!r} missing:\n{out[-5000:]}"
    assert "AT+UWCLR" in log, log[-40:]


def test_profile_file_with_old_and_broken_lines(app_exe, tmp_path):
    _store(tmp_path, "ucxapp.wifi", "old\tssid1\tDEADBEEF\nbad\tssid2\ta1:zzzz\nopen\tssid3\t\nonefield\n")
    out, _ = _shell(app_exe, STUBBORN, ["wifi.profile.list"], tmp_path)
    assert "open: ssid=ssid3 open" in out, out[-3000:]
    assert "profile(s)" in out, out[-3000:]
    assert "cannot decrypt the password of profile bad" in out, out[-3000:]


# ---- HTTP: redirects, sizes, status codes and failures, and captive portal pages ----

def _hdr(*lines):
    return ['+UHTCGH:0,0,"' + CRLF.join(lines) + CRLF + CRLF + '"', "OK"]


def _body(data):
    return ["+UHTCGBB:0,0\x01" + data, "OK"]


LOGIN_FORM = ('<html><form method=get action="sub?x=1"><input type="hidden" name="tok" value="a&amp;b">'
              '<input type="radio" name="r" value="1"><input type="radio" name="r" value="2" checked>'
              '<input type="reset" name="rs"><input name="room"><input type="checkbox" name="ok">'
              '<input value="noname"></form></html>')

PATHS = ["loop", "abs", "missing", "sized", "short", "bodyerr", "hdrerr", "reqerr", "p1", "l1", "p2", "nf", "p3"]

HTTP = {
    "identity": NORA,
    "default": ["OK"],
    "state": {**{f'AT+UHTCRP=0,"/{p}"': {"page": p} for p in PATHS}, "AT+UHTCRP=*": {"page": "other"}},
    "commands": {
        "AT+UHTCGH=* [page=hdrerr]": ["ERROR"],
        "AT+UHTCGH=* [page=loop]": _hdr("HTTP/1.1 302 Found", "Location: /loop"),
        "AT+UHTCGH=* [page=abs]": _hdr("HTTP/1.1 301 Moved", "Location: http://h2/sized"),
        "AT+UHTCGH=* [page=missing]": _hdr("HTTP/1.1 404 Not Found"),
        "AT+UHTCGH=* [page=sized]": _hdr("HTTP/1.1 200 OK", "Content-Length: 5"),
        "AT+UHTCGH=* [page=short]": _hdr("HTTP/1.1 200 OK", "Content-Length: 10"),
        "AT+UHTCGH=* [page=p1]": _hdr("HTTP/1.1 302 Found", "Location: /l1"),
        "AT+UHTCGH=* [page=p2]": _hdr("HTTP/1.1 302 Found", "Location: /nf"),
        "AT+UHTCGH=* [page=p3]": _hdr("HTTP/1.1 302 Found", "Location: /missing"),
        "AT+UHTCGH=*": _hdr("HTTP/1.1 200 OK", "Content-Type: text/html"),
        "AT+UHTCGBB=* [page=bodyerr]": ["ERROR"],
        "AT+UHTCGBB=* [page=sized]": _body("abcde"),
        "AT+UHTCGBB=* [page=short]": _body("abcde"),
        "AT+UHTCGBB=* [page=l1]": _body(LOGIN_FORM),
        "AT+UHTCGBB=* [page=nf]": _body("<html>no form here</html>"),
        "AT+UHTCGBB=*": _body("\x07" + "x" * 600),
        "AT+UHTCRG=* [page=reqerr]": ["ERROR"],
        "AT+UHTCRPOB=* [page=reqerr]": ["ERROR"],
    },
    "urcs": {"AT+UHTCRG=*": ['+UEHTCRS:0,200,"OK"'], "AT+UHTCRPOB=*": ['+UEHTCRS:0,200,"OK"']},
}


def test_http_download_get_post_and_portal(app_exe, tmp_path):
    out, log = _shell(app_exe, HTTP, [
        "http.download http://h/sized a.bin", "http.download http://h/abs b.bin", "http.download http://h/short c.bin",
        "http.download http://h/missing d.bin", "http.download http://h/loop e.bin",
        "http.download http://h/hdrerr f.bin", "http.download http://h/reqerr g.bin",
        "http.download http://h/bodyerr h.bin",
        "http.get http://h/x 5", "http.get https://h:8443/x 1 ca=myca cert=c key=k", "http.get http://h/bodyerr",
        "http.get http://h/hdrerr", "http.get http://h/reqerr",
        "http.post http://h/x a=b text/plain", "http.post http://h/missing a=b", "http.post http://h/reqerr a=b",
        "wifi.portal check http://h/x", "wifi.portal check http://h/missing", "wifi.portal login http://h/p1 room=12",
        "wifi.portal login http://h/p2", "wifi.portal login http://h/p3",
    ], tmp_path)
    for text in ("5/5 bytes (100%)", "5 bytes saved", "redirect (HTTP 301)",
                 "ERROR: incomplete download (5 of 10 bytes)",
                 "ERROR: HTTP status 404", "ERROR: too many redirects", "ERROR: cannot read HTTP headers",
                 "ERROR: GET request failed", "ERROR: reading body failed", "HTTP/1.1 200 OK", ".xxxx",
                 "output truncated", "ERROR: POST request failed", "login page served in place of http://h/x",
                 "unexpected HTTP 404 from http://h/missing", "login page: http://h/l1 (redirected, HTTP 302)",
                 "submitting the form (GET http://h/sub?x=1), fields: tok r room ok",
                 "still not online", "ERROR: no <form> on the login page",
                 "ERROR: cannot load the login page http://h/missing (HTTP 404)"):
        assert text in out, f"{text!r} missing:\n{out[-5000:]}"
    assert (tmp_path / "storage" / "a.bin").read_bytes() == b"abcde"
    assert (tmp_path / "storage" / "b.bin").read_bytes() == b"abcde"
    assert any(line.startswith('AT+UHTCRP=0,"/sub?x=1&tok=a%26b&r=2&room=12&ok=on') for line in log), log[-30:]


def test_portal_form_offline(app_exe, tmp_path):
    _store(tmp_path, "f1.html", '<FORM METHOD="POST"><input type=hidden name=a value=1>'
                                '<input type=submit name=go value=Go></FORM>')
    _store(tmp_path, "f2.html", '<form action="http://abs/x"><input name=n>')
    _store(tmp_path, "f3.html", "<html>nothing</html>")
    _store(tmp_path, "f4.html", '<form action="rel"><input name=n value=v></form>')
    out, _ = _shell(app_exe, None, [
        'wifi.portal.form f1.html http://h/login?q=1 a=2 "extra=x y/z"', "wifi.portal.form f2.html http://h/",
        "wifi.portal.form f3.html http://h/", "wifi.portal.form missing.html http://h/",
        "wifi.portal.form f4.html http://h", "wifi.portal.form f1.html", "wifi.portal.form f1.html http://h/ bad",
    ], tmp_path)
    for text in ("POST http://h/login\nfields: a go extra\nbody: a=2&go=Go&extra=x+y%2Fz", "GET http://abs/x",
                 "no <form> in f3.html", "ERROR: cannot read missing.html", "GET http://h/rel"):
        assert text in out, f"{text!r} missing:\n{out[-3000:]}"
