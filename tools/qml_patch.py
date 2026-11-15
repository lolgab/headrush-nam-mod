#!/usr/bin/env python3
"""qml_patch -- replace text inside one QML source embedded in the ARM32 `Evil` and make Qt use that source.

Evil keeps every QML file twice: the source in its qrc, and a qmlcachegen unit that wins at load time. The units are
found through a hash keyed by static UTF-16 QStrings ("/Pages/BlockDetails.qml", ...). Changing the last character
of the key makes the lookup miss, so Qt compiles the source instead. The qrc is not rebuilt, so an edited source
has to fit in the old slot: plain entries are padded with spaces, zlib entries (qCompress) are recompressed (comment
lines and blank lines are dropped first if the new text does not fit) and the tail is zero padded.

  qml_patch.py extract <Evil> <outdir>
  qml_patch.py patch <Evil> <out.regions> --key /Pages/BlockDetails.qml --old '<text>' --new '<text>' [--old/--new ...]
  qml_patch.py patch <Evil> <out.regions> --key <key> --old '<marker text>' --whole <new source file>
  qml_patch.py apply <regions.json>        (device: push regions, build /tmp/Evil.<ts>, bind-mount, restart)

`patch` writes <out.regions>.json (list of {offset, file}) next to the binary region files; `apply` needs hrdev.
"""
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.join(HERE, ".venv", "bin", "python")
HRDEV = os.path.join(HERE, "hrdev.py")


def entries(d):
    """Yield (offset_of_length_field, compressed, text) for every QML source in the qrc data."""
    for m in re.finditer(rb"\x78[\x9c\xda\x01\x5e]", d):
        o = m.start()
        if o < 8:
            continue
        L, ulen = struct.unpack(">II", d[o - 8:o])
        if not (0 < ulen < 400000 and 4 < L < 400000):
            continue
        try:
            dec = zlib.decompressobj().decompress(d[o:o + L - 4])
            t = dec.decode("utf-8")
        except Exception:
            continue
        if len(dec) == ulen and "{" in t and ("import" in t or "Item" in t or "Rectangle" in t):
            yield o - 8, True, t
    for m in re.finditer(rb"import ", d):
        o = m.start()
        if o < 4:
            continue
        L = struct.unpack(">I", d[o - 4:o])[0]
        if not (20 < L < 400000):
            continue
        try:
            t = d[o:o + L].decode("utf-8")
        except Exception:
            continue
        if "{" in t:
            yield o - 4, False, t


def find_key(d, key):
    pat = b"\xff\xff\xff\xff" + bytes([len(key)]) + b"\x00\x00\x00\x00\x00\x00\x00\x10\x00\x00\x00" + key.encode("utf-16le")
    i = d.find(pat)
    if i < 0 or d.find(pat, i + 1) >= 0:
        raise SystemExit("key %s: %s" % (key, "not found" if i < 0 else "ambiguous"))
    return i + 16 + 2 * (len(key) - 1)


def minify(t):
    t = re.sub(r"(?m)^[ \t]*//.*\n", "", t)
    t = re.sub(r"(?m)[ \t]+$", "", t)
    return re.sub(r"\n{2,}", "\n", t)


def fit(entry_off, comp, text, d):
    L = struct.unpack(">I", d[entry_off:entry_off + 4])[0]
    if not comp:
        b = text.encode()
        if len(b) > L:
            b = minify(text).encode()
        if len(b) > L:
            raise SystemExit("plain entry too small: %d > %d" % (len(b), L))
        b = b + b" " * (L - len(b))
        return entry_off + 4, b
    room = L - 4
    for cand in (text, minify(text)):
        z = zlib.compress(cand.encode(), 9)
        if len(z) <= room:
            body = struct.pack(">I", len(cand.encode())) + z
            return entry_off + 4, body + b"\x00" * (L - len(body))
    raise SystemExit("compressed entry too small even minified: %d > %d" % (len(z), room))


def cmd_patch(a):
    inp, outbase = a[0], a[1]
    key, olds, news, whole = None, [], [], None
    i = 2
    while i < len(a):
        if a[i] == "--whole":
            whole = a[i + 1]
        elif a[i] == "--key":
            key = a[i + 1]
        elif a[i] == "--old":
            olds.append(a[i + 1])
        elif a[i] == "--new":
            news.append(a[i + 1])
        i += 2
    d = open(inp, "rb").read()
    hits = [(o, c, t) for o, c, t in entries(d) if all(x in t for x in olds)]
    if len(hits) != 1:
        raise SystemExit("expected one entry containing all --old texts, found %d" % len(hits))
    o, c, t = hits[0]
    if whole:  # --old texts only identify the entry; its whole source is replaced by the file
        t = open(whole).read()
    else:
        for x, y in zip(olds, news):
            if t.count(x) != 1:
                raise SystemExit("--old must occur once: %r (%d)" % (x, t.count(x)))
            t = t.replace(x, y)
    off, body = fit(o, c, t, d)
    regions = [(off, body), (find_key(d, key), b"x")]
    os.makedirs(os.path.dirname(os.path.abspath(outbase)) or ".", exist_ok=True)
    meta = []
    for n, (ro, rb) in enumerate(regions):
        fn = "%s.%d.bin" % (outbase, n)
        open(fn, "wb").write(rb)
        meta.append({"offset": ro, "file": os.path.abspath(fn), "size": len(rb)})
    json.dump(meta, open(outbase + ".json", "w"), indent=1)
    print("entry 0x%x (%s), key patch 0x%x, %d bytes in regions" % (o, "zlib" if c else "plain", regions[1][0], sum(len(r[1]) for r in regions)))


def hr(*args, timeout=600):
    r = subprocess.run([PY, HRDEV] + list(args), capture_output=True, text=True, timeout=timeout)
    return (r.stdout + r.stderr).replace("\r", "")


def cmd_apply(a):
    meta = json.load(open(a[0]))
    restart = "--no-restart" not in a
    stamp = int(time.time())
    T = "/tmp/Evil.%d" % stamp
    # every earlier apply left a bind mount on top: drop them all so the copy is the flashed binary, not a patched layer
    print(hr("sh", "while mount | grep -q ' /usr/Evil/Evil '; do umount -l /usr/Evil/Evil || break; done; "
             "rm -f /tmp/Evil.[0-9]* /tmp/qp*.bin; cp /usr/Evil/Evil %s && chmod 755 %s && echo copied" % (T, T)).strip())
    for n, m in enumerate(meta):
        rf = "/tmp/qp%d.bin" % n
        want = hashlib.md5(open(m["file"], "rb").read()).hexdigest()
        hr("push", m["file"], rf)
        if want not in hr("sh", "md5sum %s" % rf):
            raise SystemExit("push of region %d failed (md5 mismatch); run `hrdev stop` and retry" % n)
        hr("sh", "dd if=%s of=%s bs=1 seek=%d conv=notrunc 2>/dev/null" % (rf, T, m["offset"]))
        print("region", n, hex(m["offset"]), m["size"], "ok")
    print(hr("sh", "mount --bind %s /usr/Evil/Evil && echo bound; cmp -l /usr/Evil/Evil %s | wc -l" % (T, T)).strip())
    if restart:
        print(hr("sh", "systemctl restart evil.service; sleep 40; systemctl is-active evil.service").strip())


def cmd_extract(a):
    d = open(a[0], "rb").read()
    os.makedirs(a[1], exist_ok=True)
    n = 0
    for o, c, t in sorted(entries(d)):
        open(os.path.join(a[1], "%04d_%s_%x.qml" % (n, "z" if c else "p", o)), "w").write(t)
        n += 1
    print(n, "sources")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    {"patch": cmd_patch, "apply": cmd_apply, "extract": cmd_extract}[sys.argv[1]](sys.argv[2:])
