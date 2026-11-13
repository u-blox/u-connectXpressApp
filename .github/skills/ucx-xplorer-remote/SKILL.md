---
name: ucx-xplorer-remote
description: "Keep ucxapp in step with the u-connectXplorer phone app's Remote Control protocol (JSON-RPC 2.0 over WebSocket, port 8765, docs/REMOTE_CONTROL.md). Use when: asked about phone remote control, xplorer.*, perf.phone, perf.latency/perf.throughput ... xplorer, the phone as echo peer or BLE central, UCX_XPLORER tests, or to check whether REMOTE_CONTROL.md changed and what in ucxapp must follow."
---

# u-connectXplorer Remote Control

The phone app u-connectXplorer (https://github.com/u-blox/u-connectXplorer) runs a JSON-RPC 2.0 WebSocket server
(`ws://<phone-ip>:8765`). ucxapp is a client of it: `xplorer.*` drives the phone, and `perf.*` uses the phone as
the other end of a module test. The contract is
[docs/REMOTE_CONTROL.md](https://github.com/u-blox/u-connectXplorer/blob/main/docs/REMOTE_CONTROL.md).

## Pinned version

ucxapp matches REMOTE_CONTROL.md at commit **`e2809a9`** (2026-11-13, blob `c9073ebc`, **protocol version 2**;
`UCXAPP_XPLORER_PROTOCOL` in [ucxapp_xplorer.h](../../../app/src/ucxapp_xplorer.h)). Bump this pin after every
review, whether or not code had to change.

The doc now says how it changes: `protocolVersion` (in `app.version` and `app.getCapabilities`) is raised only for
changes that break a client, and its "Protocol history" table lists them; everything added comes as a feature flag.
So first look at that table and the flag table, then at the patch.

Check for changes:

```powershell
gh api "repos/u-blox/u-connectXplorer/commits?path=docs/REMOTE_CONTROL.md&per_page=5" --jq '.[] | .sha[0:7] + " " + .commit.committer.date + " " + (.commit.message | split("\n")[0])'
gh api "repos/u-blox/u-connectXplorer/compare/e2809a9...main" --jq '.files[] | select(.filename=="docs/REMOTE_CONTROL.md") | .patch'
```

No output from the second command means nothing changed.

## What ucxapp depends on

Client: [ucxapp_xplorer.c](../../../app/src/ucxapp_xplorer.c) (WebSocket, JSON-RPC, events, saved phone).
Commands: [ucxapp_cmds_xplorer.c](../../../app/src/ucxapp_cmds_xplorer.c) and
[ucxapp_cmds_perf.c](../../../app/src/ucxapp_cmds_perf.c).

| Method / event | Used by | Fields read |
|---|---|---|
| `app.getCapabilities` | every connect (before authenticating), feature checks | `protocolVersion` (a note when above 2), `features.authChallenge`, `features.socketEcho`, `features.perfSps`, `features.stopOnDisconnect`, `features.waitForDevice` |
| `app.getChallenge` | every connect, when the phone has `authChallenge` | `nonce` |
| `app.authenticate {response}` or `{token}` | `xplorer.connect`, every reconnect | result only; errors `-32000` (no auth) / `-32001` (bad token) / `-32002` (locked out). `response` is the hex HMAC-SHA256 of the nonce under the token; the token is sent only to a phone without `authChallenge` |
| `app.configure {stopOnDisconnect:true}` | `perf.phone`, when the phone has `stopOnDisconnect` | result only |
| `app.version` | `xplorer.connect`, `xplorer.info` | `fullVersion`, else `version` |
| `app.getPlatform` | `xplorer.connect`, `xplorer.info`, `perf.phone` | `platform`, `osVersion`, `bluetoothState` |
| `socket.startServer {protocol,port,echo:true,force:true}` | `xplorer.echo`, `perf.latency/throughput ... xplorer` | result |
| `socket.getState` | `xplorer.echo` state | `active`, `protocol`, `state`, `port`, `echo`, `bytesReceived`, `bytesSent`, `packetsReceived`, `packetsSent`, `patternBreaks` |
| `socket.stop` | after an echo run | none |
| `ble.waitForDevice {name,timeoutMs}` | `perf.phone` (find the module), when the phone has `waitForDevice` | `id` |
| `ble.startScan` / `ble.getDevices` / `ble.stopScan` | `perf.phone` on a phone without `waitForDevice` | `devices[].id`, `devices[].name` |
| `ble.connect {deviceId}` / `ble.disconnect {deviceId}` | `perf.phone` | `mtu` |
| `sps.connect {deviceId,force:true}` / `sps.disconnect` | `perf.phone` | result |
| `perf.startThroughput {deviceId,mode,durationMs,packetSize,sps:true}` | `perf.phone` | `packetSize` |
| `perf.onComplete` (event), `perf.stop` | `perf.phone` | `txBytes`, `rxBytes`, `throughputKbps`, `patternBreaks`, `rttMs.*`, `endReason` (shown unless `completed` or `stopped`) |
| any method | `xplorer.call <method> [json]` | prints the result |

Fixed assumptions:
- Port 8765 (`UCXAPP_XPLORER_PORT`), plain `ws://`. The phone can serve `wss://` instead (feature `wss`);
  ucxapp has no TLS for this link yet, so encryption must be off on the phone. Doing it means TLS on the host
  socket in `ucxapp_xplorer.c` and pinning the fingerprint (`certFingerprint`) saved by `xplorer.connect`.
- The token is at most 64 characters and persists on the phone. ucxapp saves it sealed in the secure store.
- Events carry no `id`.
- The data pattern is the s-center 0x00–0xFF rolling counter.
- The phone never runs AT commands, so the module stays on ucxapp's UART.
- The echo peer is started and read back over two connections, so it must outlive a connection: only
  `perf.phone` asks for `stopOnDisconnect`.
- Error codes are printed, never matched: the new `-32010`…`-32014` need nothing from ucxapp.

A phone from before protocol version 2 (no `protocolVersion`, none of the new flags) keeps working: every new
feature is used only when its flag is there. The fake phone of the tests is that old phone by default;
`FakePhone(features=V2, protocol=2)` in `tests/harness/xplorer.py` is the new one.

## When the doc changed

1. Read the patch. Look for:
   - renamed or removed methods, fields or feature flags from the table above;
   - a changed port, auth flow or error code;
   - a changed data pattern or event name.
2. Fix the code first (the commands and the client above). Keep older phone versions working where it is cheap:
   check `ucxapp_xplorer_has("<flag>")` and print an "update u-connectXplorer" note instead of failing.
3. Update the `xplorer.*`/`perf.*` usage strings, `kExamples` in
   [ucxapp_cmds_examples.c](../../../app/src/ucxapp_cmds_examples.c) and the u-connectXplorer section of
   [COMMANDS.md](../../../COMMANDS.md).
4. Run the phone tests against a real phone:
   `$env:UCX_XPLORER='ws://<phone-ip>:8765'; $env:UCX_XPLORER_TOKEN='<token>'; .\ucxapp --detect --test tests/hardware/test_hw_xplorer.py`.
   They skip without these variables.
5. Update the table and the pinned commit in this file.

New methods are worth using only if they test a u-connectXpress capability from the phone side. Examples:
`peripheral.*` with the module as central, or `iperf.*` against the module's Wi-Fi. See the scope rules in
`.github/copilot-instructions.md`.
