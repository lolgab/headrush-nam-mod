"""
model_targets.py -- the handful of constants that actually differ between the
HeadRush `Evil` binaries this build can patch, so one pipeline can target more
than one model.

Only the values that vary per firmware live here: the two absolute addresses of
the Anxiety OD (v1) hijack (engine vtable + its real process() function), the
Update.img `compatible` string for auto-detect, and the QML knob-relabel offsets.
Everything else about the hijack is shared across every supported `Evil` and
lives as a constant in the code that uses it -- the process() vtable slot (8) and
vaddr->file base (0x8000) in patch_gonkulator.py, and the engine-object field
offsets (Drive/Tone/Level/bypass) in nam_hook.cpp. Those happen to be identical
across the Pedalboard, MX5, and Gigboard because it's the same
DSPModule<AnxietyOD,...> class compiled into all three; a future model that
differs would move them here.

Auto-detection matches the Update.img root `compatible` string. `--model`
always overrides. There is no default target: an unrecognized/absent
`compatible` with no explicit `--model` is an error. patch_gonkulator.py's
slot-value sanity check (the slot must already hold `orig_process_fn`) is
the backstop that makes a wrong-target selection refuse rather than corrupt
the binary.
"""
from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass(frozen=True)
class ModelTarget:
    name: str
    # Exact Update.img root `compatible` string for auto-detect (None = never
    # auto-matches; only reachable via explicit --model).
    match_compatible: Optional[str]

    # Anxiety OD (v1) hijack, re-derived per `Evil` (see patch_gonkulator.py):
    engine_vtable_vaddr: int  # AnxietyOD engine object's real vtable address-point
    orig_process_fn: int      # value the process() slot holds pre-patch == real
                              # process(); also the trampoline fallback AND the
                              # refuse-if-mismatch guard

    # patch_qml_labels.py (cosmetic knob relabel); None = skip for this model.
    # list of (file_offset, expected_text, new_text), all same length.
    qml_renames: Optional[List[Tuple[int, str, str]]] = None


# HeadRush Pedalboard 2.7 -- the original, real-hardware-confirmed target.
PEDALBOARD_2_7 = ModelTarget(
    name="pedalboard",
    match_compatible="inmusic,mg01",
    engine_vtable_vaddr=0x1839044,
    orig_process_fn=0x3260e0,
    qml_renames=[
        (0x1b7beba, "Drive", "Model"),
        (0x1b7bf1b, "Tone", "Inp "),
        (0x1b7be57, "Level", "Outp "),
    ],
)

# HeadRush MX5 2.7 (final release; compatible "inmusic,hg04"). Same RK3288 armv7
# hard-float non-PIE Evil; addresses re-derived from the final updater's Evil (see
# patch_gonkulator.py). The vtable slot, vaddr base, and engine field offsets are
# all identical to the Pedalboard (same class), so only the two addresses differ.
# QML knob relabel is disabled: MX5 has no per-pedal `headline:` QML source blob,
# its knob names come from a shared string pool (relabeling would hit every pedal).
# NOTE: 2.7 BETA 1's Evil differs (engine vtable 0x17f034c, process 0x302bc8); the
# beta and final both report compatible "inmusic,hg04", so only the final release
# is targeted here -- the beta will (correctly) fail patch_gonkulator.py's
# slot-value guard if fed to this target.
MX5_2_7 = ModelTarget(
    name="mx5",
    match_compatible="inmusic,hg04",
    engine_vtable_vaddr=0x17ee460,
    orig_process_fn=0x302ed0,
    qml_renames=None,
)

# HeadRush Gigboard 2.7 (compatible "inmusic,hg02"). Same RK3288 armv7 hard-float
# non-PIE Evil, same DSPModule<AnxietyOD,...> class -- addresses re-derived the
# same way as MX5's: RTTI xref-chase (mangled name string ->
# N8EvilDSPs9DSPModuleINS_9AnxietyODE...E -> type_info -> wrapper vtable
# 0x17df754) to locate AnxietyOD's own ctor/clone function (wrapper vtable slot
# 19, vaddr 0x2a1ee0), then read the real engine vtable straight from that
# function's literal pool (0x17f2234 -- AnxietyOD has RTTI, so like Pedalboard/
# MX5 its ctor embeds the real vtable directly, no generic-placeholder
# indirection). Slot 8 of that vtable (0x302840) was cross-checked against
# Pedalboard/MX5's known process() signature: identical strd r4/r5, strd r8/r9,
# strd r6/r7, strd r10/r11, str lr, vpush d8 prologue, and the identical
# this-relative `ldrb r3, [r0, #0x29c]` flag read; slot 15's setter also writes
# the model-select knob field at the same +0x2ac offset. ELF layout (one plain
# R+E LOAD segment, one plain R+W, >3KB zero code-cave right after the R+E
# segment's file end) matches patch_gonkulator.py's assumptions.
# QML knob relabel is disabled: like MX5, no per-pedal `propertyPath: "/Engine/
# Patch/Anxiety OD"` QML source blob was found (Gigboard's Evil only embeds QML
# source for a handful of other pedals) -- knob names come from a shared string
# pool, so relabeling would hit every pedal.
# STATUS: confirmed working on real Gigboard hardware. No publicly documented
# footswitch-hold hardware recovery mode was found for the Gigboard (unlike
# Pedalboard's "1 & 8" / MX5's "first two"); the stock firmware-update path is
# UI-driven (Global Settings > Firmware Update).
GIGBOARD_2_7 = ModelTarget(
    name="gigboard",
    match_compatible="inmusic,hg02",
    engine_vtable_vaddr=0x17f2234,
    orig_process_fn=0x302840,
    qml_renames=None,
)

TARGETS = {t.name: t for t in (PEDALBOARD_2_7, MX5_2_7, GIGBOARD_2_7)}


def select_target(model_name: Optional[str], compatible: Optional[str]):
    """Return (ModelTarget, reason_str). Explicit model_name wins; else auto-detect
    by `compatible`. No default target: raises if neither resolves a target,
    forcing an explicit --model."""
    if model_name:
        if model_name not in TARGETS:
            raise KeyError(
                f"unknown --model {model_name!r}; known: {', '.join(sorted(TARGETS))}")
        return TARGETS[model_name], f"explicit --model {model_name}"
    matches = [t for t in TARGETS.values() if t.match_compatible and t.match_compatible == compatible]
    if len(matches) == 1:
        return matches[0], f"auto-detected from compatible={compatible!r}"
    raise KeyError(
        f"compatible={compatible!r} matched no specific target; pass --model "
        f"explicitly ({', '.join(sorted(TARGETS))})")
