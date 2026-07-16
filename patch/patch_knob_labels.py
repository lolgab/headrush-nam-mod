#!/usr/bin/env python3
"""
patch_knob_labels.py -- rename the Ring Mod (aka Gonkulator) editor page's
knob headlines from Rate/Mix/Tone to labels matching what the NAM hijack
actually does: NAM (model-select knob, this_+0x2ac), Inp (input trim),
Outp (output trim).

This is a plain-text edit inside a Qt resource entry (uncompressed rcc v1
format: 4-byte BE length + raw bytes, confirmed this session), NOT a code
patch. Every replacement is EXACTLY the same byte length as the original
("Rate"=4, "Mix"=3, "Tone"=4) specifically to avoid needing to relocate or
grow the resource entry -- the connecting qt_resource_struct table (which
would need patching to relocate a grown entry) was searched for this session
(74 static-initializer constructors, backward-walk through the resource
blob) and not found; same-length replacement sidesteps that problem
entirely, since the entry's own total byte length never changes.

Both the mono ("Ring Mod") and stereo ("Ring Mod 2") editor pages embed
their own independent copy of this text -- both get patched.
"""
import sys

REPLACEMENTS = [
    (b'"Rate"', b'"NAM "'),
    (b'"Mix"', b'"Inp"'),
    (b'"Tone"', b'"Outp"'),
]

REGIONS = [
    ("mono editor page (Ring Mod)", 0x1b5e642, 0x1b5e642 + 865),
    ("stereo editor page (Ring Mod 2)", 0x1b895e8, 0x1b895e8 + 895),
]


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <Evil-in> <Evil-out>", file=sys.stderr)
        sys.exit(1)

    in_path, out_path = sys.argv[1:3]
    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    for region_label, start, end in REGIONS:
        region = bytes(data[start:end])
        assert b"Ring Mod" in region, f"{region_label}: sanity check failed, region doesn't look right"
        for old, new in REPLACEMENTS:
            assert len(old) == len(new), f"{old!r} -> {new!r} changes length, not safe for in-place edit"
            count = region.count(old)
            assert count == 1, f"{region_label}: expected exactly one {old!r}, found {count}"
            idx = region.index(old)
            abs_off = start + idx
            assert bytes(data[abs_off:abs_off + len(old)]) == old
            data[abs_off:abs_off + len(new)] = new
            print(f"OK  {region_label}: {old!r} -> {new!r} @ file_off=0x{abs_off:x}")
        region = bytes(data[start:end])  # refresh for next replacement's sanity check

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"OK  wrote {out_path}")


if __name__ == "__main__":
    main()
