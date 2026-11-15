#!/usr/bin/env python3
"""evil_xref -- tiny static-analysis helper for the stock (or patched) ARM32 `Evil` binary, no Ghidra needed.

  evil_xref.py <Evil> callers <vaddr>          every `bl`/`b` that targets vaddr (hex)
  evil_xref.py <Evil> dis <vaddr> [n=60]       disassemble n instructions at vaddr
  evil_xref.py <Evil> literal-callers <vaddr>  callers whose r1 is NOT set by movw/movt right before the call
                                               (i.e. the format/name argument is computed, not a constant)

Needs capstone (tools/.venv/bin/pip install capstone). Evil's section headers are unusable, so the program headers
are parsed by hand.
"""
import array
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        phoff, = struct.unpack_from("<I", self.data, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", self.data, 0x2A)
        self.segs = []
        for i in range(phnum):
            t, off, va, _pa, fs, _ms, fl, _al = struct.unpack_from("<8I", self.data, phoff + i * phentsize)
            if t == 1:
                self.segs.append((va, off, fs, fl))

    def rd(self, va, n):
        for v, o, fs, _fl in self.segs:
            if v <= va < v + fs:
                return self.data[o + va - v:o + va - v + n]
        raise KeyError(hex(va))

    def callers(self, target):
        v, o, fs = next((v, o, fs) for v, o, fs, fl in self.segs if fl & 1)
        w = array.array("I")
        w.frombytes(self.data[o:o + fs - fs % 4])
        out = []
        for i, x in enumerate(w):
            if (x >> 24) in (0xEB, 0xEA):
                imm = x & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 1 << 24
                if v + i * 4 + 8 + imm * 4 == target:
                    out.append(v + i * 4)
        return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    elf, cmd, arg = Elf(sys.argv[1]), sys.argv[2], int(sys.argv[3], 16)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    if cmd == "callers":
        cs = elf.callers(arg)
        print(len(cs), " ".join(hex(c) for c in cs))
    elif cmd == "dis":
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 60
        for i in md.disasm(elf.rd(arg, n * 4), arg):
            print("%x: %s %s" % (i.address, i.mnemonic, i.op_str))
    elif cmd == "literal-callers":
        for c in elf.callers(arg):
            w = None
            for i in reversed(list(md.disasm(elf.rd(c - 64, 64), c - 64))):
                if i.op_str.replace(" ", "").split(",")[0] == "r1" and i.mnemonic in (
                        "movw", "movt", "mov", "ldr", "add", "sub", "movs", "orr", "adds"):
                    w = i
                    break
            if not (w and w.mnemonic in ("movw", "movt")):
                print(hex(c), (w.mnemonic + " " + w.op_str) if w else "?")
    return 0


if __name__ == "__main__":
    sys.exit(main())
