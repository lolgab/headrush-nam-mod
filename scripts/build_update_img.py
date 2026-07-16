#!/usr/bin/env python3
"""
build_update_img.py -- take a stock HeadRush Pedalboard 2.7 Update.img and
produce a modified one with the NAM (Neural Amp Modeler) mod applied:

  1. Additive "Neural Amp Modeler" pedal (own ModFac_construct case, own
     engine/vtables) -- built and QEMU-validated, but NOT yet reachable from
     Evil's own pedal-add menu (see README.md "Known limitation").
  2. Gonkulator ("Ring Mod")-hijack fallback -- the only *reachable* NAM path
     right now. Its 3 knobs are relabeled NAM / Inp / Outp.

Never modifies its input. Always writes a new Update.img. See README.md for
prerequisites (ARM cross toolchain, e2fsprogs, u-boot-tools, the nam_core
submodule) and for what "reachable" vs "dormant" means here.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_image  # noqa: E402


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
    def __init__(self):
        arm_bin = "/opt/homebrew/opt/armv7-unknown-linux-gnueabihf/bin"
        prefix = "armv7-unknown-linux-gnueabihf-"
        self.arm_as = find_tool([f"{arm_bin}/{prefix}as", f"{prefix}as"],
                                 "install via: brew install messense/macos-cross-toolchains/armv7-unknown-linux-gnueabihf")
        self.arm_objcopy = find_tool([f"{arm_bin}/{prefix}objcopy", f"{prefix}objcopy"], "same tap as above")
        self.arm_gxx = find_tool([f"{arm_bin}/{prefix}g++", f"{prefix}g++"], "same tap as above")
        self.arm_strip = find_tool([f"{arm_bin}/{prefix}strip", f"{prefix}strip"], "same tap as above")

        e2fs_sbin = "/opt/homebrew/opt/e2fsprogs/sbin"
        self.debugfs = find_tool([f"{e2fs_sbin}/debugfs", "debugfs"], "install via: brew install e2fsprogs")
        self.e2fsck = find_tool([f"{e2fs_sbin}/e2fsck", "e2fsck"], "install via: brew install e2fsprogs")

        self.mkimage = find_tool(["mkimage"], "install via: brew install u-boot-tools")
        self.xz = find_tool(["xz"], "should be preinstalled; install via: brew install xz")


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
    sh([tc.arm_gxx, "-std=c++20", "-O2", "-fPIC", "-shared", "-march=armv7-a", "-mfpu=neon-vfpv4",
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


def build_launcher_script(stock_script_text, naml_env, gonk_env):
    import re
    global NAM_MOD_BLOCK_RE
    if NAM_MOD_BLOCK_RE is None:
        NAM_MOD_BLOCK_RE = re.compile(r"# --- NAM mod ---.*?# --- end NAM mod ---\n", re.DOTALL)

    block = (
        "# --- NAM mod ---\n"
        "# Additive \"Neural Amp Modeler\" pedal (case 92 in ModFac_construct, own\n"
        "# engine/vtables). Not yet reachable from Evil's own menu/DB (needs real\n"
        "# hardware to trace the name-array/type-string reader) -- these hooks are\n"
        "# wired and QEMU-validated but currently dormant.\n"
        "export LD_PRELOAD=/usr/Evil/libnam_preload.so\n"
        f"export NAM_HOOK_SLOT_NAML_ADDR={naml_env['NAM_HOOK_SLOT_NAML_ADDR']}\n"
        f"export NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR={naml_env['NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR']}\n"
        f"export NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR={naml_env['NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR']}\n"
        "# Gonkulator (\"Ring Mod\") hijack -- the only *reachable* NAM path until\n"
        "# the menu/DB integration above is solved. Knobs relabeled NAM/Inp/Outp.\n"
        f"export NAM_HOOK_SLOT_GONK_ADDR={gonk_env['NAM_HOOK_SLOT_GONK_ADDR']}\n"
        "# --- end NAM mod ---\n"
    )

    if NAM_MOD_BLOCK_RE.search(stock_script_text):
        return NAM_MOD_BLOCK_RE.sub(block, stock_script_text)

    marker = "while [ 1 ]"
    idx = stock_script_text.index(marker)
    return stock_script_text[:idx] + block + "\n" + stock_script_text[idx:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input_img", type=Path, help="stock HeadRush Pedalboard 2.7 Update.img")
    ap.add_argument("output_img", type=Path, help="path to write the modified Update.img")
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

        with open(work / "rootfs.bin", "wb") as f:
            sh([tc.xz, "-d", "-k", "-T0", "-c", str(work / "rootfs_orig.xz")], stdout=f)

        sh([tc.debugfs, "-R", f"dump /usr/Evil/Evil {work / 'Evil'}", str(work / "rootfs.bin")])
        # NOTE: "dump", not "cat" -- debugfs's "cat" interleaves its own banner
        # text with the file's stdout, which would corrupt the extracted script.
        sh([tc.debugfs, "-R", f"dump /usr/Evil/Scripts/evil {work / 'evil_script_orig.sh'}", str(work / "rootfs.bin")])

        for name in ("trampoline_naml.S", "case92_stub.S", "trampoline_trim.S", "trampoline_gonk.S"):
            assemble(tc, REPO_ROOT / "patch" / name, work / (Path(name).stem + ".bin"), work)

        evil_naml = work / "Evil.naml"
        sh([sys.executable, str(REPO_ROOT / "patch" / "patch_namloader.py"),
            str(work / "Evil"), str(work / "trampoline_naml.bin"), str(work / "case92_stub.bin"),
            str(work / "trampoline_trim.bin"), str(evil_naml)])
        naml_env = json.loads((work / (evil_naml.name + ".json")).read_text())

        evil_gonk = work / "Evil.naml_gonk"
        sh([sys.executable, str(REPO_ROOT / "patch" / "patch_gonkulator.py"),
            str(evil_naml), str(work / "trampoline_gonk.bin"), str(evil_gonk)])
        gonk_env = json.loads((work / (evil_gonk.name + ".json")).read_text())

        evil_final = work / "Evil.final"
        sh([sys.executable, str(REPO_ROOT / "patch" / "patch_knob_labels.py"),
            str(evil_gonk), str(evil_final)])

        hook_so, preload_so = build_nam_libs(tc, work)

        launcher = build_launcher_script((work / "evil_script_orig.sh").read_text(), naml_env, gonk_env)
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
        print("    Gonkulator (\"Ring Mod\") NAM hijack: REACHABLE today, knobs relabeled NAM/Inp/Outp.")
        print("    Additive \"Neural Amp Modeler\" pedal: built in, DORMANT (see README.md).")
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
