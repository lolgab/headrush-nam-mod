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
on the Pedalboard and MX5 because it's the same DSPModule<AnxietyOD,...> class
compiled into both; a future model that differs would move them here.

Auto-detection matches the Update.img root `compatible` string. `--model` on
build_update_img.py always overrides. patch_gonkulator.py's slot-value sanity
check (the slot must already hold `orig_process_fn`) is the backstop that makes a
wrong-target selection refuse rather than corrupt the binary.
"""
from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass(frozen=True)
class ModelTarget:
    name: str
    # Exact Update.img root `compatible` string for auto-detect (None = never
    # auto-matches; only reachable via explicit --model or as the default).
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
# Also the DEFAULT_TARGET, so an unrecognized `compatible` still falls here
# (guarded by patch_gonkulator.py's slot-value check).
PEDALBOARD_2_7 = ModelTarget(
    name="pedalboard-2.7",
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
    name="mx5-2.7",
    match_compatible="inmusic,hg04",
    engine_vtable_vaddr=0x17ee460,
    orig_process_fn=0x302ed0,
    qml_renames=None,
)

TARGETS = {t.name: t for t in (PEDALBOARD_2_7, MX5_2_7)}
DEFAULT_TARGET = "pedalboard-2.7"


def select_target(model_name: Optional[str], compatible: Optional[str]):
    """Return (ModelTarget, reason_str). Explicit model_name wins; else auto-detect
    by `compatible`; else fall back to the default (Pedalboard) -- safe because
    patch_gonkulator.py refuses if the slot doesn't hold the expected orig fn."""
    if model_name:
        if model_name not in TARGETS:
            raise KeyError(
                f"unknown --model {model_name!r}; known: {', '.join(sorted(TARGETS))}")
        return TARGETS[model_name], f"explicit --model {model_name}"
    matches = [t for t in TARGETS.values() if t.match_compatible and t.match_compatible == compatible]
    if len(matches) == 1:
        return matches[0], f"auto-detected from compatible={compatible!r}"
    return TARGETS[DEFAULT_TARGET], (
        f"defaulted to {DEFAULT_TARGET} (compatible={compatible!r} matched no "
        f"specific target; pass --model to override)")
