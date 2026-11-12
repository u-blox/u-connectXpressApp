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


PAIRING_NODE = bytes.fromhex("8877665544332211")
PAIRING_EXCHANGE = 0x1234
PBKDF_PARAM_RESPONSE = 0x21


def pairing_request():
    """A commissioner's first message of a pairing over the network: PBKDFParamRequest on an unsecured session,
    from node PAIRING_NODE on exchange PAIRING_EXCHANGE, asking for an answer (reliable)."""
    message = bytes([0x04, 0, 0, 0]) + (0x01234567).to_bytes(4, "little") + PAIRING_NODE
    exchange = bytes([0x05, 0x20]) + PAIRING_EXCHANGE.to_bytes(2, "little") + bytes(2)
    # initiator random, initiator session 0x1234, passcode id 0, no PBKDF parameters known
    payload = b"\x15\x30\x01\x20" + bytes(range(32)) + b"\x25\x02\x34\x12\x24\x03\x00\x28\x04\x18"
    return message + exchange + payload


def unsecured_message(message):
    """(destination node, opcode, exchange) of a Matter message on an unsecured session; the answer to
    pairing_request() is (PAIRING_NODE, PBKDF_PARAM_RESPONSE, PAIRING_EXCHANGE)."""
    at = 8 + (8 if message[0] & 4 else 0)
    size = {0: 0, 1: 8, 2: 2}.get(message[0] & 3, 0)
    return message[at:at + size], message[at + size + 1], int.from_bytes(message[at + size + 2:at + size + 4], "little")
