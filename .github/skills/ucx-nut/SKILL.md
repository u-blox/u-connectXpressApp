---
name: ucx-nut
description: Reproduce a u-connectXpress module bug (NORA-W36, NORA-B26 hang, wrong AT response, missing URC) with the NUT test framework so firmware R&D can rerun it. Use when a ucxapp test finds a module or firmware problem, when asked how R&D can reproduce an issue, or when asked about NUT/NUTS, invoke tasks or NoraB26/NoraW36 test selections.
---

# Reproduce module issues with NUT

NUT (NUnit Test System, `E:\u-blox\nut`, .NET 8) is the test framework firmware R&D runs every night. **Hand module and firmware bugs over as a NUT reproduction, not as a ucxapp test.** R&D then reruns the bug with their own setup and log tooling, and the test can join the product's nightly list.

Use ucxapp (`tests/hardware`) to find the problem. Then reproduce it in NUT before you report it.

## Workflow
1. **Isolate in ucxapp first.** Get the shortest AT sequence that triggers the problem with `.\ucxapp <port> --exec ...` and the AT trace. Use `tests/hardware/test_gatt_isolate.py` for two-module GATT cases. Rule out host problems (port missing in `.\ucxapp --detect`, a leftover process, USB) before you blame the firmware.
2. **Look for an existing NUT test.** Search `E:\u-blox\nut\uConnectTest` for the AT command or the step, for example `grep UnbondAll`. Then check whether the product's playlist runs that test (`testlists/norab26.playlist`, `testlists/noraw36.playlist`). If an existing test is missing from the playlist, that also explains why the nightly run never caught the bug. Say so in the report.
3. **Run that test** against the module (see below). If it does not reproduce the problem, write a small new NUT test (step 4).
4. **Add a new test** next to the closest existing one in `uConnectTest/` (`uConnect2/`, `uConnectCommon/`, `uConnect/Func/...`). Use the DUT/tester helpers (`dut.At.*`, `dut.Bt.*`, `GetTesters(n)`), give it a `Description`, and keep it to the minimal sequence from step 1. Before you commit C#, run `inv csharpier-format`. Follow NUT's own `.github/copilot-instructions.md` and `AGENTS.md`.
5. **Report**: the NUT test name, the `inv` command, the firmware version (`dev.info`), the pass/fail rate over N runs, and the AT trace plus the log port output.

## Setup (once)
```powershell
cd E:\u-blox\nut
Copy-Item config\user-config.yaml.example config\user-config.yaml
# Edit DUT: ComPort (AT) and LogPort per module; get the ports from .\ucxapp --detect in u-connectXpressApp
inv generate-config        # writes uConnectTest/Config.json
inv build
```
- `inv generate-config --update-config` can create `user-config.yaml` from `local_devices.yaml`.
- Close ucxapp and every other serial tool before a run, because NUT needs exclusive access to the ports.
- Tests that use testers (`GetTesters(n)`) need extra modules in `DUT`. A NORA-W36 can serve as the tester for a NORA-B26.

## Run
```powershell
# One class or test (NUnit Test Selection Language)
inv run-test-selection --product NoraB26 --test-selection "class = NUnitTest.Func.Bt.UnBond"
inv run-test-selection --product NoraW36 --test-selection "test =~ UnBondAllLowEnergy"
# Interactive picker
inv run-tests-menu --product NoraB26
# Nightly group or list without running
inv run-test-group --product NoraB26 --test-group Main
inv run-test-list --product NoraB26 --testlistfile tests.txt --dry-run
```
The legacy MSBuild runner (`MSBuild.exe Testing.proj -t:RunTests -p:Configuration=NoraB26 -p:Flags="..." [-p:Explore=1]`) is still used by CI. Prefer `inv`.

## Known reproductions
- **NORA-B26 goes silent after unbond all** (`docs/HID_FINDINGS.md`, issue 5): `uConnectTest/uConnect/Func/Bt/UnBond.cs` → `UnBondAllLowEnergy(1..5)` and `UnBondAllAndImmediateFactoryResetLowEnergy`. These tests are not in `testlists/norab26.playlist`.

## Rules
- Do not write one-off probe scripts in either repo. If NUT lacks a helper, add it to the NUT framework.
- Look up AT commands with the `u-blox-docs` MCP server before writing the test.
