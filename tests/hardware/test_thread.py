"""Thread on two modules with Thread firmware (NORA-B20): form, join, roles, mesh view and self-healing.

Covers the Thread use cases in usecase.check mesh and building. The border router cases (thread.join through a
commissioner, SRP registration and DNS-SD) need a border router and only check what the module does on its own.
"""

import os
import re
import time

import pytest
from hwutil import _run

pytestmark = pytest.mark.thread

ROUTER_UPGRADE_S = 180  # a router-eligible child asks to become a router after a random delay of up to 120 s
LEADER_TAKEOVER_S = 240  # the leader timeout (120 s) plus a new partition forming
HEX_RE = re.compile(r"^([0-9a-f]{40,})\s*$", re.M)


def _status(exe, port):
    out = _run(exe, port, "thread.status", timeout=30)
    assert out.returncode == 0, out.stdout
    role = re.search(r"^Role:\s+(\S+)", out.stdout, re.M)
    part = re.search(r"^Partition:\s+(\S+)", out.stdout, re.M)
    return (role.group(1) if role else "off"), (part.group(1) if part else "-"), out.stdout


def _wait_role(exe, port, roles, timeout):
    deadline, last = time.monotonic() + timeout, ""
    while time.monotonic() < deadline:
        role, partition, last = _status(exe, port)
        if role in roles:
            return role, partition
        time.sleep(5)
    pytest.fail(f"{port} not {'/'.join(roles)} after {timeout} s:\n{last}", pytrace=False)


def _fresh(exe, *ports):
    """Thread off, no dataset, router type, no SRP services: every test starts from the same state."""
    for port in ports:
        srp = _run(exe, port, "thread.srp", timeout=30).stdout
        removes = [f"thread.srp remove {i}" for i in re.findall(r"^\s*(\d+)\s+\S+\._\S+ port \d+", srp, re.M)]
        out = _run(exe, port, *removes, "thread.srp off", "thread.stop", "thread.dataset clear", "thread.type router",
                   timeout=30)
        assert out.returncode == 0, out.stdout


def _form(exe, port):
    """Port becomes the leader of a new random network; returns its active dataset as hex."""
    out = _run(exe, port, "thread.network new", "thread.start 60", timeout=90)
    assert out.returncode == 0, out.stdout
    assert "Thread up" in out.stdout, out.stdout
    _wait_role(exe, port, ("leader",), 30)
    ds = _run(exe, port, "thread.dataset", timeout=30)
    match = HEX_RE.search(ds.stdout)
    assert ds.returncode == 0 and match, ds.stdout
    return match.group(1)


def _join(exe, port, dataset, kind="router"):
    out = _run(exe, port, f"thread.type {kind}", f"thread.dataset {dataset}", "thread.start 90", timeout=120)
    assert out.returncode == 0, out.stdout
    assert "Thread up" in out.stdout, out.stdout


@pytest.fixture
def mesh(app_exe, port_a, port_b):
    """Both modules clean before and after the test (Thread settings survive a reboot)."""
    _fresh(app_exe, port_a, port_b)
    yield
    _fresh(app_exe, port_a, port_b)


@pytest.mark.hardware
def test_thread_caps_and_usecases_fit(app_exe, port_a):
    caps = _run(app_exe, port_a, "dev.caps", timeout=30)
    assert re.search(r"^Thread:\s+yes", caps.stdout, re.M), caps.stdout
    for case in ("mesh", "building"):
        out = _run(app_exe, port_a, f"usecase.check {case}", timeout=30)
        assert out.returncode == 0, out.stdout
        assert "[gap]" not in out.stdout or "Wi-Fi" in out.stdout, out.stdout
        assert "no Thread firmware" not in out.stdout, out.stdout


@pytest.mark.hardware
def test_thread_settings_round_trip(app_exe, port_a):
    """Credentials, device type and PSKd checks on one module, without attaching."""
    _fresh(app_exe, port_a)
    try:
        out = _run(app_exe, port_a,
                   "thread.network ucxtest 15 0x1234 dead00beef00cafe 00112233445566778899aabbccddeeff",
                   "thread.network", "thread.type sleepy 3000", "thread.type", "thread.type end", "thread.type",
                   "thread.status", timeout=60)
        assert out.returncode == 0, out.stdout
        assert re.search(r"^Network:\s+ucxtest", out.stdout, re.M), out.stdout
        assert re.search(r"^Channel:\s+15", out.stdout, re.M), out.stdout
        assert re.search(r"^PAN ID:\s+0x1234", out.stdout, re.M), out.stdout
        assert re.search(r"^Ext PAN ID:\s+dead00beef00cafe", out.stdout, re.M), out.stdout
        assert re.search(r"^Network key:\s+00112233445566778899aabbccddeeff", out.stdout, re.M), out.stdout
        assert "sleepy (polls its parent every 3000 ms)" in out.stdout, out.stdout
        assert "end (never routes)" in out.stdout, out.stdout
        assert re.search(r"^Thread:\s+off", out.stdout, re.M), out.stdout
        bad = _run(app_exe, port_a, "thread.join ABC", timeout=30)
        assert bad.returncode != 0 and "PSKd is 6-32" in bad.stdout, bad.stdout
        bad = _run(app_exe, port_a, "thread.network x 27 1 0011223344556677 00112233445566778899aabbccddeeff",
                   timeout=30)
        assert bad.returncode != 0, bad.stdout
    finally:
        _fresh(app_exe, port_a)


@pytest.mark.hardware
def test_thread_form_network_makes_leader(app_exe, port_a):
    """usecase mesh 'Form': the first node makes the network and leads it."""
    _fresh(app_exe, port_a)
    try:
        _form(app_exe, port_a)
        out = _run(app_exe, port_a, "thread.leader", "thread.addr", "thread.routers", "thread.counters", timeout=60)
        assert out.returncode == 0, out.stdout
        assert re.search(r"^Leader router:\s+\d+", out.stdout, re.M), out.stdout
        assert re.search(r"^\[?fd[0-9a-f:]+\]?\s+thread", out.stdout, re.M), out.stdout  # mesh-local address
        assert re.search(r"^1 router\(s\)", out.stdout, re.M), out.stdout
    finally:
        _fresh(app_exe, port_a)


@pytest.mark.two_devices
def test_thread_scan_finds_the_other_network(app_exe, port_a, port_b, mesh):
    _form(app_exe, port_a)
    name = re.search(r"^Network:\s+(\S+)", _run(app_exe, port_a, "thread.network", timeout=30).stdout, re.M)
    assert name, "no network name on A"
    out = _run(app_exe, port_b, "thread.scan", timeout=90)
    assert out.returncode == 0, out.stdout
    assert name.group(1) in out.stdout, out.stdout


@pytest.mark.two_devices
def test_thread_join_by_dataset_becomes_router(app_exe, port_a, port_b, mesh):
    """usecase mesh 'Join' and 'Mesh view': B takes A's dataset, upgrades to router, both see each other."""
    _join(app_exe, port_b, _form(app_exe, port_a))
    _, part_b = _wait_role(app_exe, port_b, ("router",), ROUTER_UPGRADE_S)
    role_a, part_a = _status(app_exe, port_a)[:2]
    assert role_a == "leader" and part_a == part_b, f"A {role_a} {part_a}, B router {part_b}"
    for port in (port_a, port_b):
        out = _run(app_exe, port, "thread.neighbors", "thread.routers", "thread.leader", timeout=60)
        assert out.returncode == 0, out.stdout
        assert re.search(r"rloc 0x[0-9a-f]{4}\s+router\s+-?\d+ dBm", out.stdout), out.stdout
        assert re.search(r"^2 router\(s\)", out.stdout, re.M), out.stdout
        assert "linked" in out.stdout, out.stdout


@pytest.mark.two_devices
def test_thread_join_by_credentials(app_exe, port_a, port_b, mesh):
    """The same network typed in by hand (thread.network <name> <ch> <pan> <ext> <key>) instead of the dataset."""
    _form(app_exe, port_a)
    out = _run(app_exe, port_a, "thread.network", timeout=30)
    fields = {k: re.search(rf"^{k}:\s+(\S+)", out.stdout, re.M) for k in ("Network", "Channel", "PAN ID",
                                                                            "Ext PAN ID", "Network key")}
    assert all(fields.values()), out.stdout
    name, ch, pan, ext, key = (m.group(1) for m in fields.values())
    out = _run(app_exe, port_b, "thread.type end", f"thread.network {name} {ch} {pan} {ext} {key}",
               "thread.start 90", timeout=120)
    assert out.returncode == 0 and "Thread up" in out.stdout, out.stdout
    _, part_b = _wait_role(app_exe, port_b, ("child",), 30)
    assert part_b == _status(app_exe, port_a)[1], "B attached to another partition"


@pytest.mark.two_devices
def test_thread_end_device_stays_child(app_exe, port_a, port_b, mesh):
    """usecase building 'Roles': an end device attaches as a child of the router and never routes."""
    _join(app_exe, port_b, _form(app_exe, port_a), "end")
    _wait_role(app_exe, port_b, ("child",), 30)
    out = _run(app_exe, port_a, "thread.neighbors", timeout=30)
    assert re.search(r"rloc 0x[0-9a-f]{4}\s+child\s+-?\d+ dBm", out.stdout), out.stdout
    time.sleep(30)
    assert _status(app_exe, port_b)[0] == "child", "end device became a router"


@pytest.mark.two_devices
def test_thread_sleepy_end_device_polls_parent(app_exe, port_a, port_b, mesh):
    """usecase mesh 'Battery nodes': a sleepy end device attaches and keeps answering while it polls."""
    _join(app_exe, port_b, _form(app_exe, port_a), "sleepy 2000")
    _wait_role(app_exe, port_b, ("child",), 30)
    out = _run(app_exe, port_b, "thread.type", "thread.counters", timeout=30)
    assert "sleepy (polls its parent every 2000 ms)" in out.stdout, out.stdout
    assert re.search(r"rloc 0x[0-9a-f]{4}\s+child", _run(app_exe, port_a, "thread.neighbors", timeout=30).stdout)
    time.sleep(20)
    assert _status(app_exe, port_b)[0] == "child", "sleepy end device lost its parent"


@pytest.mark.two_devices
def test_thread_counters_count_and_reset(app_exe, port_a, port_b, mesh):
    """usecase building 'Health' and 'Placement': counters move with traffic, reset clears them, RSSI per neighbour."""
    _join(app_exe, port_b, _form(app_exe, port_a), "end")
    _wait_role(app_exe, port_b, ("child",), 30)
    out = _run(app_exe, port_b, "thread.counters all", timeout=30)
    values = [int(v) for v in re.findall(r"^\S.*?\s(\d+)\s*$", out.stdout, re.M)]
    assert out.returncode == 0 and any(values), out.stdout
    out = _run(app_exe, port_b, "thread.counters reset", timeout=30)
    assert "Counters reset" in out.stdout, out.stdout
    nb = _run(app_exe, port_b, "thread.neighbors", timeout=30)
    rssi = re.search(r"(-\d+) dBm\s+lq (\d)", nb.stdout)
    assert rssi and -100 < int(rssi.group(1)) < 0 and int(rssi.group(2)) > 0, nb.stdout


@pytest.mark.two_devices
def test_thread_self_healing_router_takes_over(app_exe, port_a, port_b, mesh):
    """usecase mesh 'Self-healing': the leader goes away, the remaining router leads within a few minutes."""
    _join(app_exe, port_b, _form(app_exe, port_a))
    _wait_role(app_exe, port_b, ("router",), ROUTER_UPGRADE_S)
    out = _run(app_exe, port_a, "thread.stop", timeout=30)
    assert out.returncode == 0, out.stdout
    _wait_role(app_exe, port_b, ("leader",), LEADER_TAKEOVER_S)
    # The old leader comes back into the network B now leads.
    out = _run(app_exe, port_a, "thread.start 120", timeout=150)
    assert out.returncode == 0, out.stdout
    role_a, part_a = _wait_role(app_exe, port_a, ("router", "child", "leader"), 60)
    for _ in range(int(ROUTER_UPGRADE_S / 5)):
        role_b, part_b = _status(app_exe, port_b)[:2]
        role_a, part_a = _status(app_exe, port_a)[:2]
        if part_a == part_b and {role_a, role_b} >= {"leader"}:
            break
        time.sleep(5)
    assert part_a == part_b, f"partitions did not merge: A {role_a} {part_a}, B {role_b} {part_b}"


@pytest.mark.hardware
def test_thread_scan_finds_border_router(app_exe, port_a):
    """usecase border: a home border router (Apple TV, HomePod, Nest Hub) is in range; UCX_THREAD_BR names it."""
    out = _run(app_exe, port_a, "thread.scan", timeout=90)
    assert out.returncode == 0 and re.search(r"^\d+ network\(s\)", out.stdout, re.M), out.stdout
    wanted = os.environ.get("UCX_THREAD_BR")
    if wanted:
        assert re.search(rf"^\s*\d+\s+{re.escape(wanted)}\S*\s+ch\d+", out.stdout, re.M), out.stdout


@pytest.fixture
def border_router(app_exe, port_a):
    """The active dataset of a border router (UCX_THREAD_DATASET, from ot-ctl dataset active -x); A clean after."""
    dataset = os.environ.get("UCX_THREAD_DATASET")
    if not dataset:
        pytest.skip("set UCX_THREAD_DATASET to a border router's dataset (ot-ctl dataset active -x)")
    _fresh(app_exe, port_a)
    yield dataset
    _run(app_exe, port_a, "thread.srp off", timeout=30)
    _fresh(app_exe, port_a)


@pytest.mark.hardware
def test_thread_border_router_join_and_routable_address(app_exe, port_a, border_router):
    """usecase border 'Or dataset': A joins the border router's network and gets an off-mesh routable address."""
    _join(app_exe, port_a, border_router, "end")
    _wait_role(app_exe, port_a, ("child",), 30)
    out = _run(app_exe, port_a, "thread.addr", "thread.leader", timeout=30)
    assert re.search(r"^\[?[0-9a-f:]+\]?\s+slaac", out.stdout, re.M), out.stdout  # border router's OMR prefix
    assert re.search(r"^Leader router:\s+\d+", out.stdout, re.M), out.stdout


@pytest.mark.hardware
def test_thread_ping_leader(app_exe, port_a, border_router):
    """A child pings the partition leader (the border router) over the mesh; every echo comes back."""
    _join(app_exe, port_a, border_router, "end")
    _wait_role(app_exe, port_a, ("child",), 30)
    out = _run(app_exe, port_a, "thread.ping leader 4", timeout=60)
    assert out.returncode == 0, out.stdout
    assert re.search(r":0:ff:fe00:fc00: sent 4, received 4", out.stdout), out.stdout


@pytest.mark.hardware
def test_thread_ping_off_mesh(app_exe, port_a, border_router):
    """Ping through the border router: LAN IPv6 hosts, internet names and IPv4 (NAT64). UCX_THREAD_PING lists the
    targets, comma-separated, e.g. a LAN host's IPv6, www.google.com, 8.8.8.8."""
    targets = [t.strip() for t in os.environ.get("UCX_THREAD_PING", "").split(",") if t.strip()]
    if not targets:
        pytest.skip("set UCX_THREAD_PING to off-mesh targets (LAN IPv6, host name, IPv4)")
    _join(app_exe, port_a, border_router, "end")
    _wait_role(app_exe, port_a, ("child",), 30)
    for target in targets:
        out = _run(app_exe, port_a, f"thread.ping {target} 4", timeout=60)
        assert out.returncode == 0 and re.search(r"received [1-4]\b", out.stdout), out.stdout


@pytest.mark.hardware
def test_thread_border_router_router_upgrade(app_exe, port_a, border_router):
    """A router-eligible module next to the border router upgrades from child to router."""
    _join(app_exe, port_a, border_router)
    _wait_role(app_exe, port_a, ("router",), ROUTER_UPGRADE_S)
    out = _run(app_exe, port_a, "thread.routers", timeout=30)
    assert re.search(r"^([2-9]|\d\d+) router\(s\)", out.stdout, re.M), out.stdout


@pytest.mark.hardware
def test_thread_border_router_srp_and_dns(app_exe, port_a, border_router):
    """usecase border 'Be found' and 'Find others': register over SRP, then browse and resolve it over DNS-SD.

    The names are new every run: a border router keeps a host name for its key lease (days), and a cleared dataset
    gives the module a new SRP key, so reusing a name is rightly refused as a conflict."""
    host, inst = f"ucxtest-{int(time.time()) % 100000}", f"ucxtest-{os.getpid() % 10000}"
    _join(app_exe, port_a, border_router, "end")
    _wait_role(app_exe, port_a, ("child",), 30)
    out = _run(app_exe, port_a, f"thread.srp host {host}", f"thread.srp add _ucxtest._udp 5683 {inst}",
               "thread.srp on", timeout=60)
    assert out.returncode == 0, out.stdout
    sid = re.search(r"Service (\d+) added", out.stdout)
    assert sid, out.stdout
    assert _run(app_exe, port_a, f"thread.srp txt {sid.group(1)} fw test", timeout=30).returncode == 0
    deadline, srp = time.monotonic() + 30, ""
    while time.monotonic() < deadline and not re.search(r"\s(registered|failed)\s*$", srp, re.M):
        time.sleep(3)
        srp = _run(app_exe, port_a, "thread.srp", timeout=30).stdout
    assert re.search(rf"{inst}\._ucxtest\._udp port 5683\s+registered", srp), srp
    addr = re.search(r"^\[?([0-9a-f:]+)\]?\s+slaac", _run(app_exe, port_a, "thread.addr", timeout=30).stdout, re.M)
    out = _run(app_exe, port_a, "thread.dns browse _ucxtest._udp", f"thread.dns resolve {inst} _ucxtest._udp",
               f"thread.dns host {host}", timeout=90)
    assert out.returncode == 0, out.stdout
    assert re.search(rf"^{inst}\s*$", out.stdout, re.M), out.stdout
    assert re.search(rf"^{host}\.default\.service\.arpa\.\s+port 5683", out.stdout, re.M), out.stdout
    assert addr and out.stdout.count(addr.group(1)) >= 2, out.stdout


@pytest.mark.hardware
def test_thread_srp_client_settings(app_exe, port_a):
    """usecase border 'Be found': the SRP client keeps the host name and services; registering needs a border router."""
    _fresh(app_exe, port_a)
    try:
        out = _run(app_exe, port_a, "thread.srp host ucxtest-node", "thread.srp add _sensor._udp 5683 sensor-1",
                   "thread.srp", timeout=60)
        assert out.returncode == 0, out.stdout
        assert re.search(r"^Host name:\s+ucxtest-node", out.stdout, re.M), out.stdout
        sid = re.search(r"Service (\d+) added", out.stdout)
        assert sid and re.search(r"sensor-1\._sensor\._udp port 5683", out.stdout), out.stdout
        out = _run(app_exe, port_a, f"thread.srp txt {sid.group(1)} temp 21.5", f"thread.srp remove {sid.group(1)}",
                   "thread.srp", timeout=60)
        assert out.returncode == 0, out.stdout
        assert re.search(r"^0 service\(s\)", out.stdout, re.M), out.stdout
    finally:
        _run(app_exe, port_a, "thread.srp off", timeout=30)
        _fresh(app_exe, port_a)
