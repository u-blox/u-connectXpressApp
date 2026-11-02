#!/usr/bin/env python3
"""Release u-connectXpressApp: tag vX.Y.Z.YYMMDD, let CI build Linux, STM32 (FreeRTOS and Zephyr) and the
browser app, sign the Windows exe and publish one archive with the layout windows/, linux/,
stm32/stm32_<board>/, zephyr/zephyr_<board>/ and wasm/ (every part is required).

1. Checks the work tree is clean, on master and in sync with origin.
2. Rebuilds every target (ucxapp.py --everything --rebuild: Windows, Linux via WSL, all STM32 boards on FreeRTOS
   and Zephyr, WebAssembly, offline tests) and commits the refreshed memory tables in README.md and
   docs/STM32_MEMORY.md.
3. Builds the Windows Release exe and signs it (the EV certificate is only available locally).
4. Tags and pushes; .github/workflows/release.yml builds Linux, STM32, Zephyr and WebAssembly into the
   `release-tree` artifact.
5. Downloads that tree, adds windows/ucxapp.exe and creates the release with
   ucxapp-<tag>.zip plus ucxapp.exe, ucxapp (Linux) and ucxapp-wasm-<tag>.zip as direct downloads.

X.Y.Z is U_CX_VERSION_MAJOR/MINOR/PATCH in u_cx_version.h (the same header app/CMakeLists.txt reads), YYMMDD is today.

Examples:
    python scripts/release.py --dry-run     # build and sign only, nothing is tagged or pushed
    python scripts/release.py --publish     # full release, published instead of left as a draft
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_THUMBPRINT = "EF3FD135F1CD669E0D7F4F2CF14FE1334EECD16E"
STM32_BOARDS = ("h753", "f767", "f439", "f407", "f401", "f446", "f103", "g071")
IS_WINDOWS = platform.system() == "Windows"


def fail(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def step(msg):
    print(f"\n== {msg}")


def run(*cmd, check=True):
    result = subprocess.run([str(c) for c in cmd], cwd=ROOT, text=True, capture_output=False)
    if check and result.returncode != 0:
        fail(f"{' '.join(str(c) for c in cmd)} failed ({result.returncode})")


def out(*cmd):
    result = subprocess.run([str(c) for c in cmd], cwd=ROOT, text=True, capture_output=True)
    return result.returncode, result.stdout.strip()


def read_version():
    header_path = ROOT / "u-connectClient" / "ucx_api" / "generated" / "NORA-W36X" / "u_cx_version.h"
    header = header_path.read_text(encoding="utf-8")
    parts = []
    for n in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define U_CX_VERSION_{n} (\d+)", header)
        if not m:
            fail(f"U_CX_VERSION_{n} not found in u_cx_version.h")
        parts.append(m.group(1))
    return "v{}.{}.{}.{}".format(*parts, datetime.date.today().strftime("%y%m%d"))


def find_signtool():
    base = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    found = sorted((p for p in base.rglob("signtool.exe") if "x64" in p.parts), reverse=True)
    if not found:
        fail("signtool.exe not found (install the Windows SDK)")
    return found[0]


def build_and_sign(thumbprint, tree):
    signtool = find_signtool()
    step("Building Windows Release")
    run("cmake", "-S", "app", "-B", "build/windows", "-A", "x64")
    run("cmake", "--build", "build/windows", "--config", "Release", "--target", "ucxapp", "--clean-first")
    built = ROOT / "app" / "bin" / "ucxapp.exe"
    if not built.exists():
        fail(f"{built} not found")

    step("Signing")
    folder = tree / "windows"
    folder.mkdir(parents=True)
    exe = folder / "ucxapp.exe"
    shutil.copy2(built, exe)
    run(signtool, "sign", "/sha1", thumbprint, "/tr", "http://timestamp.digicert.com",
        "/td", "sha256", "/fd", "sha256", exe)
    run(signtool, "verify", "/pa", exe)
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    Path(f"{exe}.sha256").write_text(f"{digest}  {exe.name}\n", encoding="ascii")
    print(f"SHA-256: {digest}")
    return exe


def wait_for_run(tag, minutes):
    step("Waiting for the CI release build of the tag")
    deadline = time.time() + minutes * 60
    sha = out("git", "rev-parse", f"{tag}^{{commit}}")[1].strip()
    while time.time() < deadline:
        code, text = out("gh", "run", "list", "--workflow", "release.yml", "--branch", tag,
                         "--json", "databaseId,headSha", "--limit", "5")
        # A re-created tag keeps the old runs listed, so match the commit.
        runs = [r for r in (json.loads(text) if code == 0 and text else []) if r["headSha"] == sha]
        if runs:
            run_id = runs[0]["databaseId"]
            run("gh", "run", "watch", run_id, "--exit-status", "--interval", "30")
            return run_id
        time.sleep(10)
    fail(f"no release.yml run for {tag} after {minutes} min; check the Actions tab")


def package(tree, archive, root=None):
    """Zip the tree (or the root folder of it); zipfile keeps the Linux binary executable."""
    if root is None:
        missing = [b for b in STM32_BOARDS if not (tree / "stm32" / f"stm32_{b}").is_dir()]
        missing += [f"zephyr_{b}" for b in STM32_BOARDS if not (tree / "zephyr" / f"zephyr_{b}").is_dir()]
        for f in ("windows/ucxapp.exe", "linux/ucxapp", "wasm/ucxapp.wasm"):
            if not (tree / f).is_file():
                missing.append(f)
        if missing:
            fail(f"release tree incomplete: {missing}")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted((root or tree).rglob("*")):
            if f.is_file():
                info = zipfile.ZipInfo.from_file(f, f.relative_to(tree).as_posix())
                info.external_attr = (0o755 if f.name == "ucxapp" else 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, f.read_bytes())
    print(f"Packaged: {archive}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--thumbprint", default=DEFAULT_THUMBPRINT)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--publish", action="store_true")
    ap.add_argument("--wait-minutes", type=int, default=30)
    ap.add_argument("--resume", action="store_true",
                    help="tag already pushed and dist/<tag> signed: wait for CI, package and create the release")
    args = ap.parse_args()

    tag = read_version()
    print(f"Release tag: {tag}")
    if args.resume:
        tree = ROOT / "dist" / tag / "ucxapp"
        exe = tree / "windows" / "ucxapp.exe"
        if not exe.is_file() or not out("git", "tag", "-l", tag)[1]:
            fail(f"nothing to resume: need tag {tag} and {exe}")
        return finish(args, tag, tree, exe)

    step("Checking repository")
    if not shutil.which("gh"):
        fail("GitHub CLI (gh) not found")
    if IS_WINDOWS and not shutil.which("cmake"):
        fail("cmake not found")
    if not args.dry_run:
        run("gh", "auth", "status")
        if out("git", "status", "--porcelain")[1]:
            fail("Work tree is not clean; commit or stash first")
        if out("git", "branch", "--show-current")[1] != "master":
            fail("Not on master")
        run("git", "fetch", "origin", "master", "--tags")
        if out("git", "rev-parse", "HEAD")[1] != out("git", "rev-parse", "origin/master")[1]:
            fail("master differs from origin/master; push or pull first")
        if out("git", "tag", "-l", tag)[1]:
            fail(f"Tag {tag} already exists")

    if not IS_WINDOWS:
        fail("run the release on Windows: the Windows exe is signed with the local EV certificate")

    step("Building every target")
    run(sys.executable, "ucxapp.py", "--everything", "--rebuild")
    if not args.dry_run and out("git", "status", "--porcelain", "README.md", "docs/STM32_MEMORY.md")[1]:
        step("Committing the updated memory tables")
        run("git", "commit", "-m", f"Update memory footprint for {tag}", "README.md", "docs/STM32_MEMORY.md")
        run("git", "push", "origin", "master")

    dist = ROOT / "dist" / tag
    shutil.rmtree(dist, ignore_errors=True)
    tree = dist / "ucxapp"
    exe = build_and_sign(args.thumbprint, tree)

    if args.dry_run:
        print(f"\nDry run done: {exe}")
        return

    step(f"Tagging and pushing {tag}")
    run("git", "tag", "-a", tag, "-m", f"Release u-connectXpressApp {tag}")
    run("git", "push", "origin", tag)
    finish(args, tag, tree, exe)


def finish(args, tag, tree, exe):
    dist = tree.parent
    run_id = wait_for_run(tag, args.wait_minutes)
    step("Downloading the Linux, STM32, Zephyr and WebAssembly builds")
    run("gh", "run", "download", run_id, "--name", "release-tree", "--dir", tree)
    archive = dist / f"ucxapp-{tag}.zip"
    package(tree, archive)
    assets = [archive, exe, tree / "linux" / "ucxapp"]
    web = dist / f"ucxapp-wasm-{tag}.zip"
    package(tree, web, tree / "wasm")
    assets.append(web)

    step("Creating the release")
    run("gh", "release", "create", tag, *assets,
        "--title", f"u-connectXpressApp {tag}", "--notes-file", ".github/release-notes.md",
        "--generate-notes", *([] if args.publish else ["--draft"]))
    print(f"Release: {out('gh', 'release', 'view', tag, '--json', 'url', '--jq', '.url')[1]}")
    if not args.publish:
        print(f"Draft ready. Review it and publish: gh release edit {tag} --draft=false")


if __name__ == "__main__":
    main()
