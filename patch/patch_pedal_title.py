#!/usr/bin/env python3
"""
patch_pedal_title.py -- rename Anxiety OD's on-screen pedal-type display
                        name to "NAM".

Found a separate, plain-ASCII master display-name table in .rodata (near
file offset 0x15a0000): back-to-back null-terminated strings, one pair per
pedal type ("Gate\0", "Gate 2\0", "Green JRC-OD\0", "Green JRC-OD 2\0", ...),
each entry zero-padded up to the next 4-byte boundary. This is a completely
different region/format from the QRC-embedded QML source patched by
patch_qml_labels.py, and from the compiled-QML string table the original
(wrong-target) label patch used -- it's the master pedal-type name list,
almost certainly what the block-add menu / patch-list UI renders.

Anxiety OD has FOUR entries here, not two: "Anxiety OD", "Anxiety OD 2",
"Anxiety OD V2", "Anxiety OD V2 2" -- confirming "Anxiety OD" and
"Anxiety OD V2" are two distinct, separately-instantiable pedal types (a
V1 and V2 hardware revision), each instantiable up to twice. Our hijack
targets ONLY Anxiety OD v1 (see patch_gonkulator.py) -- the V2 entries
belong to an untouched, unrelated pedal type and must NOT be renamed, same
lesson as the original wrong-target knob-label patch.

Each entry is a plain null-terminated C string with zero-padding after the
null out to the next 4-byte boundary (verified byte-for-byte) -- NOT a
length-prefixed or fixed-stride struct, so shortening the text in place is
always safe: the new string is just null-terminated earlier and the
existing zero padding absorbs the rest. No length field anywhere to update.

Exact file offsets (stock Evil 2.7, verified byte-for-byte before writing):
  'Anxiety OD'   @ 0x15a1034  (slot: 12 bytes, incl. terminator+padding)
  'Anxiety OD 2' @ 0x15a1040  (slot: 16 bytes, incl. terminator+padding)
"""
import sys

# (file offset, expected original text, new text, total slot size incl. padding)
RENAMES = [
    (0x15a1034, "Anxiety OD", "NAM", 12),
    (0x15a1040, "Anxiety OD 2", "NAM 2", 16),
]


def patch_one(data, off, expected, new_text, slot_size):
    if len(new_text) + 1 > slot_size:
        raise ValueError(f"replacement {new_text!r} + null terminator doesn't fit in {slot_size}-byte slot")

    raw = data[off:off + slot_size]
    nul = raw.find(b"\x00")
    if nul == -1:
        raise ValueError(f"REFUSING: no null terminator found in slot @ 0x{off:x}")
    actual = raw[:nul].decode("ascii")
    if actual != expected:
        raise ValueError(
            f"REFUSING: slot @ 0x{off:x} holds {actual!r}, expected {expected!r} "
            f"-- binary layout changed, re-verify before proceeding.")
    if any(b != 0 for b in raw[nul + 1:]):
        raise ValueError(f"REFUSING: slot @ 0x{off:x} has non-zero padding after terminator, re-verify.")

    new_bytes = new_text.encode("ascii") + b"\x00" * (slot_size - len(new_text))
    data[off:off + slot_size] = new_bytes
    print(f"OK  slot @ 0x{off:x}: {expected!r} -> {new_text!r}")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <Evil-in> <Evil-out>", file=sys.stderr)
        sys.exit(1)

    in_path, out_path = sys.argv[1:3]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    for off, expected, new_text, slot_size in RENAMES:
        patch_one(data, off, expected, new_text, slot_size)

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"OK  wrote {out_path}")


if __name__ == "__main__":
    main()
