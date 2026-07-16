#!/usr/bin/env python3
"""
patch_gonkulator.py -- repoint Gonkulator engine object's process() vtable slot
                       at an injected trampoline running NAM inference instead.

Reverse-engineered facts (Evil 2.7, session notes SESSION_NOTES.md):
  - Gonkulator's plain engine object (no RTTI, -fno-rtti) has its real vtable
    at vaddr 0x182c878 (found via double-indirection *(0x182dc14+4)).
  - Slot index 39 (file_offset 0x1824914, vaddr 0x182c914) holds 0x2e7910,
    the real per-sample audio process() function. Confirmed via PyGhidra
    decompile (not just static disasm guessing):
        void process(EngineObj* this, uint32_t param2, float** input,
                     uint32_t numChannels, float** output, int32_t numFrames,
                     uint32_t* flags, void* ctx)
    input/output are per-channel float* arrays; numChannels is 1 or 2.
  - This is a REPLACE (either/or) hook, same pattern as the (superseded)
    IRLoader hook: trampoline checks a mutable hook_slot; if set, tail-jumps
    there (identical signature, so args pass through unchanged); else falls
    back to the original 0x2e7910. Unlike the IRLoader trampoline, this one
    uses r12 (ip) as scratch instead of r1, because r1 here is a REAL
    argument (param2), not free scratch space.

This patches a COPY of Evil (never the original). Sanity-checks the vtable
slot currently holds 0x2e7910 before touching anything.
"""
import json
import struct
import sys

PAGE = 0x1000

GONK_VTABLE_SLOT_FILE_OFFSET = 0x1824914
EXPECTED_ORIG_FN = 0x2e7910


def round_up(x, align):
    return (x + align - 1) & ~(align - 1)


def parse_ehdr(data):
    fields = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    ident = fields[0]
    assert ident[:4] == b"\x7fELF", "not an ELF file"
    assert ident[4] == 1, "not 32-bit ELF"
    assert ident[5] == 1, "not little-endian ELF"
    return {
        "e_type": fields[1], "e_machine": fields[2], "e_phoff": fields[5],
        "e_phentsize": fields[9], "e_phnum": fields[10],
    }


def parse_phdrs(data, ehdr):
    phdrs = []
    off = ehdr["e_phoff"]
    for i in range(ehdr["e_phnum"]):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", data, off + i * ehdr["e_phentsize"])
        phdrs.append({
            "p_type": p_type, "p_offset": p_offset, "p_vaddr": p_vaddr,
            "p_paddr": p_paddr, "p_filesz": p_filesz, "p_memsz": p_memsz,
            "p_flags": p_flags, "p_align": p_align,
        })
    return phdrs


def pack_phdr(p):
    return struct.pack("<IIIIIIII", p["p_type"], p["p_offset"], p["p_vaddr"],
                        p["p_paddr"], p["p_filesz"], p["p_memsz"],
                        p["p_flags"], p["p_align"])


PT_LOAD = 1
PT_PHDR = 6
PF_X, PF_W, PF_R = 1, 2, 4


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <Evil-in> <trampoline_gonk.bin> <Evil-out>", file=sys.stderr)
        sys.exit(1)

    in_path, tramp_path, out_path = sys.argv[1:4]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())
    with open(tramp_path, "rb") as f:
        tramp = bytearray(f.read())

    assert len(tramp) == 32, f"expected 32-byte trampoline, got {len(tramp)}"

    ehdr = parse_ehdr(data)
    assert ehdr["e_machine"] == 40, "not ARM (EM_ARM=40)"
    assert ehdr["e_type"] == 2, "expected ET_EXEC (non-PIE)"

    phdrs = parse_phdrs(data, ehdr)

    orig_val = struct.unpack_from("<I", data, GONK_VTABLE_SLOT_FILE_OFFSET)[0]
    if orig_val != EXPECTED_ORIG_FN:
        print(f"REFUSING: Gonkulator vtable slot @ 0x{GONK_VTABLE_SLOT_FILE_OFFSET:x} "
              f"holds 0x{orig_val:x}, expected 0x{EXPECTED_ORIG_FN:x}. Wrong binary / "
              f"already patched / stale offsets -- re-verify before proceeding.",
              file=sys.stderr)
        sys.exit(2)

    max_end_vaddr = max(p["p_vaddr"] + p["p_memsz"] for p in phdrs if p["p_type"] == PT_LOAD)
    new_vaddr_base = round_up(max_end_vaddr, PAGE)

    new_file_offset = round_up(len(data), PAGE)
    assert new_file_offset % PAGE == new_vaddr_base % PAGE == 0

    phdr_table_bytes = (len(phdrs) + 1) * ehdr["e_phentsize"]
    tramp_offset_in_seg = phdr_table_bytes
    hook_slot_offset_in_seg = tramp_offset_in_seg + 28
    seg_total_size = tramp_offset_in_seg + len(tramp)

    tramp_base_vaddr = new_vaddr_base + tramp_offset_in_seg
    hook_slot_addr = new_vaddr_base + hook_slot_offset_in_seg

    struct.pack_into("<I", tramp, 20, hook_slot_addr)
    struct.pack_into("<I", tramp, 24, EXPECTED_ORIG_FN)
    struct.pack_into("<I", tramp, 28, 0)

    new_phdrs_bytes = bytearray()
    for p in phdrs:
        new_phdrs_bytes += pack_phdr(p)
    new_load_entry = {
        "p_type": PT_LOAD, "p_offset": new_file_offset, "p_vaddr": new_vaddr_base,
        "p_paddr": new_vaddr_base, "p_filesz": seg_total_size, "p_memsz": seg_total_size,
        "p_flags": PF_R | PF_W | PF_X, "p_align": PAGE,
    }
    new_phdrs_bytes += pack_phdr(new_load_entry)
    assert len(new_phdrs_bytes) == phdr_table_bytes

    new_segment = bytearray(seg_total_size)
    new_segment[0:phdr_table_bytes] = new_phdrs_bytes
    new_segment[tramp_offset_in_seg:tramp_offset_in_seg + len(tramp)] = tramp

    for i, p in enumerate(phdrs):
        if p["p_type"] == PT_PHDR:
            phdrs[i].update(p_offset=new_file_offset, p_vaddr=new_vaddr_base,
                            p_paddr=new_vaddr_base, p_filesz=phdr_table_bytes,
                            p_memsz=phdr_table_bytes)
            new_phdrs_bytes = bytearray()
            for pp in phdrs:
                new_phdrs_bytes += pack_phdr(pp)
            new_phdrs_bytes += pack_phdr(new_load_entry)
            new_segment[0:phdr_table_bytes] = new_phdrs_bytes
            break

    if len(data) < new_file_offset:
        data += b"\x00" * (new_file_offset - len(data))
    data += new_segment

    struct.pack_into("<I", data, 28, new_file_offset)
    struct.pack_into("<H", data, 44, ehdr["e_phnum"] + 1)

    struct.pack_into("<I", data, GONK_VTABLE_SLOT_FILE_OFFSET, tramp_base_vaddr)

    with open(out_path, "wb") as f:
        f.write(data)

    print(f"OK  wrote {out_path}")
    print(f"    new segment vaddr=0x{new_vaddr_base:x} file_off=0x{new_file_offset:x} size=0x{seg_total_size:x}")
    print(f"    trampoline @ 0x{tramp_base_vaddr:x}  (Gonkulator engine vtable[39] now points here)")
    print(f"    hook_slot  @ 0x{hook_slot_addr:x}  <-- pass to nam_preload as NAM_HOOK_SLOT_GONK_ADDR")
    print(f"    fallback (orig process()) still reachable @ 0x{EXPECTED_ORIG_FN:x}")

    sidecar = {"NAM_HOOK_SLOT_GONK_ADDR": hex(hook_slot_addr)}
    with open(out_path + ".json", "w") as f:
        json.dump(sidecar, f, indent=2)
    print(f"OK  wrote {out_path}.json (hook-slot env var for the launcher script)")


if __name__ == "__main__":
    main()
