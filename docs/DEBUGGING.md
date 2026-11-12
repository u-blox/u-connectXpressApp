# Debugging u-connectXpressApp

Recipes that found real bugs. Start with the cheapest: the AT trace, then the virtual module, then a debugger.

## What the module said: AT trace

```powershell
.\ucxapp COM155 --exec "at.trace build/run.trace" --exec "matter.start" --exec "matter.stop"
```

`--exec` takes one command per flag (`--exec "a" --exec "b"`), not `;`-separated. The trace is written as the app
runs, but a crash loses what was not flushed: when the app crashes, reproduce with the debugger instead.

- One AT command by hand: `--exec "at ATI0"`. `ATI0` is the type code, `ATI9` the firmware version.
- The module escapes bytes it cannot print (`"\xFF\xFF…"`), u-connectClient unescapes them, and the AT client
  drops raw bytes that fail `isprint`. An engineering sample with an erased type code gives 16 × 0xFF:
  `matter.status` shows such an answer as hex (`hardware 0xFFFF…`).
- `matter.log detail` prints the Matter SDK's own log.

## Reproduce without hardware: the virtual module

`tests/harness/vucx` plays a module over TCP (`.\ucxapp --virtual` or a test). A bug seen on hardware gets a profile
setting there and a test in `tests/`, for example `gatt_services: N` (a GATT server with room for N services, as
NORA-W36 cannot remove one) or an `ATI0` answer of `'"\\xFF\\xFF"'`. Answers are sent as the module sends them:
escaped text, not raw bytes.

## A crash on Windows

Exit code `0xC0000005` is an access violation, `0xC0000409` a fail-fast (`abort`, a destructor that finds a
thread still running, a broken stack). Run the Debug build under gdb from MSYS2 (`C:\msys64\ucrt64\bin`, the
same MinGW that builds the Matter plug-in):

```powershell
.\ucxapp --debug --build                                    # app/bin/ucxapp-debug.exe and its PDB
C:\msys64\ucrt64\bin\gdb.exe -batch -ex run -ex bt -ex "info files" --args app\bin\ucxapp-debug.exe COM155 --exec "matter.start"
```

gdb names the frames of `ucxmatter.dll` (MinGW, DWARF) but not those of the app (MSVC, PDB): those are bare
addresses. `info files` shows where the exe was loaded (its `.text` starts 0x1000 above the base). Then:

```powershell
.\ucxapp --symbolize 0x7ff6a1b31fe0 0x7ff6a1b3c412 --base 0x7ff6a1a00000
0x7ff6a1b31fe0  parseLine+0x40  u_cx_at_client.c:132
```

Without `--base` the exe is assumed at 0x140000000; an address below the base is an offset into the exe, so the
fault offset of an `ucxapp-debug.exe` crash in the Windows event log (Event Viewer, Application, Error 1000) works
as it is. The Release build has no PDB: reproduce with `--debug`.

For the plug-in, `C:\msys64\ucrt64\bin\addr2line.exe -f -e app\bin\ucxmatter.dll ADDR` (ADDR relative to its
image base) does what `--symbolize` does for the app.

## Lessons from the Matter plug-in

- The plug-in is unloaded (`FreeLibrary`) on every `matter.stop` and every failed start. No pointer into it may
  outlive it: a response prefix string of the plug-in left in the AT client (`pExpectedRsp`) crashed the next
  `dev.reboot` in `parseLine`. `unload()` clears the client's state under its command mutex.
- A start that fails halfway must still shut the Matter stack down: `NodeStart` marks the stack up before
  `InitChipStack`, otherwise the half-made stack's destructor runs with its thread alive (`0xC0000409`).
- What a session leaves on the module outlives it: a reloaded web page never runs `matter.stop`, so its UDP socket
  stays bound to 5540, and a GATT service stays defined until the module reboots. Check the module's state
  (`at AT+USOST?`, `at AT+UBTGS…` failing with NO_MEMORY) before suspecting the code.
- A real module can be put in the failing state from the shell: about 30 × `--exec "at AT+UBTGS=FFF7"` fills the
  NORA-W36 GATT table.

## u-connectClient

Its unit tests run with Ceedling (installed in WSL here). Run them on a copy in WSL's own file system: on
`/mnt/e` Ceedling takes more than 20 minutes, on the copy 10 seconds. `ports/extra` (left out of the copy, it is
big) must exist as a folder for the configuration to load:

```powershell
wsl -e bash -c "rm -rf ~/ucc && mkdir -p ~/ucc/ports/extra && cd /mnt/e/u-blox/u-connectXpressApp/u-connectClient && tar --exclude=./build --exclude=./ports/extra --exclude=./.git -cf - . | tar -xf - -C ~/ucc && cd ~/ucc && ceedling test:test_u_cx_at_client"
```

A fix there goes to u-connectClient as its own pull request (branch off `cmag_merged_api`, with a unit test in
`test/`); the app keeps a workaround until the submodule points at the merged fix.
