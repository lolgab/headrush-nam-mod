#!/usr/bin/env python3
"""
patch_modfac_spy.py -- pure-observer spy on ModFac_construct's pedal-type
dispatch, logging which type index gets constructed without changing
behavior at all.

STATUS: NOT applied by build_update_img.py -- dormant, kept as reference.
The dispatch site (vaddr 0x209be4) sits deep inside a ~22MB .text section
with no code cave anywhere near it. ARM's B/BL instruction (the only
single-instruction way to replace the one 4-byte `cmp r1,#0x5b` slot we're
allowed to touch and still reach arbitrary code) has a +-32MB range;
patch_gonkulator.py's reclaimed inter-segment gap is ~51MB away -- too far.
The only region close enough AND genuinely unmapped (vaddr 0x2000-0x7fff,
the same hole patch_namloader.py uses) requires adding a new PT_LOAD
segment to use, which is the exact mechanism already confirmed (see
patch_gonkulator.py's docstring) to brick real-hardware boot. Left here in
case a future session finds a legitimate nearby gap (a section-boundary
alignment gap big enough for ~48 bytes, or a different call-target-hijack
approach instead of patching the dispatch instruction itself).

Context: the additive (non-hijacking) NAM pedal design in patch_namloader.py
has always been blocked by one missing fact -- "nothing in Evil's own UI
knows how to construct it from the pedal-add menu or a saved preset yet",
and finding that translation was believed to need a real device with
root/UART access and a debugger (breakpoint at the factory function, load a
preset, read the backtrace). This script gets the same information a
different way, with no UART needed at all: it patches the single
`cmp r1, #0x5b` dispatch instruction inside ModFac_construct (vaddr
0x209be4, documented in patch_namloader.py) to first log r1 -- the pedal
type index about to be constructed -- via a hook_slot-resolved C function,
then ALWAYS re-executes the original comparison and resumes the real,
untouched dispatch at 0x209be8. Cases 0-91 and the out-of-range default
path are bit-for-bit unaffected; this changes nothing observable, it only
observes. Add pedals one at a time from the UI menu and check the log
afterward to build a name->index mapping, including (eventually) whatever
index a saved preset uses for a NAM pedal type if one is ever added to the
menu, or to confirm which existing index the "Ring Mod"/"Ring Mod 2"
pedals actually use.

Chains after patch_gonkulator.py's output (operates on Evil.gonk, not raw
Evil): reuses the SAME reclaimed inter-segment gap that script grows the
R+E code segment's filesz/memsz to cover (~3420 bytes; the gonk trampoline
uses the first 28, leaving ~3392 free). No new PT_LOAD segment, no further
phdr growth, no file size change -- same discipline as patch_gonkulator.py,
for the same reason (a new segment/phdr relocation was confirmed to brick
real-hardware boot; see that script's docstring).
"""
import json
import struct
import sys

DISPATCH_PATCH_ADDR = 0x209be4          # the `cmp r1,#0x5b` we branch away from (never removed)
RESUME_ADDR = 0x209be8                  # right after it -- real, untouched dispatch continues here
EXPECTED_DISPATCH_WORD = 0xE351005B     # cmp r1, #0x5b

PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4
PHDR_OFF_FILESZ = 4 * 4
PHDR_OFF_MEMSZ = 5 * 4

SPY_CODE_LEN = 0x2c   # trampoline_modfac_spy.S bytes 0:0x2c -- instructions + the two literals
SPY_SEARCH_WINDOW = 4096  # comfortably covers the known ~3420-byte reclaimed gap


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


def encode_b(from_vaddr, to_vaddr):
    pc = from_vaddr + 8
    delta = to_vaddr - pc
    assert delta % 4 == 0, "branch target not word-aligned relative to source"
    imm24 = (delta >> 2) & 0xFFFFFF
    signed = imm24 if imm24 < (1 << 23) else imm24 - (1 << 24)
    assert signed * 4 == delta, "branch out of +-32MB range"
    word = (0xE << 28) | (0b101 << 25) | imm24
    return struct.pack("<I", word)


def find_free_tail_run(data, code_seg, need_len, search_window):
    """Find the first zero run of >= need_len bytes within the last
    search_window bytes of code_seg's CURRENT file range. Safe here
    specifically because this tail is patch_gonkulator.py's own reclaimed
    (verified-zero) gap, not a pre-existing real section -- unlike a blind
    scan of a segment's full range (which once found a false positive
    inside .hash's legitimate empty-bucket zero runs), we already know this
    exact region has no other owner."""
    # Word-aligned (4-byte-stride) scan, not a raw byte scan: a byte-level
    # scan can find a "run" that starts mid-instruction, spanning across an
    # encoding's own trailing zero bytes and a literal's leading zero bytes
    # without that position ever being a real, addressable instruction
    # boundary (confirmed the hard way -- encode_b() then rejects the
    # result as not word-aligned relative to the branch source). ARM code
    # must live at a 4-byte-aligned address, so only consider aligned starts.
    seg_end = code_seg["p_offset"] + code_seg["p_filesz"]
    search_start = max(code_seg["p_offset"], seg_end - search_window)
    search_start -= search_start % 4
    need_words = (need_len + 3) // 4
    for word_off in range(search_start, seg_end - need_len + 1, 4):
        if all(b == 0 for b in data[word_off:word_off + need_words * 4]):
            return word_off
    return None


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <Evil.gonk-in> <trampoline_modfac_spy.bin> <Evil-out>",
              file=sys.stderr)
        sys.exit(1)

    in_path, tramp_path, out_path = sys.argv[1:4]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())
    with open(tramp_path, "rb") as f:
        tramp = bytearray(f.read())

    assert len(tramp) == 48, f"expected 48-byte spy trampoline, got {len(tramp)}"

    ehdr = parse_ehdr(data)
    assert ehdr["e_machine"] == 40, "not ARM (EM_ARM=40)"
    assert ehdr["e_type"] == 2, "expected ET_EXEC (non-PIE)"
    phdrs = parse_phdrs(data, ehdr)

    code_segs = [p for p in phdrs if p["p_type"] == PT_LOAD and p["p_flags"] == (PF_R | PF_X)]
    if len(code_segs) != 1:
        print(f"REFUSING: expected exactly one plain R+E LOAD segment, found {len(code_segs)} "
              f"-- run this after patch_gonkulator.py, on its output.", file=sys.stderr)
        sys.exit(2)
    code_seg = code_segs[0]
    code_seg_index = phdrs.index(code_seg)

    # This segment's own vaddr<->file-offset delta (0 file_off maps to
    # p_vaddr) -- DISPATCH_PATCH_ADDR/RESUME_ADDR are vaddrs, need converting
    # to file offsets before reading/writing raw file bytes.
    vaddr_delta = code_seg["p_vaddr"] - code_seg["p_offset"]
    dispatch_file_off = DISPATCH_PATCH_ADDR - vaddr_delta

    dispatch_word = struct.unpack_from("<I", data, dispatch_file_off)[0]
    if dispatch_word != EXPECTED_DISPATCH_WORD:
        print(f"REFUSING: ModFac_construct dispatch @ vaddr 0x{DISPATCH_PATCH_ADDR:x} "
              f"(file_off 0x{dispatch_file_off:x}) holds 0x{dispatch_word:x}, expected "
              f"0x{EXPECTED_DISPATCH_WORD:x}. Wrong binary / already patched / stale offsets "
              f"-- re-verify before proceeding.", file=sys.stderr)
        sys.exit(2)

    cave_off = find_free_tail_run(data, code_seg, SPY_CODE_LEN + 4, SPY_SEARCH_WINDOW)
    if cave_off is None:
        print(f"REFUSING: no >= {SPY_CODE_LEN + 4} byte free run found in the reclaimed gap "
              f"-- binary layout changed or gap already fully used, re-verify before proceeding.",
              file=sys.stderr)
        sys.exit(2)

    tramp_base_vaddr = code_seg["p_vaddr"] + (cave_off - code_seg["p_offset"])

    # Mutable hook_slot: same technique as patch_gonkulator.py -- grow the
    # R+W data segment's memsz by one more word, right past whatever it
    # currently ends at (which already includes patch_gonkulator.py's own
    # +4 growth for its own hook_slot, if this runs after that script).
    data_segs = [p for p in phdrs if p["p_type"] == PT_LOAD and p["p_flags"] == (PF_R | PF_W)]
    if len(data_segs) != 1:
        print(f"REFUSING: expected exactly one plain R+W LOAD segment, found {len(data_segs)}.",
              file=sys.stderr)
        sys.exit(2)
    data_seg = data_segs[0]
    data_seg_index = phdrs.index(data_seg)

    hook_slot_addr = data_seg["p_vaddr"] + data_seg["p_memsz"]
    new_data_memsz = data_seg["p_memsz"] + 4

    struct.pack_into("<I", tramp, 0x24, hook_slot_addr)
    struct.pack_into("<I", tramp, 0x28, RESUME_ADDR)

    data[cave_off:cave_off + SPY_CODE_LEN] = tramp[0:SPY_CODE_LEN]

    data_phdr_off = ehdr["e_phoff"] + data_seg_index * ehdr["e_phentsize"] + PHDR_OFF_MEMSZ
    struct.pack_into("<I", data, data_phdr_off, new_data_memsz)

    b_instr = encode_b(DISPATCH_PATCH_ADDR, tramp_base_vaddr)
    struct.pack_into("<4s", data, dispatch_file_off, bytes(b_instr))

    with open(out_path, "wb") as f:
        f.write(data)

    print(f"OK  wrote {out_path}")
    print(f"    spy trampoline @ 0x{tramp_base_vaddr:x} (reused code-cave, file_off=0x{cave_off:x})")
    print(f"    data segment (index {data_seg_index}) memsz grown 0x{data_seg['p_memsz']:x} -> 0x{new_data_memsz:x}")
    print(f"    hook_slot @ 0x{hook_slot_addr:x}  <-- pass to nam_preload as NAM_HOOK_SLOT_MODFAC_SPY_ADDR")
    print(f"    0x{DISPATCH_PATCH_ADDR:x} patched: cmp r1,#0x5b -> b 0x{tramp_base_vaddr:x} "
          f"(spy re-executes cmp, resumes real dispatch @ 0x{RESUME_ADDR:x} unchanged)")

    sidecar = {"NAM_HOOK_SLOT_MODFAC_SPY_ADDR": hex(hook_slot_addr)}
    with open(out_path + ".json", "w") as f:
        json.dump(sidecar, f, indent=2)
    print(f"OK  wrote {out_path}.json (hook-slot env var for the launcher script)")


if __name__ == "__main__":
    main()
