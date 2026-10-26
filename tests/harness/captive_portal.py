"""Fake hotel Wi-Fi login page (captive portal) for testing wifi.portal.

Run on the PC and point the module's probe at it:
    py -3 tests/harness/captive_portal.py --port 8080 [--cookie] [--room] [--js-only] [--expire 60]
    wifi.portal check http://<pc-ip>:8080/generate_204
    wifi.portal login http://<pc-ip>:8080/generate_204 room=101 surname=Smith

Clients are told apart by IP address, like a real hotel portal does by MAC address.
"""

import argparse
import html
import secrets
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FORM = """<!DOCTYPE html><html><head><title>Hotel Wi-Fi</title></head><body>
<h1>Welcome to Hotel Example</h1>
<form method="post" action="/login?src=portal&amp;v=1">
<input type="hidden" name="token" value="{token}">
{room}<label><input type="checkbox" name="accept" value="yes"> I accept the terms</label>
<input type="submit" name="go" value="Connect">
</form></body></html>"""
ROOM = '<input type="text" name="room" value=""> <input name="surname">\n'
JS_ONLY = """<!DOCTYPE html><html><head><title>Hotel Wi-Fi</title></head><body>
<div id="app"></div><script>document.getElementById('app').innerHTML='<form>...</form>'</script></body></html>"""


class Portal:
    def __init__(self, cookie=False, room=False, js_only=False, expire=0):
        self.cookie, self.room, self.js_only, self.expire = cookie, room, js_only, expire
        self.allowed = {}   # ip -> time of login
        self.sessions = {}  # cookie value -> token
        self.log = []

    def online(self, ip):
        t = self.allowed.get(ip)
        return t is not None and (not self.expire or time.time() - t < self.expire)


def make_handler(portal):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            portal.log.append(fmt % args)
            print(self.client_address[0], fmt % args, flush=True)

        def send(self, code, body=b"", ctype="text/html", headers=()):
            self.send_response(code)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def cookie(self):
            for part in self.headers.get("Cookie", "").split(";"):
                k, _, v = part.strip().partition("=")
                if k == "psid":
                    return v
            return ""

        def do_GET(self):
            ip, path = self.client_address[0], urllib.parse.urlsplit(self.path).path
            if path == "/generate_204":
                if portal.online(ip):
                    self.send(204)
                else:
                    self.send(302, headers=[("Location", "/login?orig=generate_204")])
            elif path == "/login":
                if portal.js_only:
                    self.send(200, JS_ONLY.encode())
                    return
                sid, token = secrets.token_hex(8), secrets.token_hex(8)
                portal.sessions[sid] = token
                body = FORM.format(token=token, room=ROOM if portal.room else "")
                self.send(200, body.encode(), headers=[("Set-Cookie", f"psid={sid}; Path=/; HttpOnly")])
            elif path == "/status":
                self.send(200, b"online" if portal.online(ip) else b"blocked", "text/plain")
            else:
                self.send(404, b"not found", "text/plain")

        def do_POST(self):
            ip = self.client_address[0]
            if urllib.parse.urlsplit(self.path).path != "/login":
                self.send(404, b"not found", "text/plain")
                return
            n = int(self.headers.get("Content-Length", "0") or 0)
            form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(n).decode("ascii", "replace")).items()}
            sid = self.cookie()
            errors = []
            if form.get("accept") != "yes":
                errors.append("terms not accepted")
            if portal.cookie and portal.sessions.get(sid) != form.get("token"):
                errors.append("session cookie missing or wrong")
            if not portal.cookie and form.get("token") not in portal.sessions.values():
                errors.append("form token missing")
            if portal.room and not (form.get("room") and form.get("surname")):
                errors.append("room and surname required")
            if errors:
                msg = "Login failed: " + ", ".join(errors)
                self.send(403, f"<p>{html.escape(msg)}</p>".encode())
                return
            portal.allowed[ip] = time.time()
            self.send(200, b"<p>You are online. Enjoy your stay.</p>")

    return Handler


def serve(port=8080, host="0.0.0.0", **kw):
    portal = Portal(**kw)
    server = ThreadingHTTPServer((host, port), make_handler(portal))
    server.portal = portal
    return server


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--cookie", action="store_true", help="require the session cookie set by the login page")
    ap.add_argument("--room", action="store_true", help="require room and surname fields")
    ap.add_argument("--js-only", action="store_true", help="login form built by JavaScript (cannot be automated)")
    ap.add_argument("--expire", type=int, default=0, help="seconds until a login expires (0 = never)")
    a = ap.parse_args()
    server = serve(a.port, cookie=a.cookie, room=a.room, js_only=a.js_only, expire=a.expire)
    print(f"captive portal on port {a.port}: probe http://<this-pc>:{a.port}/generate_204", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
