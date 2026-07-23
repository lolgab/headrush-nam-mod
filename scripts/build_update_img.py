#!/usr/bin/env python3
"""
build_update_img.py -- take a stock HeadRush Update.img (Pedalboard 2.7 or MX5
2.7 -- auto-detected, or pick with --model; see scripts/model_targets.py) and
produce a modified one with the NAM (Neural Amp Modeler) mod applied via the
Anxiety OD (v1) pedal's process() hijack (patch_gonkulator.py) -- the user's
own choice of a pedal they're fine sacrificing board-wide (Gonkulator turned
out to be dead code, unwired to any UI page; the real Ring Mod pedal has no
findable static-analysis path; Volume works mechanically but the user needs
its real function -- see patch_gonkulator.py's docstring and README.md).
Anxiety OD's own knob (Drive, Tone, or Level) selects/scans .nam model files
instead of controlling its assigned overdrive parameter.

patch_namloader.py's additive, own-pedal-type design (own ModFac_construct
case, own engine/vtables) is NOT applied by this build -- it's unreachable
from Evil's own pedal-add menu (see README.md "Known limitation") and it
live-patches the ModFac_construct dispatch instruction, a hot path run on
every pedal construction. Not worth the risk for a pedal nothing can select
yet. The script is kept in patch/ for future manual use if the menu/DB
integration ever gets solved.

Never modifies its input. Always writes a new Update.img. See README.md for
prerequisites (ARM cross toolchain, e2fsprogs, u-boot-tools, the nam_core
submodule).
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_image  # noqa: E402
import model_targets  # noqa: E402


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sh(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    kw.setdefault("check", True)
    return subprocess.run(cmd, **kw)


def find_tool(candidates, hint):
    for c in candidates:
        if shutil.which(c):
            return c
        if Path(c).is_file():
            return c
    die(f"could not find {candidates[0]} -- {hint}")


class Toolchain:
    """Locates build tools across macOS (Homebrew), Linux (distro packages),
    and the docker/ image (a Bootlin glibc-2.31 cross toolchain) alike --
    see scripts/build_docker.sh, the recommended way to get all of these at
    once regardless of host OS."""

    def __init__(self):
        mac_bin = "/opt/homebrew/opt/armv7-unknown-linux-gnueabihf/bin"
        mac_prefix = "armv7-unknown-linux-gnueabihf-"
        # Bootlin glibc-2.31 toolchain the docker/ image installs -- targets a
        # glibc <= the device's (2.32), unlike Debian bookworm's
        # crossbuild-essential-armhf (glibc 2.36), whose too-new symbol versions
        # make the device hang on boot. See docker/Dockerfile and
        # check_glibc_compat() below.
        docker_prefix = "arm-buildroot-linux-gnueabihf-"
        linux_prefix = "arm-linux-gnueabihf-"  # Debian/Ubuntu crossbuild-essential-armhf / gcc-arm-linux-gnueabihf

        def arm_tool(name):
            return find_tool(
                [f"{mac_bin}/{mac_prefix}{name}", f"{docker_prefix}{name}",
                 f"{linux_prefix}{name}", f"{mac_prefix}{name}"],
                "install a glibc<=2.32 ARM32 hard-float cross toolchain -- macOS: "
                "`brew tap messense/macos-cross-toolchains && brew install armv7-unknown-linux-gnueabihf`; "
                "Linux/Windows-WSL: use scripts/build_docker.sh (its image installs a glibc-2.31 toolchain).")

        self.arm_as = arm_tool("as")
        self.arm_objcopy = arm_tool("objcopy")
        self.arm_gxx = arm_tool("g++")
        self.arm_strip = arm_tool("strip")
        self.arm_readelf = arm_tool("readelf")  # for the post-build glibc-compat check

        e2fs_sbin = "/opt/homebrew/opt/e2fsprogs/sbin"
        e2fs_hint = ("install e2fsprogs -- macOS: `brew install e2fsprogs`; "
                     "Debian/Ubuntu (incl. WSL): `apt install e2fsprogs`; "
                     "or use scripts/build_docker.sh.")
        self.debugfs = find_tool([f"{e2fs_sbin}/debugfs", "/sbin/debugfs", "debugfs"], e2fs_hint)
        self.e2fsck = find_tool([f"{e2fs_sbin}/e2fsck", "/sbin/e2fsck", "e2fsck"], e2fs_hint)

        self.mkimage = find_tool(["mkimage"],
                                  "install u-boot-tools -- macOS: `brew install u-boot-tools`; "
                                  "Debian/Ubuntu (incl. WSL): `apt install u-boot-tools`; "
                                  "or use scripts/build_docker.sh.")
        self.xz = find_tool(["xz"],
                             "should be preinstalled; macOS: `brew install xz`; "
                             "Debian/Ubuntu (incl. WSL): `apt install xz-utils`.")


def extract_update_img(img_path, work):
    data = img_path.read_bytes()
    props = fit_image.parse_fdt(data)
    metadata = fit_image.read_root_metadata(data, props)

    splash = fit_image.read_prop_bytes(data, props, "//images/splash", "data")
    recoverysplash = fit_image.read_prop_bytes(data, props, "//images/recoverysplash", "data")
    rootfs_xz = fit_image.read_prop_bytes(data, props, "//images/rootfs", "data")

    (work / "splash.xz").write_bytes(splash)
    (work / "recoverysplash.xz").write_bytes(recoverysplash)
    (work / "rootfs_orig.xz").write_bytes(rootfs_xz)
    print(f"OK  extracted splash ({len(splash)}B), recoverysplash ({len(recoverysplash)}B), "
          f"rootfs.xz ({len(rootfs_xz)}B) from {img_path.name}")
    return metadata


def assemble(tc, s_path, out_bin, work):
    o_path = work / (s_path.stem + ".o")
    sh([tc.arm_as, "-mfpu=neon-vfpv4", "-mfloat-abi=hard", str(s_path), "-o", str(o_path)])
    sh([tc.arm_objcopy, "-O", "binary", str(o_path), str(out_bin)])


def build_nam_libs(tc, work):
    nam_core = REPO_ROOT / "nam_core"
    if not (nam_core / "NAM" / "dsp.h").is_file():
        die(f"{nam_core} doesn't look like a populated NeuralAmpModelerCore checkout -- "
            f"run: git submodule update --init --recursive")

    hook_so = work / "libnam_hook.so"
    cpp_sources = [str(REPO_ROOT / "patch" / "nam_hook.cpp")]
    cpp_sources += [str(p) for p in sorted((nam_core / "NAM").glob("*.cpp"))]
    cpp_sources += [str(p) for p in sorted((nam_core / "NAM" / "wavenet").glob("*.cpp"))]
    # -O3/-ffast-math: NAM inference (WaveNet) is CPU-heavy relative to this
    # embedded ARM core's real-time per-block budget -- real hardware testing
    # showed audible glitching consistent with missed deadlines (see
    # nam_hook.cpp's max_process_us/over_budget_count trace). -O2 was overly
    # conservative for numeric-heavy Eigen/DSP code; -ffast-math is standard
    # practice for audio DSP (relaxes strict IEEE754 compliance for speed,
    # no correctness risk for this use case).
    # -std=c++2a (not c++20): same standard, but the spelling GCC 9.3 also
    # accepts -- the docker/ image's Bootlin glibc-2.31 toolchain is GCC 9.3.
    sh([tc.arm_gxx, "-std=c++2a", "-O3", "-ffast-math", "-fPIC", "-shared", "-march=armv7-a", "-mfpu=neon-vfpv4",
        "-mfloat-abi=hard", "-DNAM_ENABLE_A2_FAST", "-DNAM_SAMPLE_FLOAT",
        "-static-libstdc++", "-static-libgcc",
        f"-I{nam_core}", f"-I{nam_core}/NAM", f"-I{nam_core}/Dependencies/eigen",
        f"-I{nam_core}/Dependencies/nlohmann",
        *cpp_sources, "-lpthread", "-o", str(hook_so)])

    preload_so = work / "libnam_preload.so"
    sh([tc.arm_gxx, "-std=c++17", "-O2", "-fPIC", "-shared", "-march=armv7-a", "-mfpu=neon-vfpv4",
        "-mfloat-abi=hard", str(REPO_ROOT / "patch" / "nam_preload.cpp"), "-ldl", "-o", str(preload_so)])

    hook_stripped = work / "libnam_hook.stripped.so"
    preload_stripped = work / "libnam_preload.stripped.so"
    sh([tc.arm_strip, "--strip-unneeded", "-o", str(hook_stripped), str(hook_so)])
    sh([tc.arm_strip, "--strip-unneeded", "-o", str(preload_stripped), str(preload_so)])
    return hook_stripped, preload_stripped


NAM_MOD_BLOCK_RE = None  # set below, needs re module


def build_launcher_script(stock_script_text, gonk_env=None):
    import re
    global NAM_MOD_BLOCK_RE
    if NAM_MOD_BLOCK_RE is None:
        NAM_MOD_BLOCK_RE = re.compile(r"# --- NAM mod ---.*?# --- end NAM mod ---\n", re.DOTALL)

    if gonk_env is not None:
        mod_desc = (
            "# Anxiety OD (v1) process() hijack -- the only NAM path this build applies.\n"
            "# One of its knobs (Drive/Tone/Level) now selects/scans .nam model files.\n"
            "# (The additive, own-pedal-type design in patch_namloader.py is NOT\n"
            "# applied here -- see README.md.)\n"
        )
        hook_env_str = f"NAM_HOOK_SLOT_GONK_ADDR={gonk_env['NAM_HOOK_SLOT_GONK_ADDR']} "
    else:
        mod_desc = (
            "# DIAGNOSTIC-ONLY build -- no vtable patched, Evil is byte-for-byte\n"
            "# stock. Only runs the live-memory census scanner (see nam_hook.cpp/\n"
            "# nam_preload.cpp) to see what's actually constructed, without risking\n"
            "# any real pedal the user can't afford to lose.\n"
        )
        hook_env_str = ""

    comment = (
        "# --- NAM mod ---\n"
        + mod_desc +
        "# LD_PRELOAD/NAM_HOOK_SLOT_*_ADDR are scoped to the /usr/Evil/Evil exec\n"
        "# below via `env`, NOT exported here -- exporting them shell-wide would\n"
        "# also preload libnam_preload.so into systemd-inhibit (a separate,\n"
        "# dynamically-linked ELF binary launched right below). Its constructor\n"
        "# writes to a hardcoded absolute vaddr valid only inside Evil's own\n"
        "# non-PIE layout; inside systemd-inhibit's unrelated address space that\n"
        "# write hits unmapped memory and segfaults it before it ever forks Evil\n"
        "# -- an infinite crash loop, stuck on the splash screen forever.\n"
        "# --- end NAM mod ---\n"
    )

    if NAM_MOD_BLOCK_RE.search(stock_script_text):
        script = NAM_MOD_BLOCK_RE.sub(comment, stock_script_text)
    else:
        marker = "while [ 1 ]"
        idx = stock_script_text.index(marker)
        script = stock_script_text[:idx] + comment + "\n" + stock_script_text[idx:]

    old_exec = "systemd-inhibit --what=handle-power-key /usr/Evil/Evil"
    new_exec = (
        "systemd-inhibit --what=handle-power-key "
        "env LD_PRELOAD=/usr/Evil/libnam_preload.so "
        f"{hook_env_str}"
        "/usr/Evil/Evil"
    )
    count = script.count(old_exec)
    if count != 1:
        die(f"expected exactly 1 occurrence of {old_exec!r} in the launcher script, found {count} "
            f"-- stock script layout changed, re-verify before patching")
    return script.replace(old_exec, new_exec)


def device_glibc_version(tc, rootfs_bin):
    """Device glibc (major, minor) read from /lib/libc.so.6's symlink target
    (libc-X.Y.so) in the rootfs; None if undeterminable."""
    r = sh([tc.debugfs, "-R", "stat /lib/libc.so.6", str(rootfs_bin)],
           capture_output=True, text=True, check=False)
    m = re.search(r'libc-(\d+)\.(\d+)\.so', r.stdout)
    return (int(m.group(1)), int(m.group(2))) if m else None


def so_required_glibc(tc, so_path):
    """Highest GLIBC_x.y symbol version the .so requires, as (x, y)."""
    r = sh([tc.arm_readelf, "-V", str(so_path)], capture_output=True, text=True, check=False)
    vers = [(int(a), int(b)) for a, b in re.findall(r"GLIBC_(\d+)\.(\d+)", r.stdout)]
    return max(vers) if vers else (0, 0)


def check_glibc_compat(tc, rootfs_bin, so_paths):
    """Refuse to package .so files that require newer glibc symbol versions than
    the device provides -- that mismatch makes Evil fail to load the preload/hook
    libs and hang on the boot splash. It bites any build whose ARM toolchain
    targets too-new a glibc, e.g. Debian bookworm's crossbuild-essential-armhf
    (glibc 2.36) against this glibc-2.32 device; docker/Dockerfile ships a
    glibc-2.31 toolchain to avoid it, and this is the backstop."""
    dev = device_glibc_version(tc, rootfs_bin)
    if dev is None:
        print("WARN  could not determine device glibc version -- skipping glibc compat check")
        return
    for so in so_paths:
        need = so_required_glibc(tc, so)
        if need > dev:
            die(f"{Path(so).name} requires GLIBC_{need[0]}.{need[1]} but this device provides "
                f"only up to GLIBC_{dev[0]}.{dev[1]} -- the flashed image WOULD HANG ON BOOT "
                f"(Evil can't dlopen/preload it). Your ARM cross-toolchain targets too-new a "
                f"glibc. Build via scripts/build_docker.sh (its image uses a glibc-2.31 "
                f"toolchain), or point at a glibc<={dev[0]}.{dev[1]} toolchain. Current g++: {tc.arm_gxx}")
    print(f"OK  glibc compat: both libs require <= GLIBC_{dev[0]}.{dev[1]} "
          f"(device provides GLIBC_{dev[0]}.{dev[1]})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input_img", type=Path, help="stock HeadRush Update.img")
    ap.add_argument("output_img", type=Path, help="path to write the modified Update.img")
    ap.add_argument("--model", default=None, choices=sorted(model_targets.TARGETS),
                    help="target model (default: auto-detect from Update.img compatible, "
                         f"else {model_targets.DEFAULT_TARGET})")
    ap.add_argument("--keep-work-dir", action="store_true", help="don't delete the temp build directory")
    args = ap.parse_args()

    if not args.input_img.is_file():
        die(f"{args.input_img} not found")

    tc = Toolchain()

    workdir_ctx = tempfile.TemporaryDirectory(prefix="headrush-nam-build-")
    work = Path(workdir_ctx.name)
    print(f"working directory: {work}")

    try:
        metadata = extract_update_img(args.input_img, work)

        target, reason = model_targets.select_target(args.model, metadata.get("compatible"))
        print(f"OK  model target: {target.name}  ({reason})")

        with open(work / "rootfs.bin", "wb") as f:
            sh([tc.xz, "-d", "-k", "-T0", "-c", str(work / "rootfs_orig.xz")], stdout=f)

        sh([tc.debugfs, "-R", f"dump /usr/Evil/Evil {work / 'Evil'}", str(work / "rootfs.bin")])
        # NOTE: "dump", not "cat" -- debugfs's "cat" interleaves its own banner
        # text with the file's stdout, which would corrupt the extracted script.
        sh([tc.debugfs, "-R", f"dump /usr/Evil/Scripts/evil {work / 'evil_script_orig.sh'}", str(work / "rootfs.bin")])

        # Anxiety OD ("process()" vtable slot 8) hijack via patch_gonkulator.py:
        # repoints AnxietyOD's real, live engine vtable at an injected trampoline
        # running NAM inference, falling back to the original process() when no
        # hook is installed. The exact addresses are per-model (see
        # scripts/model_targets.py; Pedalboard 2.7 vtable 0x1839044 / process
        # 0x3260e0, MX5 2.7 vtable 0x17ee460 / process 0x302ed0 -- both cross-
        # confirmed via RTTI xref-chasing and ctor/clone disassembly, see
        # patch_gonkulator.py's docstring). patch_namloader.py
        # (unreachable additive design, also live-patches the
        # ModFac_construct dispatch hot path) and patch_modfac_spy.py (needs
        # a code cave beyond ARM B/BL's +-32MB reach of its target) are NOT
        # applied here. patch_knob_labels.py (an earlier, naive attempt at
        # relabeling QML text) is superseded by patch_qml_labels.py below,
        # which patches the actual compiled QML string table instead of
        # source text Evil never reads at runtime.
        tramp_bin = work / "trampoline_gonk.bin"
        assemble(tc, REPO_ROOT / "patch" / "trampoline_gonk.S", tramp_bin, work)

        evil_hijacked = work / "Evil.hijacked"
        sh([sys.executable, str(REPO_ROOT / "patch" / "patch_gonkulator.py"),
            str(work / "Evil"), str(tramp_bin), str(evil_hijacked),
            "--engine-vtable", hex(target.engine_vtable_vaddr),
            "--orig-fn", hex(target.orig_process_fn)])
        gonk_env = json.loads((work / "Evil.hijacked.json").read_text())

        # patch_qml_labels.py: targets Anxiety OD's own raw QRC-embedded QML
        # source blob (found via its unique propertyPath string, not
        # proximity guessing -- see its docstring). Refuses on mismatch.
        # Skipped for models with no per-pedal QML relabel target (e.g. MX5,
        # whose knob labels come from a shared string pool -- see model_targets).
        evil_labeled = work / "Evil.labeled"
        if target.qml_renames:
            sh([sys.executable, str(REPO_ROOT / "patch" / "patch_qml_labels.py"),
                str(evil_hijacked), str(evil_labeled), "--model", target.name])
        else:
            print(f"    (QML knob relabel skipped for {target.name} -- no per-pedal target)")
            shutil.copyfile(evil_hijacked, evil_labeled)

        # patch_pedal_title.py DISABLED: v43 real-hardware test showed
        # renaming this ASCII table entry breaks the pedal entirely (stuck
        # bypassed, controls read-only, generic fallback labels shown
        # instead of the custom page at all) -- the string is very likely
        # used as an internal type-name lookup key (page routing / type
        # recognition), not just display text. Renaming it in place breaks
        # that lookup. Do not re-enable without finding what reads this
        # table as a KEY (not just a label) and confirming the rename is
        # safe there too.
        evil_final = work / "Evil.patched"
        shutil.copyfile(evil_labeled, evil_final)

        hook_so, preload_so = build_nam_libs(tc, work)
        check_glibc_compat(tc, work / "rootfs.bin", [hook_so, preload_so])

        launcher = build_launcher_script((work / "evil_script_orig.sh").read_text(), gonk_env=gonk_env)
        (work / "evil_script.sh").write_text(launcher)

        rootfs_patched = work / "rootfs.patched.bin"
        shutil.copyfile(work / "rootfs.bin", rootfs_patched)

        def inject(inner_path, local_path, mode="0100755"):
            sh([tc.debugfs, "-w", "-R", f"rm {inner_path}", str(rootfs_patched)], check=False)
            sh([tc.debugfs, "-w", "-R", f"write {local_path} {inner_path}", str(rootfs_patched)])
            sh([tc.debugfs, "-w", "-R", f"sif {inner_path} mode {mode}", str(rootfs_patched)])

        inject("/usr/Evil/Evil", evil_final)
        inject("/usr/Evil/libnam_hook.so", hook_so)
        inject("/usr/Evil/libnam_preload.so", preload_so)
        inject("/usr/Evil/Scripts/evil", work / "evil_script.sh")

        result = sh([tc.e2fsck, "-fn", str(rootfs_patched)], capture_output=True, text=True, check=False)
        print(result.stdout)
        if result.returncode != 0:
            print(result.stderr)
            die(f"e2fsck -fn exited {result.returncode} on the patched rootfs -- refusing to package it")

        rootfs_patched_xz = work / "rootfs.patched.xz"
        with open(rootfs_patched_xz, "wb") as f:
            sh([tc.xz, "-9", "-T0", "--check=crc32", "-c", str(rootfs_patched)], stdout=f)

        its_text = fit_image.build_its(metadata, "splash.xz", "recoverysplash.xz", "rootfs.patched.xz")
        (work / "update.its").write_text(its_text)
        sh([tc.mkimage, "-f", str(work / "update.its"), str(work / "Update_new.img")], cwd=work)

        # ---- round-trip verification ----
        out_data = (work / "Update_new.img").read_bytes()
        out_props = fit_image.parse_fdt(out_data)
        verify_rootfs_xz = fit_image.read_prop_bytes(out_data, out_props, "//images/rootfs", "data")
        (work / "verify_rootfs.xz").write_bytes(verify_rootfs_xz)
        with open(work / "verify_rootfs.bin", "wb") as f:
            sh([tc.xz, "-d", "-k", "-T0", "-c", str(work / "verify_rootfs.xz")], stdout=f)
        vresult = sh([tc.e2fsck, "-fn", str(work / "verify_rootfs.bin")], capture_output=True, text=True, check=False)
        if vresult.returncode != 0:
            print(vresult.stdout, vresult.stderr)
            die(f"e2fsck -fn on the REPACKED image's rootfs exited {vresult.returncode}")
        sh([tc.debugfs, "-R", f"dump /usr/Evil/Evil {work / 'Evil_verify'}", str(work / "verify_rootfs.bin")])
        cmp_result = sh(["cmp", str(work / "Evil_verify"), str(evil_final)], check=False)
        if cmp_result.returncode != 0:
            die("round-trip verification FAILED: repacked Update.img's Evil binary doesn't match what was built")
        print("OK  round-trip verified: repacked image's /usr/Evil/Evil is byte-exact")

        args.output_img.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(work / "Update_new.img", args.output_img)
        print(f"\nOK  wrote {args.output_img}")
        print("    Anxiety OD (v1) process() NAM hijack applied (see patch_gonkulator.py) --")
        print("    its knob now selects/scans .nam models instead of its overdrive parameter.")
        print("    Additive \"Neural Amp Modeler\" pedal (patch_namloader.py): NOT applied (see README.md).")
        print("    This has NOT been flashed to any device by this script -- see README.md")
        print("    for the (manual, your responsibility) flashing steps and recovery-mode safety net.")

    finally:
        if args.keep_work_dir:
            print(f"--keep-work-dir: build directory left at {work}")
            workdir_ctx._finalizer.detach()  # prevent auto-cleanup on GC
        else:
            workdir_ctx.cleanup()


if __name__ == "__main__":
    main()
