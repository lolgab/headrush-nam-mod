#!/usr/bin/env python3
"""
patch_namloader.py -- additive "Neural Amp Modeler" pedal, NOT a hijack.

Clones Gonkulator's construction shape (SESSION_NOTES.md BREAKTHROUGH #6):
  - new engine vtable  = byte copy of the real one (0x182c878 window), with
    only the process() slot repointed at our own trampoline.
  - new descriptor cell = byte copy of the real one (0x182dc14, 0x28 bytes),
    with the two fields that hold "the engine vtable address" (+0x04, +0x20)
    repointed at our new engine vtable instead of the real 0x182c878.
  - new ctor/clone function = byte copy of 0x29a038 (1196 bytes), with only
    the movw/movt pair that materializes 0x182dc14 repointed at our new
    descriptor cell. Everything else (GonkulatorController's real vtable
    literal, internal sub-object allocations) stays byte-identical -- we are
    not touching Gonkulator's real class/controller at all.
  - new primary (DSPModule-level) vtable = byte copy of
    DSPModule<Gonkulator,...>'s real 27-entry vtable (0x181ba48), with only
    slot 19 (the ctor/clone slot) repointed at our new ctor/clone copy.
  - new ModFac_construct case: overwrites ONLY the single `cmp r1,#0x5b`
    instruction at 0x209be4 with a `b dispatch_stub`. dispatch_stub special-
    cases r1==92 (case92_body, a clone of Gonkulator's own case 0x44/68) and
    otherwise replicates the overwritten comparison then resumes the real,
    untouched `ldrls` dispatch at 0x209be8 -- cases 0-91 and the out-of-range
    default path are provably bit-for-bit unaffected.

This patches a COPY of Evil (never the original). Every address touched is
sanity-checked against its expected current value before writing.
"""
import json
import struct
import sys

PAGE = 0x1000

# --- known-good addresses/values, re-derived this session (see SESSION_NOTES.md
#     BREAKTHROUGH #6) ---
ENGINE_VTABLE_BASE = 0x182c878          # Gonkulator engine's real vtable (vfunc0)
ENGINE_WINDOW_LO = -0x30                 # covers the -0x24 vcall-adjustment read
ENGINE_WINDOW_HI = 0x180                 # generous margin past slot 39
ENGINE_PROCESS_OFF = 0x9c                # slot 39, currently 0x2e7910
EXPECTED_PROCESS_FN = 0x2e7910

DESCRIPTOR_CELL_BASE = 0x182dc14         # the "0x182dc14" global read by the ctor/clone
DESCRIPTOR_CELL_SIZE = 0x28
DESCRIPTOR_ENGINE_PTR_OFFS = (0x04, 0x20)  # both fields hold the engine vtable addr
EXPECTED_DESCRIPTOR_ENGINE_PTR = 0x182c878

CTORCLONE_BASE = 0x29a038                # Gonkulator's big ctor/clone
CTORCLONE_SIZE = 1196
CTORCLONE_MOVW_OFF = 0x29a080 - CTORCLONE_BASE   # 0x48
CTORCLONE_MOVT_OFF = 0x29a084 - CTORCLONE_BASE   # 0x4c
CTORCLONE_MOVW_REG = 3                    # r3
EXPECTED_MOVW_WORD = 0xE30D3C14            # movw r3, #0xdc14
EXPECTED_MOVT_WORD = 0xE3403182            # movt r3, #0x182

DSPMODULE_VTABLE_BASE = 0x1823a48        # DSPModule<Gonkulator,...> vfunc0
DSPMODULE_VTABLE_SLOTS = 27
DSPMODULE_CTORCLONE_SLOT = 19
EXPECTED_CTORCLONE_IN_SLOT19 = 0x29a038

# Engine vtable slots 14/16 -- shared generic byte/flag setters in the REAL
# Gonkulator (0x1a3cf4/0x1a3d28, BREAKTHROUGH #4), repurposed here as our own
# Input/Output trim setters. Safe: this is OUR OWN cloned vtable copy, not
# Gonkulator's real one -- repointing these two slots never touches any real
# Gonkulator instance. Slot 15 (the ONE true shared float setter, 0x1a3d04)
# is deliberately left untouched -- it's still the model-select knob.
ENGINE_TRIM_IN_SLOT_OFF = 14 * 4          # relative to ENGINE_VTABLE_BASE
ENGINE_TRIM_OUT_SLOT_OFF = 16 * 4
EXPECTED_TRIM_IN_ORIG = 0x1a3cf4
EXPECTED_TRIM_OUT_ORIG = 0x1a3d28

DISPATCH_PATCH_ADDR = 0x209be4           # the `cmp r1,#0x5b` we overwrite
EXPECTED_DISPATCH_WORD = 0xE351005B       # cmp r1, #0x5b


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


def vaddr_to_file_off(data_len_hint, vaddr):
    # this binary's LOAD0 segment maps file_off = vaddr - 0x8000 throughout
    # the regions we touch (consistent with every prior patch script here).
    return vaddr - 0x8000


def read_at_vaddr(data, vaddr, size):
    off = vaddr_to_file_off(len(data), vaddr)
    return bytes(data[off:off + size])


def encode_movw(rd, imm16):
    imm4 = (imm16 >> 12) & 0xF
    imm12 = imm16 & 0xFFF
    word = (0xE << 28) | (0x30 << 20) | (imm4 << 16) | (rd << 12) | imm12
    return struct.pack("<I", word)


def encode_movt(rd, imm16):
    imm4 = (imm16 >> 12) & 0xF
    imm12 = imm16 & 0xFFF
    word = (0xE << 28) | (0x34 << 20) | (imm4 << 16) | (rd << 12) | imm12
    return struct.pack("<I", word)


def encode_b(from_vaddr, to_vaddr, link=False):
    pc = from_vaddr + 8
    delta = to_vaddr - pc
    assert delta % 4 == 0, "branch target not word-aligned relative to source"
    imm24 = (delta >> 2) & 0xFFFFFF
    # sanity: must round-trip through sign-extension within +-32MB
    signed = imm24 if imm24 < (1 << 23) else imm24 - (1 << 24)
    assert signed * 4 == delta, "branch out of +-32MB range"
    word = (0xE << 28) | (0b101 << 25) | ((1 << 24) if link else 0) | imm24
    return struct.pack("<I", word)


def decode_bl(data, vaddr):
    off = vaddr_to_file_off(len(data), vaddr)
    word = struct.unpack_from("<I", data, off)[0]
    imm24 = word & 0xFFFFFF
    signed = imm24 if imm24 < (1 << 23) else imm24 - (1 << 24)
    return (vaddr + 8) + (signed * 4)


# Every EXTERNAL bl inside the 1196-byte ctor/clone function -- i.e. every call
# whose target lies OUTSIDE [CTORCLONE_BASE, CTORCLONE_BASE+CTORCLONE_SIZE).
# A raw byte-copy preserves each instruction's ENCODED offset, which is only
# correct at the ORIGINAL address; relocating the blob shifts what any
# not-fixed-up relative branch actually reaches by the full relocation delta.
# Internal branches (loops/conditionals whose target is also inside this same
# window) need no fixup -- both ends move together, the encoded relative
# offset stays correct. Offsets are relative to CTORCLONE_BASE; targets are
# decoded fresh from the real binary below rather than hand-copied, so a
# stale/wrong offset list fails loudly instead of silently corrupting a call.
CTORCLONE_EXTERNAL_BL_OFFSETS = [
    0x34, 0x44, 0x118, 0x178, 0x1b0, 0x1b8, 0x1f0, 0x234, 0x27c, 0x2c8,
    0x344, 0x38c, 0x39c, 0x3c0, 0x3d0, 0x3ec, 0x3f4, 0x430,
]


def main():
    if len(sys.argv) != 6:
        print(f"usage: {sys.argv[0]} <Evil-in> <trampoline_naml.bin> <case92_stub.bin> "
              f"<trampoline_trim.bin> <Evil-out>", file=sys.stderr)
        sys.exit(1)

    in_path, tramp_path, stub_path, trim_path, out_path = sys.argv[1:6]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())
    with open(tramp_path, "rb") as f:
        tramp = bytearray(f.read())
    with open(stub_path, "rb") as f:
        stub = bytearray(f.read())
    with open(trim_path, "rb") as f:
        trim = bytearray(f.read())

    ehdr = parse_ehdr(data)
    assert ehdr["e_machine"] == 40, "not ARM (EM_ARM=40)"
    assert ehdr["e_type"] == 2, "expected ET_EXEC (non-PIE)"
    phdrs = parse_phdrs(data, ehdr)

    # ---- sanity-check every address we're about to touch or depend on ----
    process_val = struct.unpack_from("<I", read_at_vaddr(data, ENGINE_VTABLE_BASE + ENGINE_PROCESS_OFF, 4))[0]
    assert process_val == EXPECTED_PROCESS_FN, \
        f"engine vtable process slot holds 0x{process_val:x}, expected 0x{EXPECTED_PROCESS_FN:x}"

    for off in DESCRIPTOR_ENGINE_PTR_OFFS:
        val = struct.unpack_from("<I", read_at_vaddr(data, DESCRIPTOR_CELL_BASE + off, 4))[0]
        assert val == EXPECTED_DESCRIPTOR_ENGINE_PTR, \
            f"descriptor cell +0x{off:x} holds 0x{val:x}, expected 0x{EXPECTED_DESCRIPTOR_ENGINE_PTR:x}"

    movw_word = struct.unpack_from("<I", read_at_vaddr(data, CTORCLONE_BASE + CTORCLONE_MOVW_OFF, 4))[0]
    movt_word = struct.unpack_from("<I", read_at_vaddr(data, CTORCLONE_BASE + CTORCLONE_MOVT_OFF, 4))[0]
    assert movw_word == EXPECTED_MOVW_WORD, f"ctor/clone movw holds 0x{movw_word:x}, expected 0x{EXPECTED_MOVW_WORD:x}"
    assert movt_word == EXPECTED_MOVT_WORD, f"ctor/clone movt holds 0x{movt_word:x}, expected 0x{EXPECTED_MOVT_WORD:x}"

    slot19_val = struct.unpack_from(
        "<I", read_at_vaddr(data, DSPMODULE_VTABLE_BASE + DSPMODULE_CTORCLONE_SLOT * 4, 4))[0]
    assert slot19_val == EXPECTED_CTORCLONE_IN_SLOT19, \
        f"DSPModule vtable slot19 holds 0x{slot19_val:x}, expected 0x{EXPECTED_CTORCLONE_IN_SLOT19:x}"

    dispatch_word = struct.unpack_from("<I", read_at_vaddr(data, DISPATCH_PATCH_ADDR, 4))[0]
    assert dispatch_word == EXPECTED_DISPATCH_WORD, \
        f"dispatch site holds 0x{dispatch_word:x}, expected 0x{EXPECTED_DISPATCH_WORD:x}"

    trim_in_val = struct.unpack_from("<I", read_at_vaddr(data, ENGINE_VTABLE_BASE + ENGINE_TRIM_IN_SLOT_OFF, 4))[0]
    assert trim_in_val == EXPECTED_TRIM_IN_ORIG, \
        f"engine vtable slot14 holds 0x{trim_in_val:x}, expected 0x{EXPECTED_TRIM_IN_ORIG:x}"
    trim_out_val = struct.unpack_from("<I", read_at_vaddr(data, ENGINE_VTABLE_BASE + ENGINE_TRIM_OUT_SLOT_OFF, 4))[0]
    assert trim_out_val == EXPECTED_TRIM_OUT_ORIG, \
        f"engine vtable slot16 holds 0x{trim_out_val:x}, expected 0x{EXPECTED_TRIM_OUT_ORIG:x}"

    print("OK  all sanity checks passed against extracted/Evil")

    # ---- build the new segment's contents ----
    engine_vtable_blob = bytearray(read_at_vaddr(data, ENGINE_VTABLE_BASE + ENGINE_WINDOW_LO,
                                                  ENGINE_WINDOW_HI - ENGINE_WINDOW_LO))
    descriptor_blob = bytearray(read_at_vaddr(data, DESCRIPTOR_CELL_BASE, DESCRIPTOR_CELL_SIZE))
    ctorclone_blob = bytearray(read_at_vaddr(data, CTORCLONE_BASE, CTORCLONE_SIZE))
    dspmodule_vtable_blob = bytearray(read_at_vaddr(data, DSPMODULE_VTABLE_BASE, DSPMODULE_VTABLE_SLOTS * 4))

    # Placement: NOT past the last PT_LOAD (that's ~50MB from ModFac_construct/
    # the ctor-clone function on this binary -- confirmed this session -- which
    # breaks both the single-instruction `b dispatch_stub` overwrite at
    # DISPATCH_PATCH_ADDR (ARM B/BL is +-32MB) AND every internal bl/b inside
    # the raw-copied ctorclone_blob, whose own internal calls reach as far as
    # 0x629940). Instead: vaddr 0x0-0x7fff is a genuine unmapped hole on this
    # binary (LOAD0 starts at 0x8000, nothing else maps below it -- confirmed
    # via full phdr dump this session) and sits comfortably within +-32MB of
    # every address we reference (dispatch site ~0x209be4, ctorclone's own
    # internal calls up to ~0x629940, etc). File offset is unrelated to vaddr
    # placement (ELF only requires p_vaddr === p_offset mod p_align, trivially
    # satisfied since both are page-aligned) so the new segment's bytes still
    # get appended at the end of the file as usual.
    new_vaddr_base = 0x2000
    for p in phdrs:
        if p["p_type"] == PT_LOAD:
            assert not (new_vaddr_base < p["p_vaddr"] + p["p_memsz"] and
                        p["p_vaddr"] < new_vaddr_base + 0x6000), \
                f"0x2000 hole collides with existing LOAD segment @ 0x{p['p_vaddr']:x}"

    new_file_offset = round_up(len(data), PAGE)
    assert new_file_offset % PAGE == new_vaddr_base % PAGE == 0

    phdr_table_bytes = (len(phdrs) + 1) * ehdr["e_phentsize"]

    cursor = phdr_table_bytes
    engine_vtable_off = cursor; cursor += len(engine_vtable_blob)
    cursor = round_up(cursor, 4)
    descriptor_off = cursor; cursor += len(descriptor_blob)
    cursor = round_up(cursor, 4)
    ctorclone_off = cursor; cursor += len(ctorclone_blob)
    cursor = round_up(cursor, 4)
    dspmodule_vtable_off = cursor; cursor += len(dspmodule_vtable_blob)
    cursor = round_up(cursor, 4)
    tramp_off = cursor; cursor += len(tramp)
    cursor = round_up(cursor, 4)
    stub_off = cursor; cursor += len(stub)
    cursor = round_up(cursor, 4)
    trim_off = cursor; cursor += len(trim)

    seg_total_size = cursor

    engine_vtable_vaddr = new_vaddr_base + engine_vtable_off
    descriptor_vaddr = new_vaddr_base + descriptor_off
    ctorclone_vaddr = new_vaddr_base + ctorclone_off
    dspmodule_vtable_vaddr = new_vaddr_base + dspmodule_vtable_off
    tramp_vaddr = new_vaddr_base + tramp_off
    stub_vaddr = new_vaddr_base + stub_off
    trim_vaddr = new_vaddr_base + trim_off

    # our engine vtable's own "vfunc0" sits ENGINE_WINDOW_LO bytes into the blob
    new_engine_vfunc0_vaddr = engine_vtable_vaddr + (-ENGINE_WINDOW_LO)

    # --- patch component 1: engine vtable's process slot -> our trampoline ---
    struct.pack_into("<I", engine_vtable_blob, (-ENGINE_WINDOW_LO) + ENGINE_PROCESS_OFF, tramp_vaddr)

    # --- patch component 1b: engine vtable slots 14/16 -> Input/Output trim trampolines ---
    # trampoline_trim.bin layout (confirmed via nm): trampoline_trim_in@0x00,
    # trampoline_trim_out@0x1c
    trim_in_tramp_vaddr = trim_vaddr + 0x00
    trim_out_tramp_vaddr = trim_vaddr + 0x1c
    struct.pack_into("<I", engine_vtable_blob, (-ENGINE_WINDOW_LO) + ENGINE_TRIM_IN_SLOT_OFF, trim_in_tramp_vaddr)
    struct.pack_into("<I", engine_vtable_blob, (-ENGINE_WINDOW_LO) + ENGINE_TRIM_OUT_SLOT_OFF, trim_out_tramp_vaddr)

    # --- patch component 2: descriptor cell's two engine-ptr fields -> our new engine vtable ---
    for off in DESCRIPTOR_ENGINE_PTR_OFFS:
        struct.pack_into("<I", descriptor_blob, off, new_engine_vfunc0_vaddr)

    # --- patch component 3: ctor/clone's movw/movt -> our new descriptor cell ---
    imm16 = descriptor_vaddr & 0xFFFF
    imm16_hi = (descriptor_vaddr >> 16) & 0xFFFF
    ctorclone_blob[CTORCLONE_MOVW_OFF:CTORCLONE_MOVW_OFF + 4] = encode_movw(CTORCLONE_MOVW_REG, imm16)
    ctorclone_blob[CTORCLONE_MOVT_OFF:CTORCLONE_MOVT_OFF + 4] = encode_movt(CTORCLONE_MOVW_REG, imm16_hi)

    # --- patch component 3b: fix up every EXTERNAL bl inside the copied ctor/clone ---
    # (internal branches -- loops/conditionals whose target is also inside this
    # same 1196-byte window -- need no fixup, see CTORCLONE_EXTERNAL_BL_OFFSETS
    # comment above: both ends move together with the blob.)
    for bl_off in CTORCLONE_EXTERNAL_BL_OFFSETS:
        orig_word = struct.unpack_from("<I", ctorclone_blob, bl_off)[0]
        assert (orig_word >> 24) == 0xEB, \
            f"ctorclone +0x{bl_off:x} is 0x{orig_word:x}, not a bl instruction -- offset list is stale"
        real_target = decode_bl(data, CTORCLONE_BASE + bl_off)
        assert not (CTORCLONE_BASE <= real_target < CTORCLONE_BASE + CTORCLONE_SIZE), \
            f"ctorclone +0x{bl_off:x} targets 0x{real_target:x}, which is INTERNAL -- remove from the external-fixup list"
        new_from_vaddr = ctorclone_vaddr + bl_off
        ctorclone_blob[bl_off:bl_off + 4] = encode_b(new_from_vaddr, real_target, link=True)

    # --- patch component 4: DSPModule-level vtable slot 19 -> our ctor/clone copy ---
    struct.pack_into("<I", dspmodule_vtable_blob, DSPMODULE_CTORCLONE_SLOT * 4, ctorclone_vaddr)

    # --- patch component 5: trampoline's hook_slot_lit / orig_fn_lit ---
    # trampoline_naml.bin layout (confirmed via nm): hook_slot_lit@0x14, orig_fn_lit@0x18,
    # hook_slot_naml@0x1c, noop_ret_naml@0x20
    hook_slot_naml_vaddr = tramp_vaddr + 0x1c
    noop_ret_naml_vaddr = tramp_vaddr + 0x20
    struct.pack_into("<I", tramp, 0x14, hook_slot_naml_vaddr)
    struct.pack_into("<I", tramp, 0x18, noop_ret_naml_vaddr)

    # --- patch component 6: case92_stub's slotB_lit -> our new primary vtable ---
    # case92_stub.bin layout (confirmed via nm): slotB_lit@0x40
    struct.pack_into("<I", stub, 0x40, dspmodule_vtable_vaddr)

    # --- patch component 7: trim trampolines' hook_slot_lit fields ---
    # trampoline_trim.bin layout (confirmed via nm): hook_slot_in_lit@0x14,
    # hook_slot_naml_trim_in@0x18, hook_slot_out_lit@0x30, hook_slot_naml_trim_out@0x34
    hook_slot_naml_trim_in_vaddr = trim_vaddr + 0x18
    hook_slot_naml_trim_out_vaddr = trim_vaddr + 0x34
    struct.pack_into("<I", trim, 0x14, hook_slot_naml_trim_in_vaddr)
    struct.pack_into("<I", trim, 0x30, hook_slot_naml_trim_out_vaddr)

    # ---- assemble the new segment ----
    new_segment = bytearray(seg_total_size)
    new_segment[engine_vtable_off:engine_vtable_off + len(engine_vtable_blob)] = engine_vtable_blob
    new_segment[descriptor_off:descriptor_off + len(descriptor_blob)] = descriptor_blob
    new_segment[ctorclone_off:ctorclone_off + len(ctorclone_blob)] = ctorclone_blob
    new_segment[dspmodule_vtable_off:dspmodule_vtable_off + len(dspmodule_vtable_blob)] = dspmodule_vtable_blob
    new_segment[tramp_off:tramp_off + len(tramp)] = tramp
    new_segment[stub_off:stub_off + len(stub)] = stub
    new_segment[trim_off:trim_off + len(trim)] = trim

    new_load_entry = {
        "p_type": PT_LOAD, "p_offset": new_file_offset, "p_vaddr": new_vaddr_base,
        "p_paddr": new_vaddr_base, "p_filesz": seg_total_size, "p_memsz": seg_total_size,
        "p_flags": PF_R | PF_W | PF_X, "p_align": PAGE,
    }

    new_phdrs_bytes = bytearray()
    for p in phdrs:
        new_phdrs_bytes += pack_phdr(p)
    new_phdrs_bytes += pack_phdr(new_load_entry)
    assert len(new_phdrs_bytes) == phdr_table_bytes
    new_segment[0:phdr_table_bytes] = new_phdrs_bytes

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

    # ---- the one 4-byte overwrite in the ORIGINAL code: cmp -> b dispatch_stub ----
    b_instr = encode_b(DISPATCH_PATCH_ADDR, stub_vaddr)
    dispatch_file_off = vaddr_to_file_off(len(data), DISPATCH_PATCH_ADDR)
    struct.pack_into("<4s", data, dispatch_file_off, bytes(b_instr))

    with open(out_path, "wb") as f:
        f.write(data)

    print(f"OK  wrote {out_path}")
    print(f"    new segment vaddr=0x{new_vaddr_base:x} file_off=0x{new_file_offset:x} size=0x{seg_total_size:x}")
    print(f"    new engine vtable  @ 0x{new_engine_vfunc0_vaddr:x}  (process slot -> trampoline @ 0x{tramp_vaddr:x})")
    print(f"    new descriptor cell @ 0x{descriptor_vaddr:x}")
    print(f"    new ctor/clone      @ 0x{ctorclone_vaddr:x}")
    print(f"    new primary vtable  @ 0x{dspmodule_vtable_vaddr:x}  (slot19 -> ctor/clone)")
    print(f"    dispatch_stub       @ 0x{stub_vaddr:x}  (case92_body @ 0x{stub_vaddr + 0x14:x})")
    print(f"    trampoline_naml     @ 0x{tramp_vaddr:x}")
    print(f"    hook_slot_naml      @ 0x{hook_slot_naml_vaddr:x}  <-- pass to nam_preload as NAM_HOOK_SLOT_NAML_ADDR")
    print(f"    trim trampolines    @ 0x{trim_vaddr:x}  (engine vtable slots 14/16 -> in/out trim)")
    print(f"    hook_slot_trim_in   @ 0x{hook_slot_naml_trim_in_vaddr:x}  <-- NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR")
    print(f"    hook_slot_trim_out  @ 0x{hook_slot_naml_trim_out_vaddr:x}  <-- NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR")
    print(f"    0x209be4 patched: cmp r1,#0x5b -> b 0x{stub_vaddr:x}")

    sidecar = {
        "NAM_HOOK_SLOT_NAML_ADDR": hex(hook_slot_naml_vaddr),
        "NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR": hex(hook_slot_naml_trim_in_vaddr),
        "NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR": hex(hook_slot_naml_trim_out_vaddr),
    }
    with open(out_path + ".json", "w") as f:
        json.dump(sidecar, f, indent=2)
    print(f"OK  wrote {out_path}.json (hook-slot env vars for the launcher script)")


if __name__ == "__main__":
    main()
