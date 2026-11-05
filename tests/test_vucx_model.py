"""vucx models without a pty: the generated BLE peer GATT database and the socket network."""

import zlib

import pytest

pytest.importorskip("yaml")

from harness.vucx import GattServer, Network, Vucx, peer_table, table  # noqa: E402


def test_peer_table_lays_out_handles_like_a_gatt_server():
    services, cmds, values = peer_table({"start": 100, "services": [
        {"uuid": "180F", "chars": [["2A19", "R N", "5A"]]},
        {"uuid": "180A", "chars": [["2A29", "R", "text:u-blox"], ["2A4D", "R N", "00", {"2908": "0101"}]]}]})
    assert values == {"2A19": 102, "2A29": 106, "2A4D": 108}
    assert services == ["+UBTGPSD:0,100,103,180F", "+UBTGPSD:0,104,110,180A"]
    assert cmds["AT+UBTGSCD=0,100,103"] == ["+UBTGSCD:0,101,12,102,2A19", "OK"]
    assert cmds["AT+UBTGR=0,102"] == ["+UBTGR:0,102,5A", "OK"]
    assert cmds["AT+UBTGCDD=0,102,103"] == ["+UBTGCDD:0,102,103,2902", "OK"]
    assert cmds["AT+UBTGR=0,106"] == ["+UBTGR:0,106,752D626C6F78", "OK"]
    assert cmds["AT+UBTGCDD=0,106,106"] == ["OK"]
    assert cmds["AT+UBTGCDD=0,108,110"] == ["+UBTGCDD:0,108,109,2902", "+UBTGCDD:0,108,110,2908", "OK"]
    assert cmds["AT+UBTGR=0,110"] == ["+UBTGR:0,110,0101", "OK"]


def test_nora_profile_peer_keeps_its_own_services_first():
    lines = table("nora-w36.yaml")["commands"]["AT+UBTGPSD=*"]
    assert lines[0].endswith(",1800") and lines[-1] == "OK"
    assert any(line.endswith(",1812") for line in lines) and lines.count("OK") == 1


def test_virtual_rssi_varies_within_a_realistic_range():
    """wifi.rssi on the virtual module moves like a real link instead of repeating one value."""
    module = Vucx("nora-w36.yaml")
    values = [int(module._lookup(module.commands, "AT+UWSST=4")[0].split(",")[1]) for _ in range(12)]
    assert len(set(values)) >= 4, values
    assert all(-55 <= v <= -35 for v in values), values
    assert module._lookup(module.commands, "AT+UWSST=0") == ['+UWSST:0,"vucx-net"', "OK"]


def test_network_tcp_echo_and_close():
    net = Network({})
    assert net.handle("AT+USOCR=6", b"") == (["+USOCR:0", "OK"], [])
    assert net.handle('AT+USOC=0,"192.168.1.10",80', b"") == (["OK"], ["+UESOC:0"])
    assert net.handle("AT+USOWB=0", b"hello") == (["+USOWB:0,5", "OK"], ["+UESODA:0,5"])
    assert net.handle("AT+USORB=0,3", b"") == ([("+USORB:0", b"hel"), "OK"], [])
    assert net.handle("AT+USORB=0,64", b"") == ([("+USORB:0", b"lo"), "OK"], [])
    assert net.handle("AT+USORB=0,64", b"") == ([("+USORB:0", b""), "OK"], [])
    assert net.handle("AT+USOST?", b"")[0] == ["+USOST:0,6,2", "OK"]
    assert net.handle("AT+USOCL=0", b"") == (["OK"], [])
    assert net.handle("AT+USOCL=0", b"") == (["ERROR"], [])
    assert net.handle("AT+UWSC=0", b"") is None


def test_network_ntrip_coap_and_accept():
    net = Network({"servers": {2101: "ntrip", 5683: "coap"}, "accept": [8000]})
    net.handle("AT+USOCR=6", b"")
    net.handle("AT+USOC=0,caster.local,2101", b"")
    net.handle("AT+USOWB=0", b"GET /MP HTTP/1.0\r\n\r\n")
    data = net.handle("AT+USORB=0,512", b"")[0][0][1]
    assert data.startswith(b"ICY 200 OK\r\n\r\n\xd3")
    net.handle("AT+USOCR=17", b"")
    net.handle("AT+USOSTB=1,192.168.1.10,5683", bytes([0x41, 0x01, 0x12, 0x34, 0xAB]) + b"\xb4temp")
    text, reply = net.handle("AT+USORFB=1,256", b"")[0][0]
    assert text == "+USORFB:1,192.168.1.10,5683"
    assert reply == bytes([0x61, 0x45, 0x12, 0x34, 0xAB]) + b"\xff21.5"
    assert net.handle("AT+USOL=0,8000", b"") == (["OK"], ["+UESOIC:2,192.168.1.10,0"])


def test_network_udp_client_sends_to_a_bound_accept_port():
    net = Network({"accept": [4433]})
    net.handle("AT+USOCR=17", b"")
    assert net.handle("AT+USOB=0,5000", b"") == (["OK"], [])
    net.handle("AT+USOCR=17", b"")
    assert net.handle("AT+USOB=1,4433", b"") == (["OK"], ["+UESODA:1,31"])
    assert net.handle("AT+USORFB=1,64", b"")[0][0] == ("+USORFB:1,192.168.1.10,50000",
                                                       b"hello from the vucx demo client")


def _ft(net, sock, ch, pkt):
    """One framed ft packet to the TCP server; the frames it answers with as (channel, packet)."""
    net.handle(f"AT+USOWB={sock}", len(pkt).to_bytes(2, "little") + bytes([ch]) + pkt)
    data = net.handle(f"AT+USORB={sock},65535", b"")[0][0][1]
    out = []
    while data:
        n = 3 + int.from_bytes(data[:2], "little")
        out.append((data[2], data[3:n]))
        data = data[n:]
    return out


def test_network_ft_server_get_put_list_and_delete():
    net = Network({"servers": {6970: "ft"}})
    net.handle("AT+USOCR=6", b"")
    net.handle("AT+USOC=0,192.168.1.10,6970", b"")
    big = net.ft.files["big.bin"]
    out = _ft(net, 0, 0, b"\x01" + (1000).to_bytes(2, "little") + b"big.bin")
    assert out[0] == (0, b"\x81\0" + len(big).to_bytes(4, "little") + zlib.crc32(big).to_bytes(4, "little"))
    assert [p[:2] for _, p in out[1:]] == [b.to_bytes(2, "little") for b in range(8)]
    got = b"".join(p[2:] for _, p in out[1:])
    for nxt in (8, 16):   # the client acks every 8 blocks: the next window
        out = _ft(net, 0, 0, b"\x05" + nxt.to_bytes(2, "little"))
        got += b"".join(p[2:] for _, p in out)
    assert got == big and len(out[-1][1]) - 2 < 1000   # 20 blocks, the last one short
    assert _ft(net, 0, 0, b"\x05" + (21).to_bytes(2, "little")) == []
    assert _ft(net, 0, 0, b"\x01\x00\x01nope.txt") == [(0, b"\x81\x01")]

    body = bytes(range(256)) * 3 + bytes(82)   # 850 bytes in blocks of 100: 8 full, an ACK, then the short last one
    head = (100).to_bytes(2, "little") + len(body).to_bytes(4, "little") + zlib.crc32(body).to_bytes(4, "little")
    assert _ft(net, 0, 0, b"\x02" + head + b"log.txt") == [(0, b"\x82\0")]
    for b in range(7):
        assert _ft(net, 0, 1, b.to_bytes(2, "little") + body[b * 100:(b + 1) * 100]) == []
    assert _ft(net, 0, 1, (7).to_bytes(2, "little") + body[700:800]) == [(0, b"\x85\x08\x00")]
    assert _ft(net, 0, 1, (8).to_bytes(2, "little") + body[800:]) == [(0, b"\x85\x09\x00"), (0, b"\x87\x00")]
    assert net.ft.files["log.txt"] == body
    listing = _ft(net, 0, 0, b"\x03")
    assert (0, b"\x83\0" + len(body).to_bytes(4, "little") + b"log.txt") in listing and listing[-1] == (0, b"\x83\xff")
    assert _ft(net, 0, 0, b"\x04log.txt") == [(0, b"\x84\x00")]
    assert _ft(net, 0, 0, b"\x04log.txt") == [(0, b"\x84\x01")]

    bad = (100).to_bytes(2, "little") + (3).to_bytes(4, "little") + (0).to_bytes(4, "little")
    assert _ft(net, 0, 0, b"\x08" + bad + b"x.txt") == [(0, b"\x88\0\0\0\0\0")]
    assert _ft(net, 0, 1, b"\0\0abc") == [(0, b"\x85\x01\x00"), (0, b"\x87\x06")]   # CRC mismatch
    assert "x.txt" not in net.ft.files


def test_ft_over_the_ble_peer():
    v = Vucx("nora-w36.yaml")
    ctrl, data = v.ft_peer
    assert v._ft_ble(f"AT+UBTGWNR=0,{ctrl},03")[0] == ["OK"]
    urcs = v._ft_ble(f"AT+UBTGWNR=0,{ctrl}," + (b"\x01\xf0\x00notes.txt").hex())[1]
    assert urcs[0].startswith(f"+UEBTGCN:0,{ctrl},8100") and urcs[1].startswith(f"+UEBTGCN:0,{data},0000")
    assert bytes.fromhex(urcs[1].split(",")[2])[2:] == v.ft.files["notes.txt"]
    assert v._ft_ble("AT+UBTGWNR=0,999,03") is None


def test_gatt_server_hands_out_handles_like_the_module():
    g = GattServer()
    assert g.handle("AT+UBTGS=6E400001B5A3F393E0A9E50E24DCCA9E,16") == ["+UBTGS:40", "OK"]
    assert g.handle("AT+UBTGC=6E400003B5A3F393E0A9E50E24DCCA9E,10,1,1,00,244") == ["+UBTGC:42,43", "OK"]
    assert g.handle("AT+UBTGHCC=6E400002B5A3F393E0A9E50E24DCCA9E,04") == ["+UBTGHCC:45,46", "OK"]
    assert g.handle("AT+UBTGD=2908,0101") == ["+UBTGD:47", "OK"]
    assert g.handle("AT+UBTGS=1812,2") == ["+UBTGS:60", "OK"]
    assert g.handle("AT+UBTGC=2A4D,12") == ["+UBTGC:62,63", "OK"]
    assert g.handle("AT+UBTGSA") is None
    urcs = ["+UEBTGCW:0,{c:2A4D},0100,0", "+UEBTGCW:0,{h:6E400002},41,0", "+UEBTGCW:0,{h:7A3F0011},41,0"]
    assert g.fill(urcs) == ["+UEBTGCW:0,63,0100,0", "+UEBTGCW:0,45,41,0"]
    assert g.fill(urcs, "AT+UBTGSA") == ["+UEBTGCW:0,63,0100,0", "+UEBTGCW:0,45,41,0"]
    g.active = len(g.chars)   # activated: the next activation only sees what is defined after it
    g.handle("AT+UBTGS=7A3F0010,16")
    g.handle("AT+UBTGC=7A3F0011,08")
    assert g.fill(urcs, "AT+UBTGSA") == ["+UEBTGCW:0,82,41,0"]
    g.reset()
    assert g.handle("AT+UBTGS=180F,2") == ["+UBTGS:40", "OK"] and g.fill(urcs) == []


def test_network_sps_echo():
    net = Network({})
    assert net.handle("AT+USPSC=0", b"") == (["OK"], ["+UESPSC:0"])
    assert net.handle("AT+USPSWB=0", b"ping") == (["+USPSWB:0,4", "OK"], ["+UESPSDA:0,4"])
    assert net.handle("AT+USPSRB=0,2", b"") == ([("+USPSRB:0", b"pi"), "OK"], [])
    assert net.handle("AT+USPSRB=0,64", b"") == ([("+USPSRB:0", b"ng"), "OK"], [])
    assert net.handle("AT+USPSWB=1", b"x") is None
