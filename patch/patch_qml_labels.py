#!/usr/bin/env python3
"""
patch_qml_labels.py -- rename Anxiety OD's on-screen knob labels to match
                       what they actually do after the NAM hijack.

SUPERSEDES an earlier version of this script that patched a QV4 compiled-
QML string table by offset. That version shipped (v38-v42) with zero effect
on real hardware. Root-caused: those offsets were chosen by proximity to
Anxiety OD's propertyPath string, an unverified heuristic -- "Drive"/"Tone"/
"Level" are generic labels reused by 70+ other pedal/effect types, each
compiled into its own unit at a different offset, so the old patch almost
certainly relabeled some OTHER pedal instead, unnoticed.

Real target, this time verified end-to-end: Evil embeds each UI page's QML
SOURCE TEXT (not just compiled bytecode) as a raw, uncompressed Qt Resource
System (.qrc) entry directly in .rodata -- a 4-byte big-endian length
prefix followed by that many bytes of plain ASCII/UTF-8 source, no
compression marker. Found by searching for the one-and-only literal
'propertyPath: "/Engine/Patch/Anxiety OD"' occurrence in the whole binary
(this exact string is also Anxiety OD's real internal state-tree key, so
it's unambiguous -- unlike the generic knob-name text). That occurrence
sits inside a length-prefixed blob whose recorded length (0x3e9 = 1001)
exactly matches the actual QML source text that follows, confirming this
is a real, individually-addressable resource entry, not compiled bytecode.
The source text itself is the actual BlockDetailsSubPage for Anxiety OD:
    headline: "Level";  prop: Evil.getProperty(propertyPath + "/Level")
    headline: "Drive";  prop: Evil.getProperty(propertyPath + "/Drive")
    headline: "Tone";   prop: Evil.getProperty(propertyPath + "/Tone")
    headline: "Hi-Lo";  prop: Evil.getProperty(propertyPath + "/Hi-Lo")
Only the `headline: "..."` occurrences are touched -- NOT the matching
`+ "/Level"` etc. path-suffix occurrences of the same word, which are the
real internal parameter keys and must stay put.

Same-character-count-only, matching this project's established discipline
for leaf-data patches: shortening/lengthening the QML source text would
change the resource entry's byte length, requiring updates to the 4-byte
length prefix AND every subsequent resource entry's recorded offset in the
(unlocated) qt_resource_struct index -- out of scope for a same-length text
swap.

Exact file offsets (stock Evil 2.7, verified byte-for-byte before writing):
  'Level' headline @ 0x1b7be57  (5 chars)
  'Drive' headline @ 0x1b7beba  (5 chars)
  'Tone'  headline @ 0x1b7bf1b  (4 chars)

Renamed to match the confirmed real function (see patch_gonkulator.py's
docstring for the knob-to-offset mapping derivation):
  Drive (model select)  -> "Model" (5 chars, same length)
  Tone  (input trim)    -> "Inp "  (4 chars, same length, space-padded)
  Level (output trim)   -> "Outp " (5 chars, same length, space-padded)

Hi-Lo is deliberately left alone -- not wired to anything functional yet.
"""
import sys

# (file offset of text right after the opening quote, expected original, new text)
RENAMES = [
    (0x1b7beba, "Drive", "Model"),
    (0x1b7bf1b, "Tone", "Inp "),
    (0x1b7be57, "Level", "Outp "),
]


def patch_one(data, off, expected, new_text):
    if len(new_text) != len(expected):
        raise ValueError(f"replacement {new_text!r} must be exactly as long as {expected!r}")

    actual = data[off:off + len(expected)].decode("ascii")
    if actual != expected:
        raise ValueError(
            f"REFUSING: offset 0x{off:x} holds {actual!r}, expected {expected!r} "
            f"-- binary layout changed, re-verify before proceeding.")

    data[off:off + len(new_text)] = new_text.encode("ascii")
    print(f"OK  offset 0x{off:x}: {expected!r} -> {new_text!r}")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <Evil-in> <Evil-out>", file=sys.stderr)
        sys.exit(1)

    in_path, out_path = sys.argv[1:3]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    for off, expected, new_text in RENAMES:
        patch_one(data, off, expected, new_text)

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"OK  wrote {out_path}")


if __name__ == "__main__":
    main()
