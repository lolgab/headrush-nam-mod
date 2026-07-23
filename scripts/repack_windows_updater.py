#!/usr/bin/env python3
"""
repack_windows_updater.py -- swap Update.img inside the official HeadRush
Pedalboard Windows Firmware Updater .exe for a NAM-modded one.

The Windows updater is a plain 7-Zip SFX installer: a PE stub, a small text
config block (`;!@Install@!UTF-8! ... ;!@InstallEnd@!` -- standard 7-Zip SFX
syntax, here just `RunProgram="FirmwareUpdater.exe"`), then a normal 7z
archive containing:

    Background.png
    Config.json
    Update.img
    FirmwareUpdater.exe
    libusb-1.0.dll

Config.json (device-update UI strings/layout) has no checksum of Update.img,
so replacing that one entry and rebuilding the 7z archive is enough -- no
need to touch the PE stub or FirmwareUpdater.exe itself. This mirrors what
scripts/quickstart_mac.sh does to the .app bundle's Contents/Resources/, just
with a different container format.

Requires the `7z` CLI (p7zip-full on Linux/WSL, `brew install p7zip` on
macOS) -- not part of docker/Dockerfile since this step never touches the
ARM cross-compiled artifacts, only host-side archive repacking.
"""
import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SFX_END_MARKER = b";!@InstallEnd@!"
EXPECTED_ENTRIES = {"Background.png", "Config.json", "Update.img", "FirmwareUpdater.exe", "libusb-1.0.dll"}


def strip_authenticode_directory(prefix):
    """Zero the PE Security Directory entry (Authenticode signature pointer)
    in the SFX stub. The stock exe is code-signed, with the signature
    appended as a WIN_CERTIFICATE block after the 7z archive, referenced by
    an *absolute file offset* in this directory entry. Any edit to the
    archive changes the file's total length, so that offset would point into
    the middle of our new archive instead of a valid certificate -- and 7z's
    own PE parser refuses to recognize the whole file as PE+overlay at all
    when it can't read a valid cert there (not just skip it). We can't
    produce a signature that verifies over modified bytes anyway, so this
    just declares there is none, which also matches reality (Windows will
    show the repacked installer as unsigned, same as any other build in this
    repo -- Anxiety OD hijack, ARM binary patches, etc. are already
    unsigned)."""
    e_lfanew = struct.unpack_from("<I", prefix, 0x3C)[0]
    if prefix[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        die("stock updater's PE header doesn't look like a standard PE32 image "
            "-- re-verify this script's offset math before proceeding.")
    coff_off = e_lfanew + 4
    (sizeopt,) = struct.unpack_from("<H", prefix, coff_off + 16)
    opt_off = coff_off + 20
    (magic,) = struct.unpack_from("<H", prefix, opt_off)
    if magic != 0x10B:
        die(f"expected a PE32 (not PE32+) optional header (magic 0x10b), got {magic:#x} "
            f"-- re-verify this script's offset math before proceeding.")
    numrva_off = opt_off + 92
    (numrva,) = struct.unpack_from("<I", prefix, numrva_off)
    if numrva < 5:
        die(f"PE optional header has only {numrva} data directories, expected at least 5 "
            f"(need index 4, Security) -- re-verify this script's offset math.")
    sec_entry_off = numrva_off + 4 + 4 * 8  # index 4 = IMAGE_DIRECTORY_ENTRY_SECURITY
    patched = bytearray(prefix)
    struct.pack_into("<II", patched, sec_entry_off, 0, 0)
    return bytes(patched)


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sh(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    kw.setdefault("check", True)
    return subprocess.run(cmd, **kw)


def find_7z():
    for c in ("7z", "7zz", "7za"):
        if shutil.which(c):
            return c
    die("7z not found -- install p7zip-full (Linux/WSL) or `brew install p7zip` (macOS).")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock_exe", type=Path, help="stock HeadRush Pedalboard 2.7 Windows Firmware Updater .exe")
    ap.add_argument("patched_img", type=Path, help="NAM-modded Update.img (from build.sh / build_docker.sh)")
    ap.add_argument("output_exe", type=Path, help="path to write the patched updater .exe")
    ap.add_argument("--keep-work-dir", action="store_true")
    args = ap.parse_args()

    for p in (args.stock_exe, args.patched_img):
        if not p.is_file():
            die(f"{p} not found")

    sevenzip = find_7z()

    data = args.stock_exe.read_bytes()
    marker_idx = data.find(SFX_END_MARKER)
    if marker_idx == -1:
        die(f"{args.stock_exe} doesn't look like a 7-Zip SFX installer (no {SFX_END_MARKER!r} marker found) "
            f"-- HeadRush may have changed the Windows updater's packaging.")
    archive_start = marker_idx + len(SFX_END_MARKER)
    sfx_prefix = strip_authenticode_directory(data[:archive_start])

    workdir_ctx = tempfile.TemporaryDirectory(prefix="headrush-nam-winrepack-")
    work = Path(workdir_ctx.name)
    print(f"working directory: {work}")

    try:
        payload = work / "payload"
        payload.mkdir()

        sh([sevenzip, "x", f"-o{payload}", str(args.stock_exe), "-y"])

        found = {p.name for p in payload.iterdir()}
        if found != EXPECTED_ENTRIES:
            die(f"stock updater's archive contents changed -- expected {sorted(EXPECTED_ENTRIES)}, "
                f"found {sorted(found)}. Re-verify this script against the new layout before proceeding.")

        shutil.copyfile(args.patched_img, payload / "Update.img")

        new_archive = work / "new.7z"
        # cwd=payload + bare names, not str(payload / name): 7z stores whatever
        # path it's given, and the SFX module expects flat filenames, not this
        # temp directory's absolute path baked into every archive entry.
        sh([sevenzip, "a", str(new_archive)] + sorted(EXPECTED_ENTRIES), cwd=payload)

        args.output_exe.parent.mkdir(parents=True, exist_ok=True)
        with open(args.output_exe, "wb") as f:
            f.write(sfx_prefix)
            f.write(new_archive.read_bytes())

        # ---- round-trip verification ----
        test_result = sh([sevenzip, "t", str(args.output_exe)], capture_output=True, text=True, check=False)
        if test_result.returncode != 0:
            print(test_result.stdout, test_result.stderr)
            die("7z integrity test on the repacked .exe failed -- refusing to leave a broken installer in place")

        verify_dir = work / "verify"
        verify_dir.mkdir()
        sh([sevenzip, "x", f"-o{verify_dir}", str(args.output_exe), "-y"])
        verify_found = {p.name for p in verify_dir.iterdir()}
        if verify_found != EXPECTED_ENTRIES:
            die(f"repacked .exe round-trip produced {sorted(verify_found)}, expected {sorted(EXPECTED_ENTRIES)}")
        for name in EXPECTED_ENTRIES - {"Update.img"}:
            if (verify_dir / name).read_bytes() != (payload / name).read_bytes():
                die(f"repacked .exe's {name} doesn't match the stock one byte-exact")
        if (verify_dir / "Update.img").read_bytes() != args.patched_img.read_bytes():
            die("repacked .exe's Update.img doesn't match the patched image byte-exact")
        print("OK  round-trip verified: repacked .exe extracts byte-exact "
              "(unchanged Background.png/Config.json/FirmwareUpdater.exe/libusb-1.0.dll, patched Update.img)")

        print(f"\nOK  wrote {args.output_exe}")

    finally:
        if args.keep_work_dir:
            print(f"--keep-work-dir: work directory left at {work}")
            workdir_ctx._finalizer.detach()
        else:
            workdir_ctx.cleanup()


if __name__ == "__main__":
    main()
