"""Soak: the module stays connected for hours and is checked every few minutes.

Run with `.\\ucxapp --detect --test tests/hardware -m soak`. UCX_SOAK_HOURS sets the length (default 1; run 24
before a release) and UCX_SOAK_INTERVAL_S the time between checks (default 600). Every check appends a row to
build/soak.csv: uptime, Wi-Fi state, signal, ping to the gateway, sockets and links left open.
"""

import csv
import os
import re
import time
from datetime import datetime
from pathlib import Path

import pytest
from hwutil import PING_RE, _join_wifi, _reboot, _run, _wifi_connect_command

pytestmark = [pytest.mark.soak, pytest.mark.hardware, pytest.mark.wifi]

CSV = Path(__file__).resolve().parents[2] / "build" / "soak.csv"
FIELDS = ["time", "round", "uptime_s", "wifi", "rssi_dbm", "ping_sent", "ping_received", "ping_avg_ms",
          "sockets", "links", "reconnects"]
PING_MAX_AVG_MS = 200


def _uptime_s(text):
    m = re.search(r"^(\d+) days (\d\d):(\d\d):(\d\d)$", text, re.M)
    return None if m is None else ((int(m[1]) * 24 + int(m[2])) * 60 + int(m[3])) * 60 + int(m[4])


def _count(pattern, text):
    m = re.search(pattern, text)
    return None if m is None else int(m[1])


def test_soak(app_exe, port_a):
    hours = float(os.environ.get("UCX_SOAK_HOURS", "1"))
    interval = int(os.environ.get("UCX_SOAK_INTERVAL_S", "600"))
    connect = _wifi_connect_command()
    _reboot(app_exe, port_a)
    _, gateway = _join_wifi(app_exe, port_a, connect)
    CSV.parent.mkdir(exist_ok=True)
    end = time.time() + hours * 3600
    last_uptime, reconnects, rounds = -1, 0, 0
    print(f"soak: {hours:g} h, a check every {interval} s, rows in {CSV}")
    with CSV.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        while True:
            rounds += 1
            out = _run(app_exe, port_a, "dev.uptime", "wifi.status", timeout=60)
            if "ip=" not in out.stdout:
                # Access points restart and channels change; the product must get back on its own terms.
                reconnects += 1
                _join_wifi(app_exe, port_a, connect)
            out = _run(app_exe, port_a, "dev.uptime", "wifi.status", f"net.ping {gateway} 4", "sock.open udp",
                       "sock.close 0", "sock.list", "bt.list", timeout=120)
            uptime = _uptime_s(out.stdout)
            ping = PING_RE.search(out.stdout)
            row = {
                "time": datetime.now().isoformat(timespec="seconds"), "round": rounds, "uptime_s": uptime,
                "wifi": int("wifi: connected" in out.stdout), "rssi_dbm": _count(r"rssi=(-?\d+)", out.stdout),
                "ping_sent": ping and int(ping[1]), "ping_received": ping and int(ping[2]),
                "ping_avg_ms": ping and int(ping[3]), "sockets": _count(r"(\d+) socket\(s\)", out.stdout),
                "links": _count(r"(\d+) connection\(s\)", out.stdout), "reconnects": reconnects,
            }
            writer.writerow(row)
            f.flush()
            left = max(0, end - time.time())
            print(f"soak round {rounds}: uptime {uptime} s, rssi {row['rssi_dbm']} dBm, ping {row['ping_avg_ms']} ms,"
                  f" sockets {row['sockets']}, links {row['links']}, {left / 60:.0f} min left")
            assert uptime is not None and uptime > last_uptime, f"module rebooted by itself:\n{out.stdout[-800:]}"
            assert ping is not None and ping[2] != "0", f"gateway {gateway} unreachable:\n{out.stdout[-800:]}"
            assert int(ping[3]) < PING_MAX_AVG_MS, f"ping to the gateway too slow:\n{out.stdout[-800:]}"
            assert row["sockets"] == 0, f"socket leak:\n{out.stdout[-800:]}"
            assert row["links"] == 0, f"link leak:\n{out.stdout[-800:]}"
            last_uptime = uptime
            if left <= 0:
                break
            time.sleep(min(interval, left))
    assert reconnects <= max(1, rounds // 10), f"Wi-Fi dropped {reconnects} times in {rounds} rounds, see {CSV}"
