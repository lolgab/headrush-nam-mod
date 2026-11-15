#!/usr/bin/env python3
"""hrdev -- drive a HeadRush Pedalboard that runs the USB-console mod from the Mac, no hands on the device.

The console mod exposes a root BusyBox shell on a USB CDC port (/dev/cu.usbmodemNNN). macOS hangs on the second
open() of that port, so a small daemon keeps it open and every other command talks to the daemon through a Unix
socket (the daemon is started on demand). Needs pyserial:  python3 -m venv tools/.venv && tools/.venv/bin/pip
install pyserial   (then run this script with tools/.venv/bin/python).

  hrdev.py sh  <command...>            run a shell command on the device, print its output
  hrdev.py push <local> <remote>       copy a file to the device (gzip + base64 over the serial line)
  hrdev.py pull <remote> <local>       copy a file from the device
  hrdev.py deploy [lib]                hot-deploy libnam_hook.so (default blobs/libnam_hook.so): bind-mount it over
                                       /usr/Evil/libnam_hook.so and restart Evil. No flash; a reboot reverts it.
  hrdev.py ctl <line>...               send reverb dev-control commands, print /tmp/nam_dev.out afterwards
                                       (algo / knob / inject / capture / dump / reset / stat, see patch/nam_hook.cpp)
  hrdev.py capture <secs> <out.wav>    record the reverb output on the device and pull the WAV
  hrdev.py journal [grep-pattern]      last journal lines of this boot (default: our [nam_*] lines)
  hrdev.py restart                     restart Evil (the supervisor brings it back)
  hrdev.py status | stop | ports
"""
import base64
import glob
import gzip
import hashlib
import os
import socket
import subprocess
import sys
import threading
import time

SOCK = "/tmp/hrdev.sock"
CHUNK = 900  # characters per printf; the tty line buffer is 4096 but busybox ash is happier with short lines


# ---------------------------------------------------------------------------------------------------- daemon side
class Shell:
    def __init__(self, port):
        import serial  # imported here so the client side works without pyserial

        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.1, write_timeout=5)
        self.lock = threading.Lock()
        self.s.write(b"\x03\x03\r\n")
        time.sleep(0.6)
        self.s.read(65536)
        self.run("stty -echo; PS1=; PS2=; echo ready", 5)  # no echo, no prompt text mixed into command output

    def run(self, cmd, timeout=30):
        """Runs cmd under the shell; returns (exit status, output)."""
        with self.lock:
            tag = "@@END%d@@" % int(time.time() * 1000)
            self.s.read(65536)
            self.s.write(("%s\necho %s$?\n" % (cmd, tag)).encode())
            buf = b""
            end = time.time() + timeout
            while time.time() < end:
                buf += self.s.read(65536)
                i = buf.find(tag.encode())
                if i >= 0 and b"\n" in buf[i:]:
                    out = buf[:i].decode(errors="replace").replace("\r", "")
                    rc = buf[i + len(tag):].split(b"\n")[0].strip()
                    return (int(rc) if rc.isdigit() else -1), out
            return -2, buf.decode(errors="replace").replace("\r", "") + "\n[hrdev: timeout]"

    def push(self, data, remote):
        enc = base64.b64encode(gzip.compress(data, 9)).decode()
        self.run("rm -f /tmp/hrdev.b64")
        for i in range(0, len(enc), CHUNK):
            rc, out = self.run("printf %%s '%s' >> /tmp/hrdev.b64" % enc[i:i + CHUNK], 10)
            if rc != 0:
                return rc, out
        return self.run("base64 -d /tmp/hrdev.b64 | gunzip > '%s.part' && mv '%s.part' '%s' && md5sum '%s'"
                        % (remote, remote, remote, remote), 60)

    def pull(self, remote):
        rc, out = self.run("gzip -c '%s' | base64" % remote, 300)
        if rc != 0:
            return rc, out.encode()
        return 0, gzip.decompress(base64.b64decode("".join(out.split())))


def serve(port):
    sh = Shell(port)
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(SOCK)
    srv.listen(4)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=handle, args=(sh, c), daemon=True).start()


def recv_msg(c):
    head = b""
    while not head.endswith(b"\n"):
        d = c.recv(1)
        if not d:
            return None
        head += d
    op, n = head.decode().split(" ", 1)[0], int(head.decode().split(" ", 1)[1])
    body = b""
    while len(body) < n:
        d = c.recv(min(65536, n - len(body)))
        if not d:
            break
        body += d
    return op, body


def send_msg(c, op, body):
    c.sendall(("%s %d\n" % (op, len(body))).encode() + body)


def handle(sh, c):
    try:
        op, body = recv_msg(c)
        if op == "sh":
            rc, out = sh.run(body.decode(), 120)
            send_msg(c, "rc%d" % rc, out.encode())
        elif op == "push":
            remote, data = body.split(b"\0", 1)
            rc, out = sh.push(data, remote.decode())
            send_msg(c, "rc%d" % rc, out.encode())
        elif op == "pull":
            rc, data = sh.pull(body.decode())
            send_msg(c, "rc%d" % rc, data)
        elif op == "ping":
            send_msg(c, "rc0", sh.port.encode())
        elif op == "stop":
            send_msg(c, "rc0", b"bye")
            os._exit(0)
    except Exception as e:  # keep the daemon alive
        try:
            send_msg(c, "rc-3", repr(e).encode())
        except Exception:
            pass
    finally:
        c.close()


# ---------------------------------------------------------------------------------------------------- client side
def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def request(op, body=b"", timeout=400):
    c = socket.socket(socket.AF_UNIX)
    c.settimeout(timeout)
    c.connect(SOCK)
    send_msg(c, op, body)
    r = recv_msg(c)
    c.close()
    return int(r[0][2:]), r[1]


def ensure_daemon(port=None):
    try:
        request("ping", timeout=5)
        return
    except (OSError, TypeError):
        pass
    cands = [os.environ['HRDEV_PORT']] if os.environ.get('HRDEV_PORT') else ([port] if port else ports())
    if not cands:
        sys.exit("hrdev: no /dev/cu.usbmodem* port. Is the Pedalboard connected and running the USB-console mod?")
    # two CDC ports may enumerate; the shell is on the first one that answers
    log = open("/tmp/hrdev.log", "w")
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "_serve", cands[0]], stdout=log, stderr=log,
                     start_new_session=True)
    for _ in range(100):
        time.sleep(0.2)
        try:
            request("ping", timeout=5)
            return
        except (OSError, TypeError):
            continue
    sys.exit("hrdev: daemon did not come up, see /tmp/hrdev.log")


def sh(cmd, timeout=120):
    rc, out = request("sh", cmd.encode(), timeout)
    return rc, out.decode(errors="replace")


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if a[0] == "_serve":
        serve(a[1])
        return 0
    if a[0] == "ports":
        print("\n".join(ports()) or "none")
        return 0
    if a[0] == "stop":
        try:
            request("stop", timeout=5)
        except OSError:
            pass
        return 0
    ensure_daemon()
    if a[0] == "status":
        print("port", request("ping")[1].decode())
        rc, out = sh("uname -a; pgrep -f 'Evil/Evil$' | head -3; ls -la /usr/Evil/libnam_hook.so; "
                     "md5sum /usr/Evil/libnam_hook.so; mount | grep nam_hook; cat /tmp/nam_dev.out 2>/dev/null")
        print(out)
    elif a[0] == "sh":
        rc, out = sh(" ".join(a[1:]))
        print(out, end="")
        return max(rc, 0) if rc != -2 else 124
    elif a[0] == "push":
        rc, out = request("push", a[2].encode() + b"\0" + open(a[1], "rb").read())
        print(out.decode(), end="")
        return 0 if rc == 0 else 1
    elif a[0] == "pull":
        rc, data = request("pull", a[1].encode())
        if rc != 0:
            print(data.decode(errors="replace"))
            return 1
        open(a[2], "wb").write(data)
        print("%d bytes -> %s" % (len(data), a[2]))
    elif a[0] == "deploy":
        lib = a[1] if len(a) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blobs",
                                                  "libnam_hook.so")
        raw = open(lib, "rb").read()
        h = hashlib.md5(raw).hexdigest()[:8]
        remote = "/tmp/lib_%s.so" % h
        t0 = time.time()
        rc, out = request("push", remote.encode() + b"\0" + raw)
        print(out.decode(), end="")
        if rc != 0:
            return 1
        rc, out = sh("umount -l /usr/Evil/libnam_hook.so 2>/dev/null; umount -l /usr/Evil/libnam_hook.so 2>/dev/null; "
                     "mount --bind %s /usr/Evil/libnam_hook.so && md5sum /usr/Evil/libnam_hook.so" % remote)
        print(out, end="")
        restart()
        print("deployed %s in %.1f s" % (h, time.time() - t0))
    elif a[0] == "restart":
        restart()
    elif a[0] == "ctl":
        return ctl(a[1:])
    elif a[0] == "capture":
        secs = float(a[1])
        sh("rm -f /tmp/nam_cap.wav")
        print(ctl(["capture %g" % secs], quiet=True) or "", end="")
        time.sleep(secs + 1.0)
        for _ in range(40):
            if "done" in sh("cat /tmp/nam_dev.out")[1].split("capture=")[-1]:
                break
            time.sleep(0.5)
        rc, data = request("pull", b"/tmp/nam_cap.wav")
        open(a[2], "wb").write(data)
        print("%d bytes -> %s" % (len(data), a[2]))
    elif a[0] == "journal":
        pat = a[1] if len(a) > 1 else r"nam_"
        rc, out = sh("journalctl -b --no-pager | grep -a -E '%s' | tail -60" % pat, 60)
        print(out, end="")
    else:
        print(__doc__)
        return 2
    return 0


def restart():
    rc, out = sh("P=$(ps | grep '[E]vil/Evil$' | grep -v inhibit | head -1 | cut -c1-6 | tr -d ' '); "
                 "kill $P; echo killed $P")
    print(out, end="")
    time.sleep(8)
    for _ in range(30):  # wait for Evil to come back
        rc, out = sh("ps | grep -c '[E]vil/Evil$'", 10)
        if out.strip().split("\n")[-1].strip() not in ("0", ""):
            break
        time.sleep(1)


def ctl(lines, quiet=False):
    text = "\n".join(lines) + "\n"
    rc, out = sh("rm -f /tmp/nam_dev.out; printf '%%s\\n' '%s' > /tmp/nam_ctl.tmp && mv /tmp/nam_ctl.tmp /tmp/nam_ctl"
                 % text.replace("'", "").rstrip("\n").replace("\n", "' '"))
    for _ in range(40):
        rc, out = sh("cat /tmp/nam_dev.out 2>/dev/null")
        if out.strip():
            if not quiet:
                print(out, end="")
            return 0
        time.sleep(0.25)
    print("hrdev: no answer from the hook (no Reverb engine yet, or an old libnam_hook.so?)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
