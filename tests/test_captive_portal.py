"""Offline tests of the fake hotel login page used to test wifi.portal."""

import re
import subprocess
import threading
import urllib.error
import urllib.parse
import urllib.request

import pytest
from harness.captive_portal import serve


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


@pytest.fixture
def portal(request):
    server = serve(0, "127.0.0.1", **getattr(request, "param", {}))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server, f"http://127.0.0.1:{server.server_address[1]}"
    server.shutdown()


def get(url, cookie=""):
    req = urllib.request.Request(url, headers={"Cookie": cookie} if cookie else {})
    try:
        r = urllib.request.build_opener(NoRedirect).open(req, timeout=5)
        return r.status, dict(r.headers), r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode()


def post(url, fields, cookie=""):
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if cookie:
        headers["Cookie"] = cookie
    req = urllib.request.Request(url, urllib.parse.urlencode(fields).encode(), headers)
    try:
        r = urllib.request.urlopen(req, timeout=5)
        return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def login_page(base):
    status, headers, body = get(base + "/login")
    assert status == 200
    token = re.search(r'name="token" value="(\w+)"', body).group(1)
    return token, headers.get("Set-Cookie", "").split(";")[0], body


def test_probe_redirects_until_login(portal):
    _, base = portal
    status, headers, _ = get(base + "/generate_204")
    assert status == 302 and headers["Location"].startswith("/login")
    token, _, body = login_page(base)
    assert 'action="/login?src=portal&amp;v=1"' in body
    assert post(base + "/login", {"token": token, "accept": "yes", "go": "Connect"})[0] == 200
    assert get(base + "/generate_204")[0] == 204


def test_terms_must_be_accepted(portal):
    _, base = portal
    token, _, _ = login_page(base)
    status, body = post(base + "/login", {"token": token})
    assert status == 403 and "terms" in body
    assert get(base + "/generate_204")[0] == 302


@pytest.mark.parametrize("portal", [{"cookie": True}], indirect=True)
def test_cookie_required(portal):
    _, base = portal
    token, cookie, _ = login_page(base)
    assert post(base + "/login", {"token": token, "accept": "yes"})[0] == 403
    assert post(base + "/login", {"token": token, "accept": "yes"}, cookie)[0] == 200


@pytest.mark.parametrize("portal", [{"room": True}], indirect=True)
def test_room_fields_required(portal):
    _, base = portal
    token, _, body = login_page(base)
    assert 'name="room"' in body
    assert post(base + "/login", {"token": token, "accept": "yes"})[0] == 403
    assert post(base + "/login", {"token": token, "accept": "yes", "room": "101", "surname": "Smith"})[0] == 200


@pytest.mark.parametrize("portal", [{"js_only": True}], indirect=True)
def test_js_only_has_no_form(portal):
    _, base = portal
    assert "<form" not in get(base + "/login")[2].split("<script>")[0]


def form(app_exe, tmp_path, html, page="http://10.0.0.1:8080/portal/login.php", *fields):
    (tmp_path / "page.html").write_text(html, encoding="ascii")
    args = [str(app_exe), "--offline", "--exec", " ".join(["wifi.portal.form page.html", page, *fields])]
    return subprocess.run(args, capture_output=True, text=True, timeout=30, cwd=tmp_path, stdin=subprocess.DEVNULL)


def parsed(out):
    lines = dict(line.split(" ", 1) for line in out.stdout.splitlines() if line.split(" ", 1)[0] in ("POST", "GET"))
    body = next(line[6:] for line in out.stdout.splitlines() if line.startswith("body: "))
    return lines, urllib.parse.parse_qs(body, keep_blank_values=True)


@pytest.mark.app
@pytest.mark.parametrize("portal", [{"cookie": True, "room": True}], indirect=True)
def test_app_form_is_accepted_by_the_portal(app_exe, tmp_path, portal):
    """The app fills in the real login page; posting its body with the cookie must log in."""
    _, base = portal
    _, cookie, html = login_page(base)
    out = form(app_exe, tmp_path, html, base + "/login?orig=x", "room=101", "surname=Smith")
    assert out.returncode == 0, out.stdout + out.stderr
    methods, fields = parsed(out)
    assert methods["POST"] == base + "/login?src=portal&v=1"   # &amp; decoded, absolute path resolved
    assert fields["accept"] == ["yes"] and fields["go"] == ["Connect"] and fields["room"] == ["101"]
    body = next(line[6:] for line in out.stdout.splitlines() if line.startswith("body: "))
    req = urllib.request.Request(base + "/login?src=portal&v=1", body.encode(),
                                 {"Content-Type": "application/x-www-form-urlencoded", "Cookie": cookie})
    assert urllib.request.urlopen(req, timeout=5).status == 200
    assert get(base + "/generate_204")[0] == 204


@pytest.mark.app
def test_app_form_parsing_edge_cases(app_exe, tmp_path):
    html = """<html><FORM Method=GET ACTION='accept.cgi'>
      <INPUT TYPE=hidden NAME=mac VALUE=aa:bb>
      <input type="radio" name="plan" value="free" checked><input type="radio" name="plan" value="paid">
      <input type="checkbox" name="tos"><input type="reset" name="r"><input type="button" name="b" value="x">
      <input name="email"></form><form action="/other"><input name="no" value="1"></form>"""
    out = form(app_exe, tmp_path, html, "http://10.0.0.1/portal/login.php?x=1", '"email=a b@c.d"', "extra=1")
    assert out.returncode == 0, out.stdout + out.stderr
    methods, fields = parsed(out)
    assert methods["GET"] == "http://10.0.0.1/portal/accept.cgi"
    assert fields == {"mac": ["aa:bb"], "plan": ["free"], "tos": ["on"], "email": ["a b@c.d"], "extra": ["1"]}
    assert "a+b%40c.d" in out.stdout   # url-encoded
    assert "fields: mac plan tos email extra" in out.stdout


@pytest.mark.app
def test_app_form_without_form_needs_a_browser(app_exe, tmp_path):
    out = form(app_exe, tmp_path, "<div id=app></div><script>render()</script>")
    assert out.returncode != 0
    assert "needs a browser" in out.stdout


@pytest.mark.app
def test_app_form_rejects_bad_arguments(app_exe, tmp_path):
    out = form(app_exe, tmp_path, "<form></form>", "http://h/", "novalue")
    assert out.returncode != 0
    assert "usage" in (out.stdout + out.stderr).lower()
