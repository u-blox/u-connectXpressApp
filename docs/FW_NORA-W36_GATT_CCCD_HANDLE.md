# NORA-W36: GATT client drops notifications when the CCCD is not at value handle + 1

| | |
|---|---|
| Module | NORA-W36 |
| Firmware | 3.6.0-012 |
| Area | Bluetooth LE, GATT client (`AT+UBTGCCW`, `+UEBTGCN` / `+UEBTGCI`) |
| Severity | High: notifications from spec-compliant servers are silently lost, no error anywhere |
| Found with | u-connectXpressApp `gatt.client.sps` against an iPhone (iOS 27.0.1) running u-connectXplorer as an SPS server |

## Summary

`AT+UBTGCCW=<conn>,<desc_handle>,1` writes the CCCD on the peer and returns `OK`, and the peer starts sending
notifications. But the module only forwards notifications (`+UEBTGCN`) for value handle `desc_handle - 1`.
When the CCCD is not right after the value, the notifications are dropped.

The Bluetooth Core spec doesn't fix where the CCCD sits among a characteristic's descriptors. iOS
(CoreBluetooth) puts a Characteristic Extended Properties descriptor (0x2900) before the CCCD on every
characteristic with property 0x80, so this affects any iOS peripheral with such a characteristic. Other stacks
that put descriptors before the CCCD are hit the same way.

## Peer GATT layout (as discovered by the module)

u-blox SPS service `2456E1B926E28F83E744F34F01E9D701` served by iOS:

```
+UBTGSCD:0,111,9C,112,2456E1B926E28F83E744F34F01E9D703     FIFO, value handle 112, properties 0x9C
+UBTGCDD:0,111,113,2900                                     Extended Properties
+UBTGCDD:0,111,114,2902                                     CCCD
+UBTGSCD:0,115,9C,116,2456E1B926E28F83E744F34F01E9D704     Credits, value handle 116
+UBTGCDD:0,115,117,2900
+UBTGCDD:0,115,118,2902
```

## Reproduction

1. Connect as central to a peer with the layout above (value `V`, other descriptor `V+1`, CCCD `V+2`).
2. Discover the service, its characteristics and the descriptors, then enable notifications on the real CCCD:

   ```
   AT+UBTGCDD=0,112,114
   +UBTGCDD:0,111,113,2900
   +UBTGCDD:0,111,114,2902
   OK
   AT+UBTGCCW=0,114,1
   OK
   ```

3. The peer confirms the subscription (CoreBluetooth `didSubscribeTo` fires for the FIFO characteristic).
4. Let the peer notify on handle 112 (16 bytes `hello-from-phone`). The peer reports the notification as sent.

**Expected:** `+UEBTGCN:0,112,68656C6C6F2D66726F6D2D70686F6E65`

**Actual:** no URC. Nothing on the AT interface, and no error.

### Proof that the value handle is taken as `desc_handle - 1`

Same connection, after step 2:

```
AT+UBTGCCW=0,113,1
ERROR:114                     (expected: 0x2900 is read-only on the peer)
```

The next notification from the peer on handle 112 is then delivered:

```
+UEBTGCN:0,112,68656C6C6F2D66726F6D2D70686F6E65
```

So:

- The module enables forwarding for handle `113 - 1 = 112` even though the peer rejected the write.
- The write to the real CCCD (114) enabled forwarding for 113, which never notifies.

## Observations

| Peer characteristic | CCCD at | Notifications reach the host |
|---|---|---|
| NUS TX on iOS (properties 0x10, no 0x2900) | value + 1 | yes |
| SPS FIFO / Credits on iOS (properties 0x9C, 0x2900 first) | value + 2 | **no** |
| SPS FIFO after the extra `AT+UBTGCCW` to value + 1 | value + 2 | yes (side effect of the failed write) |

The phone→module direction of SPS is fully broken for an iOS peer: neither data on FIFO nor credits on Credits
arrive. The other direction (module writes to the peer) works.

The module's own SPS client (`AT+USPSC`) against the same iOS layout also never gets data or credits. The NUTS
test `SpsClientWithIosLayout` confirms this on NORA-W36 with a Bumble peer.

### Hardware results (NUTS + Bumble peer)

Run with the `GattClientCccdLayout` tests against a Bumble peer on a Realtek UB500 USB HCI dongle (Linux host).

| Case | NORA-W36 | NORA-B26 |
|---|---|---|
| CCCD at value + 1 | delivered | delivered |
| CCCD at value + 2 (0x2900 first, iOS layout) | **dropped** | **dropped** |
| CCCD at value + 3 | **dropped** | **dropped** |
| Indications with CCCD at value + 2 / + 3 | **dropped** | **dropped** |
| `AT+USPSC` against SPS with CCCD at value + 2 | **no credits, no data** | not run |
| Rejected write to 0x2900 enables forwarding | **yes** (bug) | no |
| CCCD disable at value + 1 stops delivery | **no**, keeps forwarding | yes |

The value-handle mapping bug is common to both products, so it is most likely in shared GATT client code.
The NORA-W36 additionally keeps a local subscription after a rejected write and after a disable.

### Related findings from the Bumble interop suite

Not part of the CCCD bug, but found in the same runs and worth tracking:

- NORA-B26 as peripheral: when a bonded peer reconnects, `+UEBTC` is withheld until the link is encrypted.
  A peer that stays unencrypted is connected but never reported to the host (observed for 11 s, across a
  connection parameter update). Phones encrypt straight away, so this is mostly hidden.
- The module as central ignores a PHY change requested by the peer and keeps its own preference (allowed by
  the spec). Right after connecting it sets up MTU 247, data length 251 and 2M PHY by itself;   a PHY request sent during that returns `ERROR`.
- As central the module re-encrypts with the stored bond automatically on reconnect (NORA-B26).

NORA-W36 (3.6.0-012) differs from NORA-B26 in these interop runs:

- **Bonded reconnect as central fails:** `AT+UBTC` to a bonded peer connects, then the link drops with
  reason 0x3D (MIC failure) and `+UEBTB:<addr>,2`. The same peer and bond reconnect fine with NORA-B26.
- **Bonded reconnect as peripheral is never reported:** the peer reconnects and encrypts successfully with
  the bond, but `+UEBTC` never comes, not even after encryption (NORA-B26 reports it once encrypted).
  Affects `BondIsReusedAfterReconnect`, `RpaPeerReconnectsWithBond` and `PeerForgetsBondAndPairsAgain`.
- **No connection to an extended advertiser:** `AT+UBTC` to a peer using connectable extended advertising
  times out after 10 s with `+UEBTDC`.
- As central the module stays on 1M PHY after connecting (NORA-B26 switches to 2M by itself).
- Connection property changes (supervision timeout, data length) show up in `AT+UBTCST` about 200 ms after the
  controller applied them. Not a bug, but hosts reading right after an update see old values.

The full NUTS Bumble interop suite (`BumbleInterop`) passes on NORA-B26: 65 passed, 0 failed, 2 skipped (one
dongle limitation, one PHY-preference case). On NORA-W36 the only failures are the six tests above.

## Requested fix

1. When enabling notifications or indications, map the CCCD handle to the characteristic that owns it (the
   highest characteristic value handle below `desc_handle` in the discovered table, or keep a handle-range table
   from discovery). Don't assume `desc_handle - 1`.
   - If the module has no discovery data for the connection, forwarding every `Handle Value Notification` and
     `Indication` with its own handle (as the URC already carries it) avoids the mapping altogether.
2. Don't register a local subscription when the peer rejects the CCCD write (`ERROR:114` above still enabled
   forwarding).
3. Check `AT+USPSC` against a peer whose CCCD is not at value + 1.

## Host side

u-connectXpressApp already looks up the real CCCD with `AT+UBTGCDD` (`findCccd` in
`app/src/ucxapp_cmds_gatt.c`). `setCccd` prints a warning when the CCCD is not at value + 1, so the failure is
visible instead of silent. ucxapp has no workaround, on purpose: the extra write
relies on a failed request having a side effect.

## Regression tests in NUTS

NUTS (branch `cmag_nut_bumble`) reproduces this without a phone: a Bumble BLE peer on a USB HCI dongle
(`uConnectTest/BumblePeer`) serves the iOS layouts.

| Test | Covers |
|---|---|
| `GattClientCccdLayout.CccdWriteEnablesDelivery` | CCCD at value + 1, + 2 (0x2900 first, iOS) and + 3 (requested fix 1) |
| `GattClientCccdLayout.RejectedCccdWriteChangesNothing` | Write to 0x2900 is rejected and enables nothing (requested fix 2) |
| `GattClientCccdLayout.SpsClientWithIosLayout` | `AT+USPSC` against SPS with 0x2900 before the CCCDs (requested fix 3) |
| `GattClientCccdLayout.CccdDisableStopsDelivery` | Writing 0 to the real CCCD stops the URCs |
| `GattClientCccdLayout.TwoSubscriptionsKeepTheirHandles` | Two characteristics with non-+1 CCCDs report their own value handles |

```
inv run-test-selection --product NoraW36 --test-selection "class =~ GattClientCccdLayout"
```

Hardware test that shows it: `tests/hardware/test_hw_xplorer.py::test_module_connects_to_the_phone_as_a_peripheral[sps]`
(`UCX_XPLORER`, `UCX_XPLORER_TOKEN`, `UCX_PORT_A`).
