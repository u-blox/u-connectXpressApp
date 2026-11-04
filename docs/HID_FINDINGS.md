# HID findings (open)

Status of the HID tests and open issues. Keep this file until every item is fixed, then delete it.

## Test setup

The HID tests in `tests/hardware/test_hid.py` use a Linux board as the BLE peer instead of the PC, because Windows reconnects to bonded devices by itself and takes the link over.

- Peer: i.MX91 FRDM ("u-blox-hub-acc2") with BlueZ 5.84, serial console as root without a password. The kernel has no `CONFIG_UHID`, so the peer works at GATT level over D-Bus (`tests/hardware/peer/ucxhid_peer.py`, uploaded by `tests/hardware/linuxpeer.py`).
- The board boots the u-blox hub image from the SD card (U-Boot env: `mmcdev=1`, `fdtfile=imx91-11x11-frdm-iw610.dtb`, `mmcroot=/dev/mmcblk1p2 rootwait rw`). The eMMC image has no Bluetooth.
- Each test restarts bluetoothd on the peer, so GATT applications from earlier runs are gone.

```powershell
$env:UCX_HID_PEER="COM58"; $env:UCX_PORT_A="COM13"; $env:UCX_PORT_B=""
py -m pytest tests\hardware\test_hid.py
```

## Results (NORA-W36 3.6.0-012)

| Test | NORA-W36 3.6.0-012 | NORA-B26 3.5.0-156 |
|---|---|---|
| `test_hid_device_reports_reach_a_linux_host` | pass | pass |
| `test_hid_client_reads_a_linux_keyboard[report]` | pass | hangs (see issue 5) |
| `test_hid_client_reads_a_linux_keyboard[boot]` | pass | not run |
| `test_hid_device_bonded_host_reconnects_after_reboot` | **fail** (older run) | pass |

The W36 results are from earlier runs; only `[report]` was rerun on the W36 on 2026-11-04. `test_gatt_isolate.py` passes on all four cases with the B26 as server and the W36 as client.

## Open issues

1. **Bonded host reconnect after a module reboot fails.** BlueZ `Connect` returns `le-connection-abort-by-local`; btmon earlier showed "ACL Connection Already Exists" because the BlueZ auto-connect for bonded devices raced the explicit connect. This is the same pattern as Windows taking the link. Probably test logic (let the auto-connect make the link), not yet confirmed.
2. **Notifications not reported (possible firmware issue).** With a large GATT database on the peer (report handles 0x014C/0x0182), btmon showed the keyboard notifications sent over the air, but the module gave no `+UEBTGCN` URC. It went away after a bluetoothd restart on the peer. Needs a minimal repro between two NORA-W36 modules with notifications on high handles.
3. **`AT+UBTGCCW=<conn>,<handle>,0` returns `ERROR:114`** after a subscription on a bonded, encrypted link (every `hid.client off`). Check against the AT manual and the security level.
5. **NORA-B26 goes silent during the HID client test.** The module stopped answering AT commands in the first step of the test (after `bt.unbond all`), so the run aborts with "does not answer AT commands". After a power-cycle the module answers again, but a later run silences it again. Replaying the same steps by hand with raw serial probes did not reproduce it, and the port briefly disappeared from the OS (COM24 not found). Suspect the USB/UART connection or a leftover state from the previous process before firmware. Check the port with `.\ucxapp --detect`, then compare the AT trace with the W36.
4. **Two-module GATT tests fail** (`tests/hardware/test_ble.py`, NORA-W36 on two ports): the client cannot find the server name `ucxgatt` in the scan. Check that the second module advertises before suspecting the firmware.

## Reproduce without HID

`tests/hardware/test_gatt_isolate.py` needs only two modules, no HID host and no Linux peer. Module A serves Battery, either alone (low handles) or behind eight other services (high handles). Module B connects by address, optionally bonds, subscribes, gets three notifications and unsubscribes. That covers issues 2 and 3, and connecting by address skips the name scan in issue 4.

```powershell
$env:UCX_PORT_A="COM13"; $env:UCX_PORT_B="COM155"
py -m pytest tests\hardware\test_gatt_isolate.py
```

The four cases are low/high handles × plain/bonded. If only `high-bonded` fails, the firmware is the likely suspect. The test has not run on hardware yet.

## Next steps

- Run `test_gatt_isolate.py` on two NORA-W36 modules, then a NORA-W36 with a NORA-B26.
- Run `test_hid.py` on the NORA-B26.
- Fix the reconnect test.
- Add two-module tests for `bt.security` levels, `bt.bond` and notifications on high handles.
