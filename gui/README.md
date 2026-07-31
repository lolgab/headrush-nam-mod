# gui/ -- portable NAM-mod installer (milestones 1, 3, 4, 5)

Plain C (C11) reimplementation of `scripts/build_update_img.py`'s patch
pipeline, with no external CLI dependencies (`debugfs`, `e2fsck`, `xz`,
`mkimage`, `python3`, ARM cross-toolchain, `unzip`/`ditto`, `curl`) -- see
`.claude/plans/crystalline-baking-moonbeam.md` for the full design and the
phased rollout. Ships a GUI app (`headrush-nam-gui`, Nuklear/SDL2) that
downloads the stock firmware, applies the mod, and writes a ready-to-run
result -- no terminal needed:

- **macOS**: a patched, ad-hoc-codesigned copy of the whole `.app` bundle
  (milestone 4) -- same behavior as `scripts/quickstart_mac.sh`, just via a
  GUI. Just double-click it like the official updater.
- **Windows**: a patched, repacked copy of the 7z-SFX `.exe` updater
  (milestone 5) -- same behavior as `scripts/repack_windows_updater.py`,
  just via a GUI. The repack *logic* is thoroughly validated (see below)
  but this GUI hasn't actually been compiled/run on a real Windows host
  yet in this repo -- see app/main.c's own top comment for what's left.
- **Other OSes**: a plain `Update_nam.img` (milestone 3's "simplest output
  path", same as `scripts/quickstart_linux.sh`).

## Layout

- `core/` -- the portable pipeline: `fit_image` (FDT/FIT read+write),
  `elf_patch` (Anxiety OD vtable hijack), `qml_patch` (knob relabel),
  `ext4_image` (libext2fs-backed dump/inject/verify), `xz_codec`
  (liblzma-backed compress/decompress), `launcher_script`, `model_targets`.
- `blobs/` -- prebuilt `libnam_hook.so` / `libnam_preload.so` /
  `trampoline_gonk.bin`, built once from `patch/*.cpp` via the ARM
  cross-toolchain (see `scripts/build_update_img.py`'s `build_nam_libs()`
  for the exact flags). These never depend on user input -- only on
  `patch/*.cpp` -- so end users never need the ARM toolchain themselves.
  Currently checked in from a manual build; a CI job that rebuilds these
  whenever `patch/*.cpp` changes is a later milestone.
- `core/http_download` (libcurl-backed download) and `core/zip_reader`
  (miniz-backed zip extraction/full-tree extraction) round out the GUI's
  download path. `core/patch_pipeline` is the actual end-to-end pipeline
  (parse FIT -> decompress rootfs -> dump/patch/inject via ext4_image ->
  recompress -> rebuild FIT -> round-trip verify) -- both the CLI and the
  GUI app call this exact same function, no duplicated logic.
- `tools/gui_core_cli.c` -- milestone-1 CLI, same argv shape as
  `build_update_img.py`. Thin wrapper around `core/patch_pipeline`.
- `app/main.c` -- the GUI itself: pick a model -> downloads the stock Mac
  updater zip (same CDN URL as `scripts/quickstart_mac.sh` -- a plain zip,
  no 7z SFX involved, unlike the Windows updater, so no LZMA SDK dependency
  yet at this milestone) -> runs `core/patch_pipeline` on a background
  thread, log/progress shown live -> writes the final result (see above).
- `app/mac_package.c` (macOS only, milestone 4) -- extracts the whole
  downloaded `.app` (not just `Update.img`, via `nam_zip_extract_all`),
  swaps in the patched image, copies the bundle to the output path, and
  ad-hoc codesigns it (shells out to `cp`/`codesign`, both always present
  on macOS -- the one deliberate exception to "no shelled-out tools" here,
  matching `scripts/quickstart_mac.sh`'s own approach for this same step).
- `app/win_repack.c` (Windows target, milestone 5) -- direct port of
  `scripts/repack_windows_updater.py`: strips the (now-invalid)
  Authenticode signature pointer from the PE header, then extracts/
  recreates the embedded 7z archive with the patched `Update.img` swapped
  in. The plain-C LZMA SDK has no 7z *writer* (archive creation is only in
  7-Zip's much larger C++ codebase), so this shells out to a 7z-compatible
  CLI for extract/create -- the deliberate exception on this platform,
  mirroring `codesign`/`cp` on macOS. Production packaging should bundle a
  small `7za` binary so end users install nothing (see the `TODO` in
  `app/main.c`); dev builds just need `7z`/`7zz` on `PATH`.
- `third_party/sha1/` -- vendored public-domain SHA-1 (needed to reproduce
  the FIT image hash nodes a real `mkimage` computes).
- `third_party/miniz/` -- vendored MIT-licensed miniz (zip reading).
- `third_party/nuklear/` -- vendored public-domain Nuklear + its SDL2
  renderer backend (both single-header, from the upstream
  Immediate-Mode-UI/Nuklear repo).

## Building

```
cmake -S gui -B gui/build
cmake --build gui/build
```

Requires `e2fsprogs` (libext2fs), `xz` (liblzma), `curl` (libcurl), and
`sdl2` dev packages -- `brew install e2fsprogs xz curl sdl2` on macOS,
`apt install e2fslibs-dev liblzma-dev libcurl4-openssl-dev libsdl2-dev`
(package names vary by distro) on Linux. If SDL2 isn't found, the
`headrush-nam-gui` target is skipped with a warning and `gui-core-cli`
still builds.

## Running

CLI (same argv shape as `build_update_img.py`):

```
gui/build/gui-core-cli <stock Update.img> <out.img> --model pedalboard \
  --blobs-dir gui/blobs
```

GUI (run from the repo root, or anywhere with `gui/blobs` reachable at
`./gui/blobs` -- packaging the blobs path properly is milestone 6):

```
gui/build/headrush-nam-gui
```

Pick a model, click "Install NAM Mod" -- downloads the stock firmware,
patches it, and writes the result (a patched `.app` on macOS,
`Update_nam.img` elsewhere) in the current directory.

## Verification status

Every module was validated against the REAL Python/CLI tools it replaces
before being wired together, not just unit-tested in isolation:

- `fit_image`: round-tripped a real `mkimage`-built FIT image through
  parse -> rebuild -> re-parse; data/hash/metadata all matched. (Exact
  byte-for-byte container reproduction isn't the goal -- `mkimage` leaves
  non-spec trailing padding and a wall-clock timestamp; semantic
  equivalence is what's verified and what matters.)
- `elf_patch` / `qml_patch`: run against synthetic fixtures built to match
  real ELF/QML layouts; output byte-identical to `patch_gonkulator.py` /
  `patch_qml_labels.py`, including the refuse-on-mismatch guards.
- `ext4_image`: dump/inject/remove-and-recreate cycles cross-checked
  against real `debugfs`/`e2fsck -fn` (clean both before and after).
- `xz_codec`: cross-checked against real `xz -d`/`xz -t` both directions.
- `launcher_script`: byte-identical to `build_launcher_script()`, including
  its refuse-path.

**Full pipeline, run against real HeadRush Pedalboard 2.7 firmware**
(downloaded from the CDN URL already in `scripts/quickstart_mac.sh`),
compared against `scripts/build_update_img.py`'s own output for the same
input:

- Patched `/usr/Evil/Evil`: **byte-identical** between the two pipelines.
- Patched launcher script: **byte-identical**.
- `libnam_preload.so` as injected: **byte-identical**.
- `libnam_hook.so`: identical except for `.note.gnu.build-id` (expected --
  ELF build-id is non-deterministic across separate compiler invocations
  even from identical source; harmless, not loaded by address/content).
- `e2fsck -fn` on the resulting rootfs: clean.

This is the real correctness gate for milestone 1 (see the plan) and it
passed against production firmware, not just synthetic fixtures.

### Milestone 3 (GUI download path)

- `zip_reader`: extracted `Update.img` from the real, freshly-downloaded
  Mac updater zip; **byte-identical** to what `ditto -x -k` extracts from
  the same zip.
- `http_download`: exercised against both the real firmware CDN and a
  small real HTTPS URL, with live progress callbacks confirmed firing
  correctly through to 100%.
- `headrush-nam-gui` builds and launches (SDL2 window creation, Nuklear
  font-atlas init, and the render loop all succeed with no errors, and the
  process runs and consumes CPU actively rendering) -- but this sandbox's
  display session couldn't be screenshotted for a visual check (no
  accessibility/window-server access from this environment), so the actual
  on-screen rendering of the pick-a-model screen is **not yet visually
  confirmed**. Every piece of logic the GUI calls (download, zip extract,
  patch pipeline) is independently verified above; what's unverified is
  purely the Nuklear widget layout/rendering itself. Worth a real visual
  check on a normal desktop before relying on it.

### Milestone 4 (macOS .app packaging)

Run against the same real, freshly-downloaded Pedalboard 2.7 Mac updater
zip, with a real patched `Update.img` from the milestone-1 pipeline:

- `nam_zip_extract_all`: extracted the *entire* zip (not just
  `Update.img`); resulting file tree, every Unix permission bit, and the
  extracted `Update.img`'s bytes are all **identical** to `ditto -x -k`'s
  own extraction of the same zip.
- `nam_mac_package_app`: swapped in the patched `Update.img`, copied the
  bundle, and ad-hoc codesigned it -- `codesign -v` on the result exits 0
  (valid signature), the main executable's `rwxr-xr-x` bit survived the
  copy, and the packaged `Update.img` is byte-identical to what the
  pipeline produced. Caught and fixed a real bug in the process: `err` was
  left uninitialized on the full-success path, which would have made the
  GUI display a garbage "warning" after every successful install.

### Milestone 5 (Windows .exe repack)

Run against the real, freshly-downloaded Pedalboard 2.7 **Windows** updater
`.exe.zip` (its embedded 7z-SFX `.exe` is genuinely a Windows PE binary,
but the repack logic itself needs no Windows host -- 7z and PE-header
parsing are both platform-agnostic), compared directly against
`scripts/repack_windows_updater.py`'s own output for the same input:

- `nam_win_extract_stock_img`: extracted the stock `Update.img` from the
  real `.exe`'s embedded 7z archive; **byte-identical** to a direct `7z x`
  extraction.
- `nam_win_repack_updater`: repacked with a real patched `Update.img`.
  The PE stub through the Authenticode-zeroed header (everything before
  the 7z archive) is **byte-identical** to the Python tool's output. The
  two outputs' raw bytes differ slightly *inside* the newly-created 7z
  archive itself (7z embeds per-file timestamps, so two separately-run
  archive-creation passes never match byte-for-byte -- same class of
  non-determinism as `mkimage`'s FIT timestamp, see milestone 1) -- so
  verification extracted **all 5 files from both** `.exe`s and confirmed
  every one matches byte-for-byte, including the patched `Update.img`.
  Both outputs also pass `7z t` (archive integrity test) independently.
