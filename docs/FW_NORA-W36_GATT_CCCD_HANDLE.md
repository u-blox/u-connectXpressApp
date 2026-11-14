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

The module's own SPS client (`AT+USPSC`) against the same iOS peer also never got data. It wasn't traced at the
GATT level, so the cause there is not confirmed but probably the same.

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

Hardware test that shows it: `tests/hardware/test_hw_xplorer.py::test_module_connects_to_the_phone_as_a_peripheral[sps]`
(`UCX_XPLORER`, `UCX_XPLORER_TOKEN`, `UCX_PORT_A`).
