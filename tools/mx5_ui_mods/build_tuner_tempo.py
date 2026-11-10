#!/usr/bin/env python3
"""
build_tuner_tempo.py -- make the tuner screen's tempo box adjustable by touch on
the HeadRush MX5 (firmware 2.7).

Stock, the tuner shows tempo read-only; the only way to change it is tap-tempo on
a footswitch. Later HeadRush hardware (Flex Prime) lets you press the tempo
display and dial it in. This back-ports the spirit of that: tapping the left half
of the tempo box decrements BPM, the right half increments it.

HOW IT WORKS (see README.md for the full derivation)
----------------------------------------------------
Evil's UI is Qt QML compiled ahead-of-time (qtquickcompiler): each page exists
BOTH as compiled QV4 bytecode (`qv4cdata` units) and as its original .qml source
in the Qt resource data. At runtime only the compiled unit is used, which is why
editing the source alone has no effect. But Qt validates a compiled unit before
using it, and falls back to parsing the source when the unit is rejected. So:

  1. overwrite Tuner.qml's SOURCE with our edited version (same byte length, so
     no Qt resource-table offsets need fixing), and
  2. corrupt one byte of Tuner.qml's COMPILED unit magic ('qv4cdata' -> 'Xv4cdata')

...and Qt re-parses our edited source for that one page. Confirmed on real MX5
hardware. Only the tuner page is affected; every other page still uses its
compiled unit.

The edit itself swaps the read-only `TunerBPMInfo` for a `MouseArea` wrapping the
same display, writing the tempo via the engine's own property tree:

    onClicked: t.translator.unnormalized += mouseX < width/2 ? -1 : 1
    property var t: Evil.getP("/Engine/TempoCtrl/Tempo")

Usage:
    ./build_tuner_tempo.py <stock Update.img> <output Update.img>

Never modifies the input. Recovery if a flash goes wrong: hold the first two
footswitches while powering on to force firmware-update mode, then reflash stock.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from update_img import UpdateImg  # noqa: E402

EVIL = "/usr/Evil/Evil"

# --- MX5 2.7 (compatible "inmusic,hg04") offsets, file offsets into /usr/Evil/Evil ---
# Tuner.qml's plain-text source in the Qt resource data.
TUNER_SRC_LO = 0x187AFC7
TUNER_SRC_HI = 0x187BDB8            # exclusive; 3569 bytes
TUNER_SRC_MAGIC = b'import "../PageController"'
# Tuner.qml's compiled QV4 unit. Located by its distinctive UTF-16 literals
# ("focusTunerRef", "tunerRefFocused") inside the qv4cdata region.
TUNER_UNIT = 0x2B08C70


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <input Update.img> <output Update.img>")
    payload = (Path(__file__).resolve().parent / "payload" / "Tuner.qml").read_bytes()
    want = TUNER_SRC_HI - TUNER_SRC_LO
    if len(payload) != want:
        sys.exit(f"ERROR: payload/Tuner.qml is {len(payload)} bytes, must be exactly {want} "
                 f"(the edit is same-length so Qt's resource offsets stay valid)")

    with UpdateImg(sys.argv[1]) as img:
        print(f"OK  {sys.argv[1]}: compatible={img.metadata['compatible']!r}")
        if img.metadata["compatible"] != "inmusic,hg04":
            sys.exit(f"REFUSING: these offsets are MX5 (inmusic,hg04) only, got "
                     f"{img.metadata['compatible']!r}")
        evil = bytearray(img.read(EVIL))

        got = bytes(evil[TUNER_SRC_LO:TUNER_SRC_LO + len(TUNER_SRC_MAGIC)])
        if got != TUNER_SRC_MAGIC:
            sys.exit(f"REFUSING: Tuner.qml source not at 0x{TUNER_SRC_LO:x} (found {got!r}) "
                     f"-- wrong firmware build or already patched")
        evil[TUNER_SRC_LO:TUNER_SRC_HI] = payload
        print(f"OK  replaced Tuner.qml source @0x{TUNER_SRC_LO:x} ({want} bytes, same length)")

        magic = bytes(evil[TUNER_UNIT:TUNER_UNIT + 8])
        if magic != b"qv4cdata":
            sys.exit(f"REFUSING: no compiled unit at 0x{TUNER_UNIT:x} (found {magic!r}) "
                     f"-- wrong firmware build or already patched")
        evil[TUNER_UNIT] = ord("X")
        print(f"OK  invalidated Tuner compiled unit @0x{TUNER_UNIT:x} "
              f"(qv4cdata -> Xv4cdata) so Qt falls back to the source")

        img.write(EVIL, bytes(evil), mode="0100755")
        img.save(sys.argv[2])
    print("\nFlash with the official updater. Test: tuner screen -> tap the left/right\n"
          "half of the tempo box to step BPM down/up.")


if __name__ == "__main__":
    main()
