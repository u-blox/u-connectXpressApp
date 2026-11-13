"""A TLS client behind the virtual module: one of the scripted clients of a listening port (vucx `requests:`),
so the app's TLS server (web.start https, the tls bridges) runs a real handshake without a network.

    client = TlsClient(b"GET /api/version HTTP/1.1\\r\\nHost: m\\r\\n\\r\\n", ca=tmp_path / "storage" / "tls-ca.crt")
    profile["sockets"]["requests"] = {443: [client]}
    ...
    client.response      # the decrypted answer, client.error if the handshake failed

Python's ssl does the TLS; the records travel as the module's socket reads and writes."""

import re
import ssl
import sys


def _context(ca, cert):
    """A client context of the plain ssl module: an injected truststore would check the server against the
    operating system's store instead of `ca`."""
    truststore = None
    if ssl.SSLContext.__module__.endswith("truststore._api"):
        truststore = sys.modules[ssl.SSLContext.__module__]
        truststore.extract_from_ssl()
    try:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        if ca is None:
            ctx.verify_mode = ssl.CERT_NONE
        else:
            ctx.load_verify_locations(cafile=str(ca))
        if cert:
            ctx.load_cert_chain(*map(str, cert))
        return ctx
    finally:
        if truststore is not None:
            truststore.inject_into_ssl()


class TlsClient:
    """Connects, shakes hands, sends `request` and reads the answer until it is complete (HTTP: the headers and
    Content-Length bytes), then hangs up. `ca`: the file the server's certificate must chain to, read when the
    client connects (the app may create it at start); None accepts any certificate. `cert`: (certificate, key)
    files of a client certificate."""

    def __init__(self, request, ca=None, cert=None):
        self.request = request.encode() if isinstance(request, str) else bytes(request)
        self.ca = ca
        self.cert = cert
        self.response = b""
        self.error = None
        self.closed = False
        self.version = None
        self._tls = None
        self._sent = False

    def _pump(self):
        if self.version is None:
            try:
                self._tls.do_handshake()
            except ssl.SSLWantReadError:
                return
            self.version = self._tls.version()
        if not self._sent:
            self._tls.write(self.request)
            self._sent = True
        try:
            while True:
                chunk = self._tls.read(16384)
                if not chunk:
                    self.closed = True   # the server said close_notify
                    return
                self.response += chunk
        except ssl.SSLWantReadError:
            pass
        head, sep, body = self.response.partition(b"\r\n\r\n")
        length = re.search(rb"(?im)^content-length:\s*(\d+)", head)
        if sep and length is not None and len(body) >= int(length[1]):
            self.closed = True

    def __call__(self, data):
        if data is None:
            self._in, self._out = ssl.MemoryBIO(), ssl.MemoryBIO()
            self._tls = _context(self.ca, self.cert).wrap_bio(self._in, self._out)
        elif self.closed:
            return b""
        else:
            self._in.write(data)
        try:
            self._pump()
        except ssl.SSLError as e:    # a certificate that does not chain to `ca`, an alert from the server
            self.error = e
            self.closed = True
        return self._out.read()
