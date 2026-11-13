"""A Matter commissioner's side of PASE (passcode pairing) over UDP, in plain Python, for the tests: it pairs with
the node through the virtual module (vucx peers:), then reads an attribute over the encrypted session. The SPAKE2+
part is checked against the SDK's own test vector (tests/test_pase.py), so a node that pairs but cannot encrypt
or decrypt its session (as with the raw session keystore under the PSA crypto backend) fails here, not on a phone.

Matter 1.4 core specification 4.14.1 (PASE), 3.10 (SPAKE2+ draft-01), 4.4 (message format), A (TLV)."""

import hashlib
import hmac
import os

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

# NIST P-256
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def decode(point):
    """A P-256 point from its SEC1 encoding (compressed or not)."""
    x = int.from_bytes(point[1:33], "big")
    if point[0] == 4:
        return x, int.from_bytes(point[33:65], "big")
    y = pow((x * x * x - 3 * x + B) % P, (P + 1) // 4, P)
    return x, y if (y & 1) == (point[0] & 1) else P - y


def encode(pt):
    return b"\x04" + pt[0].to_bytes(32, "big") + pt[1].to_bytes(32, "big")


def add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % P == 0:
        return None
    if a == b:
        s = (3 * a[0] * a[0] - 3) * pow(2 * a[1], -1, P) % P
    else:
        s = (b[1] - a[1]) * pow(b[0] - a[0], -1, P) % P
    x = (s * s - a[0] - b[0]) % P
    return x, (s * (a[0] - x) - a[1]) % P


def mul(k, pt):
    out = None
    while k:
        if k & 1:
            out = add(out, pt)
        pt = add(pt, pt)
        k >>= 1
    return out


def neg(pt):
    return pt[0], P - pt[1]


M = decode(bytes.fromhex("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"))
NN = decode(bytes.fromhex("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"))


def hkdf(ikm, salt, info, length):
    prk = hmac.new(salt or bytes(32), ikm, hashlib.sha256).digest()
    out, block = b"", b""
    for i in range((length + 31) // 32):
        block = hmac.new(prk, block + info + bytes([i + 1]), hashlib.sha256).digest()
        out += block
    return out[:length]


def passcode_w(passcode, salt, iterations):
    """(w0, w1) of a passcode, as the commissioner and the node's verifier both derive them."""
    ws = hashlib.pbkdf2_hmac("sha256", passcode.to_bytes(4, "little"), salt, iterations, 80)
    return int.from_bytes(ws[:40], "big") % N, int.from_bytes(ws[40:], "big") % N


def prover(context, w0, w1, x, y_point, id_p=b"", id_v=b""):
    """The prover's (commissioner's) SPAKE2+ with its scalar x and the verifier's share Y: a dict with X, Z, V,
    Ka, Ke, KcA, KcB, cA (its confirmation, HMAC over Y) and cB (what the verifier's must be, HMAC over X)."""
    X = add(mul(x, G), mul(w0, M))
    Yw = add(decode(y_point), neg(mul(w0, NN)))
    Z, V = mul(x, Yw), mul(w1, Yw)
    tt = b""
    for item in (context, id_p, id_v, encode(M), encode(NN), encode(X), y_point, encode(Z), encode(V),
                 w0.to_bytes(32, "big")):
        tt += len(item).to_bytes(8, "little") + item
    k = hashlib.sha256(tt).digest()
    kc = hkdf(k[:16], b"", b"ConfirmationKeys", 32)
    return {"X": encode(X), "Z": encode(Z), "V": encode(V), "Ka": k[:16], "Ke": k[16:], "KcA": kc[:16],
            "KcB": kc[16:], "cA": hmac.new(kc[:16], y_point, hashlib.sha256).digest(),
            "cB": hmac.new(kc[16:], encode(X), hashlib.sha256).digest()}


def tlv(data, at=0):
    """One TLV element: (tag, value, next offset); a struct or list is a dict by tag, an array a list."""
    control = data[at]
    at += 1
    size = {0x00: 0, 0x20: 1, 0x40: 2, 0x60: 4, 0x80: 2, 0xA0: 4, 0xC0: 6, 0xE0: 8}[control & 0xE0]
    tag = int.from_bytes(data[at:at + size], "little") if size else None
    at += size
    kind = control & 0x1F
    if kind <= 0x07:
        n = 1 << (kind & 3)
        return tag, int.from_bytes(data[at:at + n], "little", signed=kind < 4), at + n
    if kind in (0x08, 0x09):
        return tag, kind == 0x09, at
    if 0x0C <= kind <= 0x13:
        n = 1 << (kind & 3)
        length = int.from_bytes(data[at:at + n], "little")
        return tag, bytes(data[at + n:at + n + length]), at + n + length
    if kind == 0x14:
        return tag, None, at
    if kind in (0x15, 0x16, 0x17):
        items = []
        while data[at] != 0x18:
            t, v, at = tlv(data, at)
            items.append((t, v))
        return tag, dict(items) if kind != 0x16 else [v for _, v in items], at + 1
    raise ValueError(f"TLV type {kind:#x}")


def _octets(tag, value):
    return bytes([0x30, tag, len(value)]) + value


SECURE_CHANNEL, INTERACTION_MODEL = 0x0000, 0x0001
PBKDF_REQUEST, PBKDF_RESPONSE, PAKE1, PAKE2, PAKE3, STANDALONE_ACK, STATUS_REPORT = (
    0x20, 0x21, 0x22, 0x23, 0x24, 0x10, 0x40)
READ_REQUEST, REPORT_DATA = 0x02, 0x05

# Read Basic Information (cluster 0x0028) VendorID (attribute 2) of endpoint 0, not fabric filtered
READ_VENDOR_ID = bytes.fromhex("1536001724020024032824040218182803 24FF0C18".replace(" ", ""))
NODE_LABEL = 5


def read_basic_information(attribute):
    """READ_VENDOR_ID for another attribute of Basic Information (below 0x100)."""
    return READ_VENDOR_ID[:12] + bytes([attribute]) + READ_VENDOR_ID[13:]


def parse(message):
    """(session, header bytes, counter, source node or None, rest) of a Matter message."""
    flags, session, counter = message[0], int.from_bytes(message[1:3], "little"), message[4:8]
    at = 8
    source = None
    if flags & 4:
        source, at = message[at:at + 8], at + 8
    at += {1: 8, 2: 2}.get(flags & 3, 0)
    return session, message[:at], int.from_bytes(counter, "little"), source, message[at:]


def exchange_header(payload):
    """(exchange flags, opcode, exchange, protocol, acknowledged counter or None, application payload)."""
    flags, opcode, exchange = payload[0], payload[1], int.from_bytes(payload[2:4], "little")
    at = 4 + (2 if flags & 0x10 else 0)
    protocol = int.from_bytes(payload[at:at + 2], "little")
    at += 2
    ack = None
    if flags & 0x02:
        ack, at = int.from_bytes(payload[at:at + 4], "little"), at + 4
    if flags & 0x08:
        at += 2 + int.from_bytes(payload[at:at + 2], "little")
    return flags, opcode, exchange, protocol, ack, payload[at:]


class Commissioner:
    """A commissioner that pairs with the passcode over UDP and then reads the node's vendor ID over the session.
    Called with None for its first datagram and with each datagram the node sends it, it returns what to send
    back. state: "pbkdf", "pake1", "pake3", "paired", "read" or "failed: ..."; report is the ReportData payload."""

    NODE = bytes.fromhex("8877665544332211")

    def __init__(self, passcode, session=0x2345, exchange=0x3456, attribute=2):
        self.passcode, self.session, self.exchange = passcode, session, exchange
        self.read = read_basic_information(attribute)
        self.counter = int.from_bytes(os.urandom(3), "little") + 1
        self.secure_counter = 0
        self.state, self.report, self.seen = "pbkdf", None, set()
        self.request = b"\x15" + _octets(1, os.urandom(32)) + bytes([0x25, 2]) + session.to_bytes(2, "little") + \
            b"\x24\x03\x00\x28\x04\x18"
        self.keys = None

    def _unsecured(self, opcode, payload, ack=None, reliable=True):
        self.counter += 1
        header = bytes([0x04, 0, 0, 0]) + self.counter.to_bytes(4, "little") + self.NODE
        flags = 0x01 | (0x04 if reliable else 0) | (0x02 if ack is not None else 0)
        return header + bytes([flags, opcode]) + self.exchange.to_bytes(2, "little") + \
            SECURE_CHANNEL.to_bytes(2, "little") + (ack.to_bytes(4, "little") if ack is not None else b"") + payload

    def _secure(self, protocol, opcode, exchange, payload, ack=None, reliable=True):
        self.secure_counter += 1
        header = bytes([0x00]) + self.peer_session.to_bytes(2, "little") + b"\x00" + \
            self.secure_counter.to_bytes(4, "little")
        flags = 0x01 | (0x04 if reliable else 0) | (0x02 if ack is not None else 0)
        plain = bytes([flags, opcode]) + exchange.to_bytes(2, "little") + protocol.to_bytes(2, "little") + \
            (ack.to_bytes(4, "little") if ack is not None else b"") + payload
        nonce = b"\x00" + self.secure_counter.to_bytes(4, "little") + bytes(8)
        return header + AESCCM(self.keys[:16], tag_length=16).encrypt(nonce, plain, header)

    def __call__(self, data):
        try:
            return self._step(data)
        except Exception as e:      # the test sees it in state, the virtual module goes on
            self.state = f"failed: {e!r}"
            return []

    def _step(self, data):
        if data is None:
            self.state = "pbkdf"
            return [self._unsecured(PBKDF_REQUEST, self.request)]
        session, header, counter, source, rest = parse(data)
        if (session, counter) in self.seen:      # a retransmission: acknowledged already
            return []
        self.seen.add((session, counter))
        if session == self.session and self.keys is not None:
            nonce = header[3:4] + counter.to_bytes(4, "little") + (source or bytes(8))
            rest = AESCCM(self.keys[16:32], tag_length=16).decrypt(nonce, rest, header)
            flags, opcode, exchange, protocol, _, payload = exchange_header(rest)
            if protocol == INTERACTION_MODEL and opcode == REPORT_DATA:
                self.report, self.state = payload, "read"
            if flags & 0x04:
                return [self._secure(SECURE_CHANNEL, STANDALONE_ACK, exchange, b"", ack=counter, reliable=False)]
            return []
        if session != 0:
            raise ValueError(f"message on session {session}")
        flags, opcode, _, protocol, _, payload = exchange_header(rest)
        if protocol != SECURE_CHANNEL or opcode == STANDALONE_ACK:
            return []
        if opcode == PBKDF_RESPONSE and self.state == "pbkdf":
            _, fields, _ = tlv(payload)
            self.peer_session = fields[3]
            self.w0, self.w1 = passcode_w(self.passcode, fields[4][2], fields[4][1])
            self.context = hashlib.sha256(b"CHIP PAKE V1 Commissioning" + self.request + payload).digest()
            self.x = int.from_bytes(os.urandom(32), "big") % (N - 1) + 1
            self.X = encode(add(mul(self.x, G), mul(self.w0, M)))
            self.state = "pake1"
            return [self._unsecured(PAKE1, b"\x15" + bytes([0x30, 1, 65]) + self.X + b"\x18", ack=counter)]
        if opcode == PAKE2 and self.state == "pake1":
            _, fields, _ = tlv(payload)
            k = prover(self.context, self.w0, self.w1, self.x, fields[1])
            if not hmac.compare_digest(k["cB"], fields[2]):
                raise ValueError("the node's confirmation does not match: wrong passcode")
            self.Ke = k["Ke"]
            self.state = "pake3"
            return [self._unsecured(PAKE3, b"\x15" + _octets(1, k["cA"]) + b"\x18", ack=counter)]
        if opcode == STATUS_REPORT and self.state == "pake3":
            general, code = int.from_bytes(payload[:2], "little"), int.from_bytes(payload[6:8], "little")
            if general or code:
                raise ValueError(f"the node refused the pairing: status {general}/{code}")
            self.keys = hkdf(self.Ke, b"", b"SessionKeys", 48)
            self.state = "paired"
            out = [self._unsecured(STANDALONE_ACK, b"", ack=counter, reliable=False)] if flags & 0x04 else []
            return out + [self._secure(INTERACTION_MODEL, READ_REQUEST, self.exchange + 1, self.read)]
        if opcode == STATUS_REPORT:
            raise ValueError(f"status report in state {self.state}: {payload.hex()}")
        return []
