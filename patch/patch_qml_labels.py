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
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import model_targets  # noqa: E402

# Pedalboard 2.7 renames (default). Other models pull their own from
# scripts/model_targets.py via --model; a model with qml_renames=None (e.g. MX5
# or Gigboard, whose knob labels come from a shared string pool) has no
# per-pedal target and should not be run through this script at all.
RENAMES = model_targets.PEDALBOARD_2_7.qml_renames


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
    ap = argparse.ArgumentParser(description="rename Anxiety OD on-screen knob labels")
    ap.add_argument("in_path", help="Evil binary in")
    ap.add_argument("out_path", help="Evil binary out")
    ap.add_argument("--model", default=None,
                    help="model target whose qml_renames to apply (default: Pedalboard 2.7)")
    args = ap.parse_args()
    in_path, out_path = args.in_path, args.out_path

    renames = RENAMES
    if args.model:
        target = model_targets.TARGETS[args.model]
        renames = target.qml_renames
        if not renames:
            print(f"REFUSING: model {args.model!r} has no QML knob relabel defined "
                  f"(qml_renames is None) -- do not run this script for it.", file=sys.stderr)
            sys.exit(2)

    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    for off, expected, new_text in renames:
        patch_one(data, off, expected, new_text)

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"OK  wrote {out_path}")


if __name__ == "__main__":
    main()
