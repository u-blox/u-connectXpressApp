"""vucx models without a pty: the generated BLE peer GATT database and the socket network."""

import pytest

pytest.importorskip("yaml")

from harness.vucx import Network, peer_table, table  # noqa: E402


def test_peer_table_lays_out_handles_like_a_gatt_server():
    services, cmds = peer_table({"start": 100, "services": [
        {"uuid": "180F", "chars": [["2A19", "R N", "5A"]]},
        {"uuid": "180A", "chars": [["2A29", "R", "text:u-blox"], ["2A4D", "R N", "00", {"2908": "0101"}]]}]})
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
