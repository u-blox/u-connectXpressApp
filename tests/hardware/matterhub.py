"""The u-blox Matter hub (i.MX 93 kit, UCX_MATTER_HUB) as the commissioner in the Matter hardware tests.

Its chip-tool commissions the module's node over Bluetooth LE and hands it the hub's own Wi-Fi access point, then
drives the node's endpoints over that network. The console login and command runner come from LinuxPeer."""

import re
import time

from linuxpeer import LinuxPeer

STORE = "/tmp/ucxapp-test-ct"   # chip-tool's fabric, emptied for every test
HOSTAPD = "/run/u-blox-matter/hostapd.conf"


class MatterHub(LinuxPeer):
    def ap(self):
        """(ssid, passphrase) of the hub's Wi-Fi access point, the network it gives the node."""
        rc, out = self.run(f"grep -E '^(ssid|wpa_passphrase)=' {HOSTAPD}")
        found = dict(re.findall(r"^(ssid|wpa_passphrase)=(.*)$", out, re.M))
        if rc != 0 or "ssid" not in found:
            raise RuntimeError(f"no access point on the hub ({HOSTAPD}):\n{out}")
        return found["ssid"].strip(), found.get("wpa_passphrase", "").strip()

    def forget(self):
        """A new fabric for the next commissioning: chip-tool forgets every node it commissioned before."""
        self.run(f"rm -rf {STORE}; mkdir -p {STORE}")

    def pair_start(self, node_id, manual_code, ssid, passphrase):
        """Starts chip-tool commissioning the node over Bluetooth LE in the background; pair_wait ends it."""
        code = manual_code.replace("-", "")
        self.run(f"rm -f {STORE}/pair.log; (chip-tool pairing code-wifi {node_id} '{ssid}' '{passphrase}' {code} "
                 f"--storage-directory {STORE} > {STORE}/pair.log 2>&1; echo __PAIR_RC=$?__ >> {STORE}/pair.log) "
                 f"> /dev/null 2>&1 & true")   # run() appends '; echo ...', and '& ;' is a syntax error

    def pair_wait(self, timeout=180):
        """(commissioned, chip-tool's last lines) once the commissioning started by pair_start ends."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            rc, out = self.run(f"grep -c __PAIR_RC= {STORE}/pair.log")
            if rc == 0 and out.strip().endswith("1"):
                break
            time.sleep(2)
        _, tail = self.run(f"grep -E -i 'commissioning (stage|complete)|completed with|error|fail|timeout|__PAIR_RC' "
                           f"{STORE}/pair.log | tail -30")
        return "Device commissioning completed with success" in tail and "__PAIR_RC=0__" in tail, tail

    def onoff(self, node_id, command, endpoint=1, timeout=60):
        """onoff on/off/toggle on the node's light; True when the node answered with success."""
        rc, out = self.run(f"chip-tool onoff {command} {node_id} {endpoint} --storage-directory {STORE} 2>&1 "
                           f"| grep -E -i 'status|error|timeout' | tail -5", timeout=timeout)
        return rc == 0 and re.search(r"status\s*=\s*0x0+\b|SUCCESS", out, re.I) is not None, out

    def read_onoff(self, node_id, endpoint=1, timeout=60):
        """The light's OnOff attribute as chip-tool reads it over the network: True, False or None."""
        _, out = self.run(f"chip-tool onoff read on-off {node_id} {endpoint} --storage-directory {STORE} 2>&1 "
                          f"| grep -o -E 'OnOff: (TRUE|FALSE)' | tail -1", timeout=timeout)
        return {"OnOff: TRUE": True, "OnOff: FALSE": False}.get(out.strip().splitlines()[-1] if out.strip() else "")
