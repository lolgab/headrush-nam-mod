"""
update_img.py -- open a HeadRush Update.img, edit files inside its rootfs, and
repack it.

Shared helper for the MX5 UI mods in this directory. Deliberately standalone
(pure Python + debugfs/xz/mkimage) so a mod can be built without the NAM
toolchain; it never touches the input file.

Every build script here takes `<input Update.img> <output Update.img>`, so mods
compose by chaining:

    ./build_tuner_tempo.py  stock.img  a.img
    ./build_usb_console.py  a.img      final.img
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_image  # noqa: E402


def _tool(*names):
    for n in names:
        p = shutil.which(n) or (n if Path(n).is_file() else None)
        if p:
            return p
    sys.exit(f"ERROR: none of {names} found -- install e2fsprogs / xz-utils / u-boot-tools")


DEBUGFS = None
XZ = None
MKIMAGE = None
E2FSCK = None


def _init_tools():
    global DEBUGFS, XZ, MKIMAGE, E2FSCK
    DEBUGFS = DEBUGFS or _tool("debugfs", "/usr/sbin/debugfs", "/sbin/debugfs")
    E2FSCK = E2FSCK or _tool("e2fsck", "/usr/sbin/e2fsck", "/sbin/e2fsck")
    XZ = XZ or _tool("xz")
    MKIMAGE = MKIMAGE or _tool("mkimage")


def _run(cmd, **kw):
    kw.setdefault("capture_output", True)
    kw.setdefault("text", True)
    return subprocess.run(cmd, **kw)


class UpdateImg:
    """Context manager: unpack an Update.img's rootfs, edit it, repack.

    with UpdateImg("stock.img") as img:
        data = img.read("/usr/Evil/Evil")
        img.write("/usr/Evil/Evil", data, mode="0100755")
        img.save("out.img")
    """

    def __init__(self, in_path):
        _init_tools()
        self.in_path = Path(in_path)
        if not self.in_path.is_file():
            sys.exit(f"ERROR: {in_path} not found")
        self._tmp = None
        self.work = None
        self.rootfs = None
        self.metadata = None

    def __enter__(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="mx5-uimod-")
        self.work = Path(self._tmp.name)
        data = self.in_path.read_bytes()
        props = fit_image.parse_fdt(data)
        self.metadata = fit_image.read_root_metadata(data, props)
        for name, node in (("splash", "//images/splash"),
                           ("recoverysplash", "//images/recoverysplash"),
                           ("rootfs", "//images/rootfs")):
            (self.work / f"{name}.xz").write_bytes(
                fit_image.read_prop_bytes(data, props, node, "data"))
        self.rootfs = self.work / "rootfs.bin"
        with open(self.rootfs, "wb") as f:
            subprocess.run([XZ, "-d", "-k", "-T0", "-c", str(self.work / "rootfs.xz")],
                           stdout=f, check=True)
        return self

    def __exit__(self, *exc):
        self._tmp.cleanup()
        return False

    # ---- rootfs file operations -------------------------------------------
    def read(self, inner_path):
        """Return the bytes of a file inside the rootfs."""
        out = self.work / "._read_tmp"
        _run([DEBUGFS, "-R", f"dump {inner_path} {out}", str(self.rootfs)])
        if not out.exists():
            raise FileNotFoundError(inner_path)
        data = out.read_bytes()
        out.unlink()
        return data

    def write(self, inner_path, data, mode="0100644"):
        """Create/replace a file inside the rootfs."""
        tmp = self.work / "._write_tmp"
        tmp.write_bytes(data if isinstance(data, (bytes, bytearray)) else data.encode())
        _run([DEBUGFS, "-w", "-R", f"rm {inner_path}", str(self.rootfs)])
        r = _run([DEBUGFS, "-w", "-R", f"write {tmp} {inner_path}", str(self.rootfs)])
        if "Allocated inode" not in (r.stdout or "") + (r.stderr or ""):
            raise RuntimeError(f"debugfs write failed for {inner_path}: {r.stdout}{r.stderr}")
        _run([DEBUGFS, "-w", "-R", f"sif {inner_path} mode {mode}", str(self.rootfs)])
        tmp.unlink()

    def symlink(self, inner_path, target):
        """Create a symlink inside the rootfs (used to enable systemd units)."""
        _run([DEBUGFS, "-w", "-R", f"rm {inner_path}", str(self.rootfs)])
        _run([DEBUGFS, "-w", "-R", f"symlink {inner_path} {target}", str(self.rootfs)])

    def exists(self, inner_path):
        r = _run([DEBUGFS, "-R", f"stat {inner_path}", str(self.rootfs)])
        return "Inode:" in (r.stdout or "")

    # ---- repack ------------------------------------------------------------
    def save(self, out_path):
        r = _run([E2FSCK, "-fn", str(self.rootfs)])
        if r.returncode != 0:
            sys.exit(f"ERROR: e2fsck rejected the patched rootfs:\n{r.stdout}{r.stderr}")
        packed = self.work / "rootfs.patched.xz"
        with open(packed, "wb") as f:
            subprocess.run([XZ, "-9", "-T0", "--check=crc32", "-c", str(self.rootfs)],
                           stdout=f, check=True)
        its = fit_image.build_its(self.metadata, "splash.xz", "recoverysplash.xz",
                                  "rootfs.patched.xz")
        (self.work / "update.its").write_text(its)
        new = self.work / "Update_new.img"
        r = _run([MKIMAGE, "-f", str(self.work / "update.its"), str(new)], cwd=self.work)
        if not new.exists():
            sys.exit(f"ERROR: mkimage failed:\n{r.stdout}{r.stderr}")
        out_path = Path(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(new, out_path)
        print(f"OK  wrote {out_path} ({out_path.stat().st_size} bytes)")
