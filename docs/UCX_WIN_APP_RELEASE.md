# Release process

One command builds every target, signs, tags and publishes:

```powershell
python scripts/release.py --dry-run     # build everything and sign; nothing is tagged or pushed
python scripts/release.py               # the release as a draft on GitHub (--publish publishes it at once)
```

`.\ucxapp --release` is the same command.

## What it does

1. Checks the work tree: clean, on `master`, in sync with `origin`, tag not used yet.
2. `ucxapp.py --everything --rebuild`: Windows, Linux (WSL or `--linux-host`), every STM32 board on FreeRTOS and
   Zephyr, the browser app and the Matter plug-in, then the offline tests. The refreshed memory tables in
   `README.md` and `docs/STM32_MEMORY.md` are committed.
3. Rebuilds the Windows exe and signs it and `ucxmatter.dll` with the u-blox EV certificate (the token must be
   plugged in; `--thumbprint` picks another certificate).
4. Tags `vX.Y.Z.YYMMDD` and pushes the tag. `.github/workflows/release.yml` then builds Linux, every STM32 board on
   FreeRTOS and Zephyr, the browser app and the Matter plug-in into the `release-tree` artifact.
5. Downloads that tree, adds the signed `windows/` folder and creates the release from
   `.github/release-notes.md`: `ucxapp-<tag>.zip` with every target, plus `ucxapp.exe`, `ucxmatter.dll`, `ucxapp`
   and `libucxmatter.so` (Linux) and `ucxapp-wasm-<tag>.zip` as direct downloads.

Every part is required: the script stops when a board, a Matter plug-in or the Matter node of the browser app is
missing. After a CI failure, fix it and run `python scripts/release.py --resume` (tag pushed, `dist/<tag>` signed).

## Version

`X.Y.Z` is the u-connectXpress API version (`U_CX_VERSION_*` in u-connectClient's `u_cx_version.h`), `YYMMDD`
the build date. CMake writes it into the exe (`app/version.h.in`); `app.version` shows it.

## Before a release

- CI is green on `master` (`gh run list`), coverage included.
- `.\ucxapp --test` passes, and the hardware tests on the bench: `.\ucxapp --detect --test tests/hardware`.
- The soak test ran for 24 hours: `UCX_SOAK_HOURS=24`, `python -m pytest tests/hardware/test_soak.py -m soak`.
- `.github/release-notes.md` says what is new.

## What the release PC needs

Visual Studio 2022 Build Tools and CMake, WSL (or a Linux machine for `--linux-host`), the Arm GNU toolchain with
the STM32Cube HALs, a Zephyr workspace, emsdk, MSYS2 GCC (the Matter plug-in), the Windows SDK (`signtool`), the
certificate token and a logged-in `gh`.

## Signing only

```powershell
.\ucxapp --rebuild --sign EF3FD135F1CD669E0D7F4F2CF14FE1334EECD16E
```

writes `app/release/ucxapp-signed.exe` and, next to it, the signed `ucxmatter.dll`.
