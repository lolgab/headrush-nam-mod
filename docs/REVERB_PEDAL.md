# The Reverb pedals (Dattorro plate reverb)

An additive pedal pair, **Reverb** and **Reverb 2**, next to the NAM pair
([ADDITIVE_PEDAL.md](ADDITIVE_PEDAL.md)). They appear at the end of the
**Reverb** list of the block-type picker and run a plate reverb instead of NAM.
It is a third pair next to "NAM" and "NAM TWO": 6 extra pedal ids in all.
**Opt-in and not yet flashed on hardware**:
`gui-core-cli ... --model pedalboard --reverb-pedal` (needs the NAM pedals,
the Pedalboard default). Not in the GUI.

## Algorithm

Jon Dattorro's plate reverb, *Effect Design Part 1* (JAES 45(9), 1997),
figure 1, written from the paper (no third-party code): `patch/dattorro_reverb.h`.

* Chosen because it is dense and smooth for guitar, cheap (4 input allpasses
  plus a figure-8 tank, a few hundred flops per sample) and has no licensing
  strings. Zita-rev1 / Freeverb3 are GPL or of unclear license; Freeverb is
  metallic.
* Mono in (L+R), stereo out. Bandwidth lowpass, 4 allpass diffusers
  (142/107/379/277 samples at 29761 Hz, g 0.75/0.75/0.625/0.625), then two
  cross-coupled tank loops (modulated allpass, delay, damping lowpass, decay
  gain, allpass, delay, decay gain), 7 signed taps per output side. Delay
  lengths are scaled to the engine rate (48 kHz is assumed, as for NAM).
* Fixed memory (about 380 KB per instance, static, up to 4), no allocation on
  the audio thread; control changes are smoothed (about 20 ms).
* Bypass is dry and drops the tail so it does not come back on re-engage.

## Algorithms

Four on the page, plus Shimmer through the dev control. Code in `patch/dattorro_reverb.h`, `patch/fdn_reverb.h` and `patch/room_spring_reverb.h`, run through `patch/reverb_unit.h`:

| Algorithm | What it is | Pick it with |
|---|---|---|
| **Plate** | Dattorro plate (above) | Hi-Cut On, Gain Low (default) |
| **Hall** | 8-line feedback delay network, Householder matrix, per-line RT60 gain and damping lowpass, modulated delays, 4 input diffusers. RT60 0.3 .. 14 s | Hi-Cut On, Gain High |
| **Room** | Small 8-line FDN with very short prime delays (4..14 ms), 2 input diffusers, 8 early-reflection taps (7..67 ms), barely modulated. RT60 0.15 .. 1.8 s | Hi-Cut Off, Gain Low |
| **Spring** | Two spring tanks (L / R): feedback loops of a 32-section allpass dispersion chain (the chirp), a delay and a damping lowpass, fed from a dull input. RT60 0.6 .. 5 s | Hi-Cut Off, Gain High |
| Shimmer | The hall plus an octave-up pitch shifter in its feedback (through a soft limiter). Not on the page | dev control only (`algo shimmer`) |

Decay / mix / damping mean the same in all three. The Hall's wet level is compensated over a (decay, damping) grid so
the decay knob is not also a volume knob (`reverb_lab --table`). Measured on the Mac (`patch/reverb_lab.cpp`):
hall RT60 0.5 / 1.7 / 5.3 s at decay 0.2 / 0.5 / 0.8, stable at maximum decay, wet level flat within 1 dB, shimmer tail
has the octave at -13 dB and the next octave at -29 dB re the fundamental. Not yet heard or timed on the device:
see [DEV_LOOP.md](DEV_LOOP.md) for how (`hrdev ctl stat` reports `load_pct`).

## Controls

The pedal is a Glorious Drive clone like NAM, so its three knobs keep the
captions **DRIVE / LEVEL / TONE** (they cannot be renamed, see
[NAM_PEDAL_DISPLAY.md](NAM_PEDAL_DISPLAY.md)) and show percent:

| Knob  | Reverb control | Range |
|-------|----------------|-------|
| Drive | **Decay**      | tank feedback 0.25 .. 0.93, about 0.3 s .. a long hall |
| Level | **Dry/Wet mix**| equal-power crossfade, 0 % dry .. 100 % wet |
| Tone  | **Damping**    | 0 dark .. 100 bright (tank lowpass) |

All default to 50 %.

## How it plugs in

* **6 ids need two space fixes** in `nam_elf_patch_node_registration` /
  `nam_elf_patch_engine_class` (`core/spy_patch.c`); the 4-id default build is
  byte-identical to before. (1) The node-hook blob lives in the old types array
  (668 bytes) with the `{storage, name, path}` triples after it, which no longer
  fits: the triples go to the code cave and the four `add rX, rY, #0x24` that
  formed their address become `ldr rX, [rY, #0x24]` reading a pointer stored
  there; the pedal name strings go to the cave too. (2) The R+W tail slack cannot
  hold the 8-entry engine table, so it shrinks to 6 entries and the engine
  stub's `cmp r2, #8` becomes `cmp r2, #6`.

* `core/patch_pipeline.c` registers a second `NamPair` ("Reverb") on the same
  engine class (`NAM_ENGINE_TYPE`), so the registry, picker, node and engine
  patches are the ones NAM uses; two pairs were confirmed on hardware (v23).
* `nam_process_eng` (`patch/nam_hook.cpp`) looks each engine up once in the
  engine table the engine stub fills (`{engine, &name QString}`, from
  `NAM_ENG_TAB_ADDR`); an engine whose pedal name starts with "Reverb" goes to
  `reverb_process()`, every other engine to NAM.
* The display watcher skips pedals named "Reverb*" so their knobs stay percent
  (no dB faders, no model name on Drive).

## Tests

* DSP, native: an impulse through `dattorro::Reverb` was finite and decaying at
  every decay setting, wet level about -0.5 dB relative to a noise input at
  full wet, mix 0 bit-exact dry.
* Dispatch: `patch/test_reverb_eng.c` (ARM, same harness as `test_nam_eng.c`).

## Open items

* **Engine table capacity (main risk).** The name-based dispatch only sees
  engines the stub recorded, and the table holds the first 6 (8 with 4 ids).
  If the firmware builds more engines than ids at boot (prototypes), a real
  Reverb engine can be missing from the table and would run NAM. Check the
  journal for `[nam_eng] engine ... -> Reverb`. A sturdier fix is a separate
  engine class for the Reverb pair (not done).

* Hardware: not flashed. Risks are the same as for the NAM pair plus the
  second registry pair; the name-based dispatch has only been exercised by the
  fake-table test.
* The picker's Reverb list is built by the same static initializer as the
  Distortion one; the same hook is attached to its `bl 0x59c0f0` (0x669b8,
  veneer in dead case 82 at 0x209e70). The name to pedal-id lookup is by name,
  so the category should not matter, but this is untested on hardware.
  The stock captions are a limit of the firmware UI, as for NAM. A pedal image falls back to `img/Other.png`.
* The wet level (0.6 on the tap sum) and the knob curves are by ear-free
  calibration only; adjust after listening.
