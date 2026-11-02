"""Reads the pcapng files written by at.trace / at.trace.pcap: one interface, exported-PDU frames."""

import struct


def read(path):
    """Returns (interfaces, frames). interfaces: [{linktype, name, tsresol}]; frames: [{ts, out, tags, payload}],
    ts in interface units, out True for outbound (TX), tags {tag: bytes} from the exported-PDU header."""
    data = path.read_bytes()
    assert struct.unpack_from("<I", data, 0)[0] == 0x0A0D0D0A, "no pcapng section header"
    assert struct.unpack_from("<I", data, 8)[0] == 0x1A2B3C4D, "not little-endian pcapng"
    interfaces, frames, pos = [], [], 0
    while pos < len(data):
        btype, blen = struct.unpack_from("<II", data, pos)
        assert blen % 4 == 0 and blen >= 12 and pos + blen <= len(data), f"bad block at {pos}"
        assert struct.unpack_from("<I", data, pos + blen - 4)[0] == blen, f"trailing length differs at {pos}"
        body = data[pos + 8:pos + blen - 4]
        if btype == 1:
            linktype = struct.unpack_from("<H", body, 0)[0]
            opts = _options(body[8:])
            interfaces.append({"linktype": linktype, "name": opts.get(2, b"").decode(),
                               "tsresol": opts.get(9, b"\x06")[0]})
        elif btype == 6:
            _, hi, lo, caplen, origlen = struct.unpack_from("<IIIII", body, 0)
            assert caplen == origlen
            pdu = body[20:20 + caplen]
            opts = _options(body[20 + caplen + (-caplen % 4):])
            flags = struct.unpack("<I", opts[2])[0] if 2 in opts else 0
            tags, off = {}, 0
            while True:
                tag, tlen = struct.unpack_from(">HH", pdu, off)
                off += 4
                if tag == 0:
                    break
                tags[tag] = pdu[off:off + tlen]
                off += tlen
            frames.append({"ts": (hi << 32) | lo, "out": flags & 3 == 2, "tags": tags, "payload": pdu[off:]})
        pos += blen
    return interfaces, frames


def _options(raw):
    opts, off = {}, 0
    while off + 4 <= len(raw):
        code, olen = struct.unpack_from("<HH", raw, off)
        if code == 0:
            break
        opts[code] = raw[off + 4:off + 4 + olen]
        off += 4 + olen + (-olen % 4)
    return opts
