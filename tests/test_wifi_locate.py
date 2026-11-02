"""wifi.locate: position from a Wi-Fi scan via Google, Skyhook, HERE or Combain, sent over the module's HTTPS.

Key storage and errors run anywhere; the scan, request and answer run against vucx (needs a pty: Linux/WSL)."""

import json
import sys

import pytest
from test_gnss_replay import _run as _run_offline
from test_vucx import _run

pytestmark = pytest.mark.app

STORE = "ucxapp.wifiloc"
GOOGLE_ANSWER = '{\n  "location": {\n    "lat": 55.6125,\n    "lng": 12.9925\n  },\n  "accuracy": 24.5\n}'
HERE_ANSWER = '{"location":{"lat":52.22296709,"lng":-0.07337817,"accuracy":152}}'
GOOGLE_403 = ('{"error":{"code":403,"message":"API key not valid. Please pass a valid API key.",'
              '"status":"PERMISSION_DENIED"}}')


# ---- offline: keys and errors, no module ----

def test_key_is_saved_private_and_becomes_default(app_exe, tmp_path):
    out = _run_offline(app_exe, tmp_path, "wifi.locate key google AIza-k_1", "wifi.locate key here hk+/=",
                       "wifi.locate key Google AIza-k_2", "wifi.locate key")
    assert "google API key saved to ucxapp.wifiloc; google is the default service" in out.stdout, out.stdout
    assert (tmp_path / "storage" / STORE).read_text() == "google AIza-k_2\nhere hk+/=\n"
    assert "google   AIza...  (default)" in out.stdout, out.stdout
    assert "here     hk+/...\n" in out.stdout, out.stdout
    assert "AIza-k_2" not in out.stdout.split("google is the default service")[-1], out.stdout   # listed shortened


def test_key_list_when_none_saved(app_exe, tmp_path):
    out = _run_offline(app_exe, tmp_path, "wifi.locate key")
    assert "no API keys saved" in out.stdout, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("wifi.locate", "ERROR: no API key saved (wifi.locate key <google|skyhook|here|combain> <api-key>)"),
    ("wifi.locate combain", "ERROR: no combain API key"),
    ("wifi.locate google AIza", "ERROR: not connected to a module"),
    ("wifi.locate body", "ERROR: not connected to a module"),
    ("wifi.locate bing", "usage"),
    ("wifi.locate bing k", "usage"),
    ("wifi.locate google a;b", "usage"),
    ("wifi.locate google k extra", "usage"),
    ("wifi.locate key google", "usage"),
    ("wifi.locate key bing k", "usage"),
    ("wifi.locate key google a,b", "usage"),
    ("wifi.locate body bing", "usage"),
    ("wifi.locate body here x", "usage"),
])
def test_errors(app_exe, tmp_path, command, message):
    out = _run_offline(app_exe, tmp_path, command)
    assert message.lower() in out.stdout.lower(), out.stdout


def test_saved_key_without_module(app_exe, tmp_path):
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / STORE).write_text("bogus x\nskyhook sk1\r\n")
    for command in ("wifi.locate", "wifi.locate skyhook"):   # the default is the first valid line
        out = _run_offline(app_exe, tmp_path, command)
        assert "ERROR: not connected to a module" in out.stdout, out.stdout
        assert "no API key" not in out.stdout, out.stdout


# ---- against vucx: the scan, the request and the answer ----

vucx_only = pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")


def _module(status=200, answer=GOOGLE_ANSWER, scan=None):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    p = load("wifi.yaml")
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}[status]
    p["default"] = ["OK"]
    header = rf'+UHTCGH:0,0,"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n\r\n"'
    p["commands"] = {**p["commands"],
                     "AT+UHTCGH=*": [header, "OK"],
                     "AT+UHTCGBB=*": [f"+UHTCGBB:0,0\x01{answer}", "OK"],
                     "AT+UHTCRPOB=*": ["+UHTCRPOB:0,64", "OK"]}
    if scan is not None:
        p["commands"]["AT+UWSSC*"] = scan + ["OK"]
    p["urcs"] = {**p["urcs"], "AT+UHTCRPOB=*": [f'+UEHTCRS:0,{status},"{reason}"']}
    module = Vucx(p)
    module.start()
    return module


def _posted(module):
    bodies = [data for cmd, data in module.payloads if cmd.startswith("AT+UHTCRPOB")]
    assert len(bodies) == 1, module.log
    return json.loads(bodies[0])


@vucx_only
def test_google_position_from_scan(app_exe, tmp_path):
    module = _module()
    try:
        out = _run(app_exe, module.port, "wifi.locate key google AIzaTest", "wifi.locate", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "Asking google with 2 access points..." in out.stdout, out.stdout
    assert "Position: 55.612500, 12.992500 +/- 24 m (google, 2 access points)" in out.stdout, out.stdout
    assert "https://www.openstreetmap.org/?mlat=55.612500&mlon=12.992500" in out.stdout, out.stdout
    assert "gnss.virtual 55.612500 12.992500" in out.stdout, out.stdout
    assert _posted(module) == {"considerIp": False, "wifiAccessPoints": [
        {"macAddress": "AA:BB:CC:DD:EE:F0", "signalStrength": -45, "channel": 6},
        {"macAddress": "AA:BB:CC:DD:EE:F1", "signalStrength": -80, "channel": 36}]}
    assert any("www.googleapis.com" in c for c in module.log), module.log
    assert any(c.startswith("AT+UHTCRP=") and "/geolocation/v1/geolocate?key=AIzaTest" in c for c in module.log), \
        module.log


@vucx_only
def test_here_request_format_and_nested_accuracy(app_exe, tmp_path):
    module = _module(answer=HERE_ANSWER)
    try:
        out = _run(app_exe, module.port, "wifi.locate here hk+/=", cwd=tmp_path)
    finally:
        module.stop()
    assert "Position: 52.222967, -0.073378 +/- 152 m (here, 2 access points)" in out.stdout, out.stdout
    assert _posted(module) == {"wlan": [{"mac": "AA:BB:CC:DD:EE:F0", "rss": -45},
                                        {"mac": "AA:BB:CC:DD:EE:F1", "rss": -80}]}
    assert any("/v2/locate?apiKey=hk%2B%2F%3D" in c for c in module.log), module.log
    assert not (tmp_path / "storage" / STORE).exists()   # a key on the command line is used once, not saved


@vucx_only
@pytest.mark.parametrize(("service", "host", "path"), [
    ("combain", "apiv2.combain.com", "/?key=ck"),
    ("skyhook", "global.skyhook.com", "/wps2/json/location?key=ck&user=ucxapp"),
])
def test_google_compatible_services(app_exe, tmp_path, service, host, path):
    module = _module()
    try:
        out = _run(app_exe, module.port, f"wifi.locate {service} ck", cwd=tmp_path)
    finally:
        module.stop()
    assert f"Position: 55.612500, 12.992500 +/- 24 m ({service}, 2 access points)" in out.stdout, out.stdout
    assert "wifiAccessPoints" in _posted(module)
    assert any(host in c for c in module.log), module.log
    assert any(c.startswith("AT+UHTCRP=") and path in c for c in module.log), module.log


@vucx_only
def test_bad_key_shows_the_service_message(app_exe, tmp_path):
    module = _module(status=403, answer=GOOGLE_403)
    try:
        out = _run(app_exe, module.port, "wifi.locate google bad", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0
    assert "ERROR: google answered HTTP 403: API key not valid. Please pass a valid API key." in out.stdout, out.stdout
    assert "check the API key: wifi.locate key google <api-key>" in out.stdout, out.stdout


@vucx_only
def test_unknown_place_and_answer_without_position(app_exe, tmp_path):
    module = _module(status=404, answer='{"error":{"code":404,"message":"Not Found"}}')
    try:
        out = _run(app_exe, module.port, "wifi.locate google k", cwd=tmp_path)
    finally:
        module.stop()
    assert "ERROR: google answered HTTP 404: Not Found" in out.stdout, out.stdout
    assert "knows none of the access points" in out.stdout, out.stdout

    module = _module(answer='{"result":"ok"}')
    try:
        out = _run(app_exe, module.port, "wifi.locate google k", cwd=tmp_path)
    finally:
        module.stop()
    assert 'ERROR: no position in the answer from google: {"result":"ok"}' in out.stdout, out.stdout


@vucx_only
def test_nomap_networks_are_left_out_and_two_are_needed(app_exe, tmp_path):
    scan = ['+UWSSC:AABBCCDDEEF0,"home",6,-45,8,8,8', '+UWSSC:AABBCCDDEEF2,"cafe_nomap",11,-50,8,8,8']
    module = _module(scan=scan)
    try:
        out = _run(app_exe, module.port, "wifi.locate google k", cwd=tmp_path)
    finally:
        module.stop()
    assert "ERROR: 1 usable access point(s) found, google needs at least 2" in out.stdout, out.stdout
    assert not any(c.startswith("AT+UHTCRPOB") for c in module.log), module.log


@vucx_only
def test_body_prints_the_request_without_sending(app_exe, tmp_path):
    module = _module()
    try:
        out = _run(app_exe, module.port, "wifi.locate body here", cwd=tmp_path)
    finally:
        module.stop()
    assert "POST https://positioning.hereapi.com/v2/locate?apiKey=<api-key> (Content-Type: application/json)" \
        in out.stdout, out.stdout
    assert '{"wlan":[{"mac":"AA:BB:CC:DD:EE:F0","rss":-45},{"mac":"AA:BB:CC:DD:EE:F1","rss":-80}]}' in out.stdout
    assert not any(c.startswith("AT+UHTC") for c in module.log), module.log
