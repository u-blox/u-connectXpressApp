"""What the Matter plug-in shows of a node's setup codes and traffic, for the tests to check against."""

import re


def at_burst(trace, first, last):
    """Timing of a run of AT commands in an at.trace file, from the command `first` to the answer of the next
    `last` (a command that ends in "=" stands for any parameters, another one for itself): (seconds for the
    whole run, the longest wait between an answer and the next command, the commands). u-connectMatter needed a
    session lock here: its receive thread took the AT client between two commands and each one then waited 0.2
    to 1.7 s. ucxapp's client must not need it."""
    def sends(kind, text, command):
        return kind == "TX" and (text.startswith(command) if command.endswith("=") else text == command)

    lines = [(float(m[1]), m[2], m[3]) for m in re.finditer(r"^(\d+\.\d+) (\w+) (.*)$", trace, re.M)]
    start = next(i for i, (_, kind, text) in enumerate(lines) if sends(kind, text, first))
    end = next(i for i, (_, kind, text) in enumerate(lines) if i >= start and sends(kind, text, last))
    done = next(i for i, (_, kind, text) in enumerate(lines) if i > end and kind == "RX" and text in ("OK", "ERROR"))
    sent = [i for i in range(start, end + 1) if lines[i][1] == "TX"]
    gap = max((lines[i][0] - lines[i - 1][0] for i in sent[1:]), default=0.0)
    return lines[done][0] - lines[start][0], gap, [lines[i][2] for i in sent]


def setup_codes(status):
    """Discriminator and passcode of a node, from what matter.status prints. They are the node's own, drawn at
    random on its first start and kept in its store (matter/src/UbloxCommissionableDataProvider.h)."""
    found = re.search(r"setup: discriminator (\d+), vendor 0x[0-9A-F]{4}, product 0x[0-9A-F]{4}, passcode (\d{8})\n",
                      status)
    assert found, status
    return int(found[1]), int(found[2])


def codes_allowed(discriminator, passcode):
    """Neither a passcode Matter forbids nor what every SDK example has (3840 to 3842, 20202021)."""
    return (0 <= discriminator <= 0xFFF and discriminator not in (3840, 3841, 3842) and 1 <= passcode <= 99999998
            and passcode % 11111111 != 0 and passcode not in (12345678, 87654321, 20202021))


def manual_code(discriminator, passcode):
    """The manual pairing code without its check digit, as matter.qr prints it."""
    short = discriminator >> 8
    digits = f"{short >> 2}{((short & 3) << 14) | (passcode & 0x3FFF):05d}{passcode >> 14:04d}"
    return f"{digits[:4]}-{digits[4:7]}-{digits[7:]}"


def manual_decode(code):
    """(the upper four bits of the discriminator, passcode) of a manual pairing code as matter.qr prints it."""
    digits = code.replace("-", "")
    low = int(digits[1:6])
    return (int(digits[0]) << 2) | (low >> 14), (int(digits[6:10]) << 14) | (low & 0x3FFF)


_VERHOEFF_D = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9], [1, 2, 3, 4, 0, 6, 7, 8, 9, 5], [2, 3, 4, 0, 1, 7, 8, 9, 5, 6],
               [3, 4, 0, 1, 2, 8, 9, 5, 6, 7], [4, 0, 1, 2, 3, 9, 5, 6, 7, 8], [5, 9, 8, 7, 6, 0, 4, 3, 2, 1],
               [6, 5, 9, 8, 7, 1, 0, 4, 3, 2], [7, 6, 5, 9, 8, 2, 1, 0, 4, 3], [8, 7, 6, 5, 9, 3, 2, 1, 0, 4],
               [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]]
_VERHOEFF_P = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9], [1, 5, 7, 6, 2, 8, 3, 0, 9, 4]]
for _ in range(6):
    _VERHOEFF_P.append([_VERHOEFF_P[1][d] for d in _VERHOEFF_P[-1]])
_VERHOEFF_INV = [0, 4, 3, 2, 1, 5, 6, 7, 8, 9]


def verhoeff_check(digits):
    """The Verhoeff check digit of a string of digits, which ends a manual pairing code."""
    c = 0
    for i, digit in enumerate(reversed(digits)):
        c = _VERHOEFF_D[c][_VERHOEFF_P[(i + 1) % 8][int(digit)]]
    return str(_VERHOEFF_INV[c])


BASE38 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."


def qr_decode(payload):
    """The fields of a QR code payload (MT:...) without optional TLV data: base38 in chunks of 5, 4 or 2 characters
    for 3, 2 or 1 bytes, each chunk least significant character first, then the bits from the least significant
    one up: version 3, vendor 16, product 16, commissioning flow 2, rendezvous 8, discriminator 12, passcode 27,
    padding 4."""
    assert payload.startswith("MT:"), payload
    text, data = payload[3:], bytearray()
    while text:
        chunk, text = text[:5], text[5:]
        assert len(chunk) in (5, 4, 2), payload
        size = {5: 3, 4: 2, 2: 1}[len(chunk)]
        value = sum(BASE38.index(ch) * 38 ** i for i, ch in enumerate(chunk))
        assert value < 256 ** size, payload
        data += value.to_bytes(size, "little")
    bits = int.from_bytes(data, "little")
    fields = {}
    for name, width in (("version", 3), ("vendor", 16), ("product", 16), ("flow", 2), ("rendezvous", 8),
                        ("discriminator", 12), ("passcode", 27), ("padding", 4)):
        fields[name] = bits & ((1 << width) - 1)
        bits >>= width
    assert len(data) == 11 and bits == 0, payload   # nothing after the padding: no TLV data
    return fields


PAIRING_NODE = bytes.fromhex("8877665544332211")
PAIRING_EXCHANGE = 0x1234
PBKDF_PARAM_RESPONSE = 0x21


def pairing_request(counter=0x01234567, exchange=PAIRING_EXCHANGE):
    """A commissioner's first message of a pairing over the network: PBKDFParamRequest on an unsecured session,
    from node PAIRING_NODE on exchange PAIRING_EXCHANGE, asking for an answer (reliable)."""
    message = bytes([0x04, 0, 0, 0]) + counter.to_bytes(4, "little") + PAIRING_NODE
    exchange = bytes([0x05, 0x20]) + exchange.to_bytes(2, "little") + bytes(2)
    # initiator random, initiator session 0x1234, passcode id 0, no PBKDF parameters known
    payload = b"\x15\x30\x01\x20" + bytes(range(32)) + b"\x25\x02\x34\x12\x24\x03\x00\x28\x04\x18"
    return message + exchange + payload


def unsecured_message(message):
    """(destination node, opcode, exchange) of a Matter message on an unsecured session; the answer to
    pairing_request() is (PAIRING_NODE, PBKDF_PARAM_RESPONSE, PAIRING_EXCHANGE)."""
    at = 8 + (8 if message[0] & 4 else 0)
    size = {0: 0, 1: 8, 2: 2}.get(message[0] & 3, 0)
    return message[at:at + size], message[at + size + 1], int.from_bytes(message[at + size + 2:at + size + 4], "little")
