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

ucxapp matches REMOTE_CONTROL.md at commit **`8a61b6d`** (2026-11-02, blob `0f200377`). Bump this pin after
every review, whether or not code had to change.

Check for changes:

```powershell
gh api "repos/u-blox/u-connectXplorer/commits?path=docs/REMOTE_CONTROL.md&per_page=5" --jq '.[] | .sha[0:7] + " " + .commit.committer.date + " " + (.commit.message | split("\n")[0])'
gh api "repos/u-blox/u-connectXplorer/compare/8a61b6d...main" --jq '.files[] | select(.filename=="docs/REMOTE_CONTROL.md") | .patch'
```

No output from the second command means nothing changed.

## What ucxapp depends on

Client: [ucxapp_xplorer.c](../../../app/src/ucxapp_xplorer.c) (WebSocket, JSON-RPC, events, saved phone).
Commands: [ucxapp_cmds_xplorer.c](../../../app/src/ucxapp_cmds_xplorer.c) and
[ucxapp_cmds_perf.c](../../../app/src/ucxapp_cmds_perf.c).

| Method / event | Used by | Fields read |
|---|---|---|
| `app.authenticate {token}` | `xplorer.connect`, every reconnect | result only; errors `-32000` (no auth) / `-32001` (bad token) |
| `app.version` | `xplorer.connect`, `xplorer.info` | `fullVersion` (undocumented), else `version` |
| `app.getPlatform` | `xplorer.connect`, `xplorer.info`, `perf.phone` | `platform`, `osVersion`, `bluetoothState` |
| `app.getCapabilities` | feature checks | `features.socketEcho`, `features.perfSps` |
| `socket.startServer {protocol,port,echo:true,force:true}` | `xplorer.echo`, `perf.latency/throughput ... xplorer` | result |
| `socket.getState` | `xplorer.echo` state | `active`, `protocol`, `state`, `port`, `echo`, `bytesReceived`, `bytesSent`, `packetsReceived`, `packetsSent`, `patternBreaks` |
| `socket.stop` | after an echo run | none |
| `ble.startScan` / `ble.getDevices` / `ble.stopScan` | `perf.phone` (find the module) | `devices[].id`, `devices[].name` |
| `ble.connect {deviceId}` / `ble.disconnect {deviceId}` | `perf.phone` | `mtu` |
| `sps.connect {deviceId,force:true}` / `sps.disconnect` | `perf.phone` | result |
| `perf.startThroughput {deviceId,mode,durationMs,packetSize,sps:true}` | `perf.phone` | `packetSize` |
| `perf.onComplete` (event), `perf.stop` | `perf.phone` | `txBytes`, `rxBytes`, `throughputKbps`, `patternBreaks` |
| any method | `xplorer.call <method> [json]` | prints the result |

Fixed assumptions:
- Port 8765 (`UCXAPP_XPLORER_PORT`).
- The token is at most 64 characters and persists on the phone. ucxapp saves it sealed in the secure store.
- Events carry no `id`.
- The data pattern is the s-center 0x00–0xFF rolling counter.
- The phone never runs AT commands, so the module stays on ucxapp's UART.

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
