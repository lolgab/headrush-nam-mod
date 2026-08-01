# Research: an additive (non-hijacking) NAM pedal

This is the writeup for the second NAM pedal design mentioned in
[TECHNICAL.md](TECHNICAL.md): a genuinely new "Neural Amp Modeler" pedal
type, sacrificing nothing. It is **not applied by the shipped build** —
kept here as reverse-engineering notes for whoever picks up the
reachability problem.

## The idea

Instead of hijacking a real pedal's `process()` (the shipped design, see
`patch/patch_gonkulator.py`), clone an existing pedal's whole construction
shape into a brand-new, independent object graph, then give the firmware's
effect factory a new case that builds it. Nothing about the real pedal it's
cloned from is ever touched.

`patch/patch_namloader.py` builds exactly this, cloning Gonkulator's shape
(Gonkulator was chosen as the donor because it turned out to be dead code —
see the README — so nothing real depends on its layout):

- **New engine vtable** — byte copy of Gonkulator's real one, with only the
  `process()` slot repointed at a new trampoline
  (`patch/trampoline_naml.S`), and slots 14/16 repurposed as Input/Output
  trim setters (`patch/trampoline_trim.S`) — safe only because this is our
  own vtable copy, not the real Gonkulator's.
- **New descriptor cell** — byte copy of the real one, with its two
  engine-vtable-address fields repointed at the new vtable.
- **New ctor/clone function** — byte copy of Gonkulator's, with the one
  `movw`/`movt` pair that materializes the descriptor cell's address
  repointed at the new cell.
- **New DSPModule-level vtable** — byte copy of Gonkulator's 27-entry
  vtable, with only the ctor/clone slot (19) repointed at the new
  ctor/clone copy.
- **New `ModFac_construct` case** (`patch/case92_stub.S`) — the firmware's
  effect factory dispatches on a type index via
  `cmp r1,#0x5b; ldrls pc,[pc,r1,lsl 2]` at vaddr `0x209be4`/`0x209be8`
  (92 packed table entries, 0–91, no room to extend in place). The patch
  overwrites *only* the `cmp` instruction with a branch to a stub that
  special-cases `r1==92` (routing to the new pedal) and otherwise
  replicates the original comparison before resuming the real, untouched
  dispatch — cases 0–91 and the out-of-range default path are provably
  unaffected.

Every address the script touches is sanity-checked against its expected
current value before writing, and it only ever patches a copy of `Evil`.

## Why it isn't applied

**Unreachable.** Nothing in Evil's own UI — the pedal-add menu, or a saved
preset's `type` field — knows how to produce type index 92. That
menu/preset → factory-call translation was never found through static
analysis alone.

**Risk without payoff.** Even setting reachability aside, the
`ModFac_construct` dispatch instruction it patches runs on *every* pedal
construction on the board, including whatever preset loads at boot. That's
real surgery to a hot path, for a pedal nothing can select yet — not worth
the risk until reachability is solved.

Because of both, the shipped build does not invoke
`patch_namloader.py`. The pedal itself (engine, vtables, DSP hook, trim
knobs) is fully built and was validated structurally and under QEMU
emulation, but never tried on real hardware.

## The dispatch-tracing side-quest: `patch_modfac_spy.py`

One way to find the missing menu/preset → factory-call translation is a
real device with root/UART access and a debugger: breakpoint the factory
function, load any preset, read the backtrace. `patch/patch_modfac_spy.py`
is an attempt at getting the same information *without* UART — a pure
observer that patches the same `cmp r1,#0x5b` dispatch instruction to first
log `r1` (the type index about to be constructed) via a hook, then resume
the original comparison unchanged. No behavior change, just a log line per
pedal construction.

**Also blocked**, for a different reason: ARM's `B`/`BL` instruction (the
only single-instruction way to redirect that one 4-byte slot to arbitrary
code) has a ±32MB range. The nearest confirmed-safe code cave
(`patch_gonkulator.py`'s reclaimed inter-segment gap) is ~51MB away — too
far. The only region close enough *and* genuinely unmapped is the same
`0x2000`–`0x7fff` hole `patch_namloader.py` uses, which requires adding a
new `PT_LOAD` segment — and that specific mechanism (new segment +
relocated program-header table) is already confirmed, via real-device A/B
testing, to brick boot for reasons that don't reproduce under QEMU. Left
dormant in case a future session finds a legitimate nearby gap or a
different call-target-hijack approach.

## Path forward

If UART access and a debugger become available: trace the real
menu/preset → factory-call translation, wire type index 92 into it (or
whatever index the real UI reserves), wire `patch_namloader.py`'s patch
into the shipped build, and re-validate under QEMU before trying
on real hardware.
