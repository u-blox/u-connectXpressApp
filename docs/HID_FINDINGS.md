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

## Other changes since the last update

- `fw.update eval` flashes the newest EVAL (NORA-W36 3.6.0-019); default `fw.update` skips EVAL builds and compares against the stable 3.5.1. Added `fw.list` (local downloads) and `fw.github.list` (remote list). Committed in `c8c281b`. The default-skip behaviour is not yet confirmed by the user.
- WPS: the virtual phone sends SET_CONFIG as text and again as protobuf, which joined the Wi-Fi twice. `wpsAlreadyJoined()` in `app/src/ucxapp_cmds_gatt.c` now prevents the second join. The `usecase.tool` examples run `wifi.roaming on`. Committed in `cca7bfd`.
- CI: pytest failed on "import file mismatch" because `tests/test_xplorer.py` and `tests/hardware/test_xplorer.py` share a basename. Renamed the hardware file to `test_hw_xplorer.py` (`da87c58`) and added a basename check in `tests/test_repo.py` (`6c77f84`).

## Open issues

1. **Bonded host reconnect after a module reboot fails.** BlueZ `Connect` returns `le-connection-abort-by-local`; btmon earlier showed "ACL Connection Already Exists" because the BlueZ auto-connect for bonded devices raced the explicit connect. This is the same pattern as Windows taking the link. Probably test logic (let the auto-connect make the link), not yet confirmed.
2. **Notifications not reported (possible firmware issue).** With a large GATT database on the peer (report handles 0x014C/0x0182), btmon showed the keyboard notifications sent over the air, but the module gave no `+UEBTGCN` URC. It went away after a bluetoothd restart on the peer. Needs a minimal repro between two NORA-W36 modules with notifications on high handles.
3. **`AT+UBTGCCW=<conn>,<handle>,0` returns `ERROR:114`** after a subscription on a bonded, encrypted link (every `hid.client off`). Check against the AT manual and the security level.
5. **NORA-B26 goes silent during the HID client test (most serious).** The module stopped answering AT commands in the first step of the test (after `bt.unbond all`), so the run aborts with "does not answer AT commands". After a power-cycle the module answers again, but a later run silences it again. Replaying the same steps by hand with raw serial probes did not reproduce it, and the port briefly disappeared from the OS (COM24 not found). Suspect the USB/UART connection or a leftover state from the previous process before firmware. Check the port with `.\ucxapp --detect`, then compare the AT trace with the W36. The failing sequence is a reboot (`AT+CPWROFF`), then `AT+UBTUB` and `AT+UBTBDL`.
   - **Trace (2026-11-04):** in the failing process the module never answered the first `ATE0`, so `AT+UBTUB` was never sent. The silence started during or after the preceding `dev.reboot` step (`AT+CPWROFF` → `+STARTUP`). That step's output was discarded at the time; `_reboot` in `tests/hardware/hwutil.py` now keeps it, and the "does not answer AT commands" stop prints it.
   - **u-connectClient or firmware:** a raw serial `AT` from a separate tool (not u-connectClient) also got no reply. The silence survived new processes and only a power-cycle cleared it, so the host library holds no state that could explain it. Left: firmware hanging in the reboot/boot, or the EVK (the USB-UART port disappeared from Windows once). The B26 log port output from the moment of the hang decides between them. Host detail to rule out: ucxapp opens the port with DTR and RTS asserted and pulses DTR (WAKE_UP) after a failed `ATE0`; check the EVK-NORA-B26 wiring of DTR/RTS.
   - **Not reproduced without HID (2026-11-05):** `tests/hardware/test_reboot_unbond.py` repeats the cleanup the same way (reboot in one process, `bt.unbond all` + `bt.bonds` in a new one, port closed and reopened in between), each module as DUT, plain and bonded to the other module. With `UCX_UNBOND_CYCLES=20`, the NORA-B26 3.5.0-156 answered in all 40 cycles (20 plain, 20 bonded to a NORA-W36 3.6.0-012); the W36 as DUT passed too. One W36-DUT bonded run failed once in cycle 7 because the B26 peer was not connectable (no `+UEBTC` within 5.5 s, the B26 still answering); a rerun passed 20/20. What the HID run had and this test lacks: the BlueZ peer, `hid.setup`/HID client GATT state and encrypted links to a Linux host. Next: rerun `test_hid.py -k client` on the B26 when the i.MX91 is back, with the log port connected.
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
