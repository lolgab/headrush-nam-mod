#!/usr/bin/env python3
"""
patch_gonkulator.py -- repoint Anxiety OD engine object's process() vtable
                       slot at an injected trampoline running NAM inference
                       instead.

STATUS: originally targeted the "Gonkulator" class (confirmed dead end --
real, RTTI-confirmed class, but not wired to any current UI editor page).
Then retargeted to Volume (reachable, but user needs its real function --
expression pedal control -- so that hijack was never shipped even after
being made to work). Now targeting ANXIETY OD (v1, not the V2 sibling),
the user's own choice of a pedal they're fine sacrificing. Same
DSPModule<AnxietyOD,AnxietyODController,DSPSerializer> architecture as
Volume/Gonkulator, so the exact same hijack mechanism applies unchanged.
Unlike Volume/Gonkulator, AnxietyOD HAS working Itanium RTTI, which gave an
independent, second way to derive its engine vtable (xref-chasing from the
RTTI type_info name string) that was cross-checked against the ctor/clone
disassembly approach -- both agree exactly, on both the wrapper vtable
(via ModFac_construct case) and the real engine vtable.

Reverse-engineered facts (Evil 2.7):
  - AnxietyOD's wrapper vtable is 0x1826358, found via RTTI xref-chase: the
    mangled name string "N8EvilDSPs9DSPModuleINS_9AnxietyODE...E" (embedded
    literal, `strings`-visible even in this stripped binary) is referenced
    by a type_info struct, which is itself referenced by this vtable's
    typeinfo slot (vtable_addr - 4). This is exactly ModFac_construct case
    6's known slotB value (see kCaseVtables in nam_hook.cpp) -- AnxietyOD
    is CASE 6.
  - AnxietyOD's own ctor/clone function is at vaddr 0x2c9298 (wrapper
    vtable slot 19, same slot number that held Volume's own ctor/clone --
    consistent across every DSPModule<T,...> wrapper's vtable layout).
    That function writes its new engine object's vtable field TWICE before
    storing it into the wrapper's own field at +0x270: first a generic
    placeholder (0x1610f14, shared with every DSPModule subtype, same value
    Volume/Gonkulator use too), then -- unconditionally, immediately after
    -- the real, final vtable, 0x1839044, read directly from a literal pool
    constant (AnxietyOD has RTTI, so its ctor skips the manual
    descriptor-cell double-indirection Volume/Gonkulator's no-RTTI ctors
    needed; the compiler just embeds the real vtable's address directly).
    0x1839044 independently matches the RTTI-xref-chase result above exactly.
  - Slot index 8 of the real vtable (0x1839044), file_offset computed below,
    vaddr 0x1839064, holds 0x3260e0, the real per-sample audio process()
    function -- same slot index 8 as Volume's own engine vtable (this base
    virtual-function layout is shared across every DSPModule engine
    subclass in this shape). Confirmed by comparing its prologue
    instruction-for-instruction against Gonkulator/Volume's own already-
    ABI-confirmed process(): identical register save sequence (strd
    r4/r6/r8/sl, str lr, vpush d8), identical `ldrb r3, [r0, #0x29c]`
    this-relative flag read (same field offset as Volume/Gonkulator -- this
    base class layout, including the model-select knob field at +0x2ac fed
    by the shared setter at vtable slot 15 / 0x1a3d04, exactly matches
    Volume's engine vtable slot 15 too), identical argument register usage
    (r0=this, r2=input array, r3=numChannels, stack args beyond). Same
    signature as Gonkulator's/Volume's:
        void process(EngineObj* this, uint32_t param2, float** input,
                     uint32_t numChannels, float** output, int32_t numFrames,
                     uint32_t* flags, void* ctx)
    input/output are per-channel float* arrays; numChannels is 1 or 2.
  - AnxietyOD's real knobs (Drive/Tone/Level) are all 0-100% (NOT
    Gonkulator's 200-2000 Hz range) -- see nam_hook.cpp's knob_min()/
    knob_max() defaults.
  - This is a REPLACE (either/or) hook, same pattern as the (superseded)
    IRLoader hook: trampoline checks a mutable hook_slot; if set, tail-jumps
    there (identical signature, so args pass through unchanged); else falls
    back to the original 0x3260e0. Unlike the IRLoader trampoline, this one
    uses r12 (ip) as scratch instead of r1, because r1 here is a REAL
    argument (param2), not free scratch space.
  - AnxietyOD has a V2 sibling too (ModuleEditors/AnxietyODV2 exists), not
    hijacked here -- only the v1 (mono) vtable is patched.

DOES NOT add a new PT_LOAD segment or touch e_phoff/e_phnum -- an earlier
version did (new segment + relocated phdr table), and that specific
mechanism was confirmed (via a real-device A/B elimination: FIT repack, xz
recompression, debugfs file injection, LD_PRELOAD+dlopen of the real NAM
code, and a pure same-length string patch to Evil all booted fine; only the
new-segment/phdr-relocation version bricked, whether or not the hook ever
actually fired) to break boot on the real device, for reasons that don't
reproduce under QEMU user-mode emulation (which got the same patched binary
all the way through dynamic linking and Qt init). Suspected cause: the real
kernel's ELF loader handling relocated/appended phdrs differently than
glibc's userspace loader helper does, but this was never confirmed via
UART -- treat as "known to fail" empirically, root cause unconfirmed.

Instead this reuses two pieces of space that already exist in Evil's
CURRENT, UNMODIFIED program headers:
  - The trampoline's code + its two read-only literals (28 bytes) go in a
    verified-zero "code cave" already inside the existing R+E LOAD segment
    (linker alignment slack between the last real section and the RELRO
    overlap boundary -- confirmed empirically to be a clean, sizeable run of
    zero bytes outside every declared section). No new segment, no phdr
    changes, no file growth: just overwriting already-unused, already-R+X
    bytes in place -- the same category of change as the knob-label string
    patch (confirmed safe on real hardware).
  - The mutable hook_slot (4 bytes nam_preload writes the real hook pointer
    into at process startup) goes right past the end of the existing R+W
    LOAD segment's declared memsz, by growing that ONE existing phdr's
    p_memsz field slightly -- a single-field edit to an EXISTING entry, not
    a new one. Needs no file bytes at all: memsz > filesz is exactly how
    every ELF binary's own .bss already works, so the kernel zero-fills it
    the same way it always zero-fills BSS.

This patches a COPY of Evil (never the original). Sanity-checks the vtable
slot currently holds 0x2e7910, and the code cave is genuinely zero, before
touching anything.
"""
import argparse
import json
import struct
import sys

PAGE = 0x1000

# The two values that vary per `Evil` are CLI args (--engine-vtable, --orig-fn);
# their defaults are the HeadRush Pedalboard 2.7 values, so a bare invocation is
# unchanged, and build_update_img.py passes per-model overrides (see
# scripts/model_targets.py). PROCESS_SLOT and VADDR_BASE are the same for every
# supported Evil (same DSPModule<AnxietyOD,...> class / ELF layout), so they're
# fixed constants here rather than per-model.
DEFAULT_ENGINE_VTABLE = 0x1839044  # AnxietyOD engine object's real vtable address-point
DEFAULT_ORIG_FN = 0x3260e0         # value the slot holds pre-patch (real process())
PROCESS_SLOT = 8                   # process() index in the engine vtable
VADDR_BASE = 0x8000                # code segment p_vaddr - p_offset (vaddr -> file offset)
TRAMP_CODE_LEN = 28  # trampoline_gonk.S bytes 0:28 -- instructions + the two literals


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

# Phdr field byte offsets within one Elf32_Phdr entry (see pack_phdr's field order).
PHDR_OFF_FILESZ = 4 * 4
PHDR_OFF_MEMSZ = 5 * 4


def find_reclaimable_gap(data, phdrs, code_seg, need_len):
    """Find the dead-space gap between code_seg's file end and the next
    PT_LOAD segment's file start. Between two consecutive PT_LOAD segments
    there is typically a page-alignment gap in the FILE that neither
    segment's p_filesz/p_offset actually covers -- real bytes sitting in the
    file, but not mapped into the process's address space by ANY current
    segment (unlike a section-internal zero run, e.g. inside .hash, which
    IS mapped and IS in use). Confirmed empirically for this binary: a
    verified-all-zero gap of several KB between the R+E and R+W segments.

    This does NOT scan for zero bytes inside a segment's own declared
    filesz/vaddr range -- that already burned us once (a "zero run" that
    turned out to be real, in-use .hash bucket data). Reusing this
    between-segments gap instead means the reused bytes were never part of
    any segment's mapped image in the first place, so there's nothing to
    collide with. We simply extend code_seg's OWN p_filesz/p_memsz to bring
    the gap inside its mapped range."""
    code_file_end = code_seg["p_offset"] + code_seg["p_filesz"]
    later = [p for p in phdrs if p["p_type"] == PT_LOAD and p["p_offset"] >= code_file_end
             and p is not code_seg]
    if not later:
        return None
    next_seg = min(later, key=lambda p: p["p_offset"])
    gap_size = next_seg["p_offset"] - code_file_end
    if gap_size < need_len:
        return None
    gap_bytes = data[code_file_end:code_file_end + gap_size]
    if any(gap_bytes):
        return None  # not genuinely dead space -- refuse rather than guess
    return gap_size


def auto_int(x):
    return int(x, 0)


def main():
    ap = argparse.ArgumentParser(description="repoint Anxiety OD engine process() vtable slot")
    ap.add_argument("in_path", help="stock Evil binary (never modified)")
    ap.add_argument("tramp_path", help="assembled trampoline_gonk.bin (32 bytes)")
    ap.add_argument("out_path", help="patched Evil to write")
    ap.add_argument("--engine-vtable", type=auto_int, default=DEFAULT_ENGINE_VTABLE,
                    help="AnxietyOD engine vtable address-point (default: Pedalboard 2.7)")
    ap.add_argument("--orig-fn", type=auto_int, default=DEFAULT_ORIG_FN,
                    help="value the slot must already hold (real process(); fallback + sanity guard)")
    args = ap.parse_args()

    in_path, tramp_path, out_path = args.in_path, args.tramp_path, args.out_path
    slot_vaddr = args.engine_vtable + PROCESS_SLOT * 4
    gonk_vtable_slot_file_offset = slot_vaddr - VADDR_BASE
    expected_orig_fn = args.orig_fn

    with open(in_path, "rb") as f:
        data = bytearray(f.read())
    with open(tramp_path, "rb") as f:
        tramp = bytearray(f.read())

    assert len(tramp) == 32, f"expected 32-byte trampoline, got {len(tramp)}"

    ehdr = parse_ehdr(data)
    assert ehdr["e_machine"] == 40, "not ARM (EM_ARM=40)"
    assert ehdr["e_type"] == 2, "expected ET_EXEC (non-PIE)"

    phdrs = parse_phdrs(data, ehdr)

    orig_val = struct.unpack_from("<I", data, gonk_vtable_slot_file_offset)[0]
    if orig_val != expected_orig_fn:
        print(f"REFUSING: AnxietyOD vtable slot @ 0x{gonk_vtable_slot_file_offset:x} "
              f"holds 0x{orig_val:x}, expected 0x{expected_orig_fn:x}. Wrong binary / "
              f"wrong --model / already patched / stale offsets -- re-verify before proceeding.",
              file=sys.stderr)
        sys.exit(2)

    code_segs = [p for p in phdrs if p["p_type"] == PT_LOAD and p["p_flags"] == (PF_R | PF_X)]
    data_segs = [p for p in phdrs if p["p_type"] == PT_LOAD and p["p_flags"] == (PF_R | PF_W)]
    if len(code_segs) != 1 or len(data_segs) != 1:
        print(f"REFUSING: expected exactly one plain R+E LOAD segment and one plain R+W "
              f"LOAD segment, found {len(code_segs)} and {len(data_segs)} -- binary layout "
              f"changed, re-verify before proceeding.", file=sys.stderr)
        sys.exit(2)
    code_seg, data_seg = code_segs[0], data_segs[0]
    code_seg_index = phdrs.index(code_seg)
    data_seg_index = phdrs.index(data_seg)

    gap_size = find_reclaimable_gap(data, phdrs, code_seg, TRAMP_CODE_LEN)
    if gap_size is None:
        print(f"REFUSING: no verified-zero, currently-unmapped-by-any-segment gap of "
              f">= {TRAMP_CODE_LEN} bytes found right after the R+E segment's file end "
              f"-- binary layout changed, re-verify before proceeding.", file=sys.stderr)
        sys.exit(2)

    cave_off = code_seg["p_offset"] + code_seg["p_filesz"]  # == old file end, gap starts here
    tramp_base_vaddr = code_seg["p_vaddr"] + code_seg["p_filesz"]  # == old vaddr end
    new_code_filesz = code_seg["p_filesz"] + gap_size
    new_code_memsz = code_seg["p_memsz"] + gap_size

    # Mutable hook_slot goes right past the data segment's CURRENT memsz end
    # -- exactly where its own .bss already ends, extended by one word. No
    # new phdr entry: this only grows one existing entry's p_memsz field.
    hook_slot_addr = data_seg["p_vaddr"] + data_seg["p_memsz"]
    new_data_memsz = data_seg["p_memsz"] + 4

    struct.pack_into("<I", tramp, 20, hook_slot_addr)
    struct.pack_into("<I", tramp, 24, expected_orig_fn)

    data[cave_off:cave_off + TRAMP_CODE_LEN] = tramp[0:TRAMP_CODE_LEN]

    code_phdr_base = ehdr["e_phoff"] + code_seg_index * ehdr["e_phentsize"]
    struct.pack_into("<I", data, code_phdr_base + PHDR_OFF_FILESZ, new_code_filesz)
    struct.pack_into("<I", data, code_phdr_base + PHDR_OFF_MEMSZ, new_code_memsz)

    data_phdr_off = ehdr["e_phoff"] + data_seg_index * ehdr["e_phentsize"] + PHDR_OFF_MEMSZ
    struct.pack_into("<I", data, data_phdr_off, new_data_memsz)

    struct.pack_into("<I", data, gonk_vtable_slot_file_offset, tramp_base_vaddr)

    with open(out_path, "wb") as f:
        f.write(data)

    print(f"OK  wrote {out_path}")
    print(f"    reclaimed inter-segment gap file_off=0x{cave_off:x} size=0x{gap_size:x}")
    print(f"    code segment (index {code_seg_index}) filesz/memsz grown "
          f"0x{code_seg['p_filesz']:x} -> 0x{new_code_filesz:x}")
    print(f"    trampoline @ 0x{tramp_base_vaddr:x}  (AnxietyOD engine vtable slot 8 now points here)")
    print(f"    data segment (index {data_seg_index}) memsz grown 0x{data_seg['p_memsz']:x} -> 0x{new_data_memsz:x}")
    print(f"    hook_slot  @ 0x{hook_slot_addr:x}  <-- pass to nam_preload as NAM_HOOK_SLOT_GONK_ADDR")
    print(f"    fallback (orig process()) still reachable @ 0x{expected_orig_fn:x}")
    print(f"    no new PT_LOAD segment, no e_phoff/e_phnum change, file size unchanged "
          f"(0x{len(data):x} bytes)")

    sidecar = {"NAM_HOOK_SLOT_GONK_ADDR": hex(hook_slot_addr)}
    with open(out_path + ".json", "w") as f:
        json.dump(sidecar, f, indent=2)
    print(f"OK  wrote {out_path}.json (hook-slot env var for the launcher script)")


if __name__ == "__main__":
    main()
