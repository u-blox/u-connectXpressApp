"""GNSS correction frames for tests: RTCM3, SPARTN and UBX with correct CRCs (the receiver-side check is in
app/src/ucxapp_corr.c). SPARTN layout and CRCs as in ubxlib's common/spartn (u_spartn.c, u_spartn_crc.c)."""

import struct

_CRC4_BITS = (0x0B, 0x05, 0x0A, 0x07, 0x0E, 0x0F, 0x0D, 0x09)   # ubxlib's u8Crc4Table at 1, 2, 4 .. 128


def crc(data, width, poly, init=0, xorout=0):
    """MSB-first CRC without reflection."""
    top, mask, c = 1 << (width - 1), (1 << width) - 1, init
    for b in data:
        c ^= b << (width - 8)
        for _ in range(8):
            c = ((c << 1) ^ poly if c & top else c << 1) & mask
    return c ^ xorout


def crc4(data):
    r = 0
    for b in data:
        x, r = b ^ r, 0
        for i in range(8):
            if x >> i & 1:
                r ^= _CRC4_BITS[i]
    return r


def rtcm(payload):
    frame = bytes([0xD3, len(payload) >> 8, len(payload) & 0xFF]) + payload
    return frame + crc(frame, 24, 0x864CFB).to_bytes(3, "big")


def corrupt(frame):
    """The frame with its last CRC byte inverted."""
    return frame[:-1] + bytes([frame[-1] ^ 0xFF])


def ubx(cls, mid, payload=b""):
    body = bytes([cls, mid]) + struct.pack("<H", len(payload)) + payload
    a = b = 0
    for x in body:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return b"\xb5\x62" + body + bytes([a, b])


_SPARTN_CRC = ((8, 0x07, 0, 0), (16, 0x1021, 0, 0), (24, 0x864CFB, 0, 0), (32, 0x04C11DB7, 0xFFFFFFFF, 0xFFFFFFFF))
_AUTH_LEN = (8, 12, 16, 32, 64)


def spartn(msg_type, payload, crc_type=1, sub_type=0, time_tag=1234, auth_len=None):
    """A SPARTN frame; auth_len (0..4) makes it encrypted and authenticated with that authentication length."""
    enc = auth_len is not None
    head = (msg_type << 17) | (len(payload) << 7) | (int(enc) << 6) | (crc_type << 4)
    head |= crc4(head.to_bytes(3, "big"))
    long_tag = time_tag > 0xFFFF
    tag_bits = 32 if long_tag else 16
    desc = (((sub_type << 1 | int(long_tag)) << tag_bits | time_tag) << 7 | 1) << 4 | 2   # SID 1, PID 2
    desc = desc.to_bytes((4 + 1 + tag_bits + 7 + 4) // 8, "big")
    extra, auth = b"", b""
    if enc:
        extra = ((1 << 12) | (5 << 6) | (2 << 3) | auth_len).to_bytes(2, "big")   # EID 1, ESN 5, AI 2, AL
        auth = bytes(range(_AUTH_LEN[auth_len]))
    body = head.to_bytes(3, "big") + desc + extra + payload + auth
    width, poly, init, xorout = _SPARTN_CRC[crc_type]
    return b"\x73" + body + crc(body, width, poly, init, xorout).to_bytes(width // 8, "big")
