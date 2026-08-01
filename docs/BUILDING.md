# Building the installer from source

Plain C (C11) implementation of the NAM-mod patch pipeline, with no external
CLI dependencies (`debugfs`, `e2fsck`, `xz`, `mkimage`, `python3`, ARM
cross-toolchain, `unzip`/`ditto`, `curl`). Ships a GUI app
(`headrush-nam-gui`, Nuklear/SDL2) that downloads the stock firmware,
applies the mod, and writes a ready-to-run result -- no terminal needed:

- **macOS**: a patched, ad-hoc-codesigned copy of the whole `.app` bundle.
  Just double-click it like the official updater.
- **Windows**: a patched, repacked copy of the 7z-SFX `.exe` updater. Builds
  under MSYS2's MINGW64 environment (CI does this for real -- see the
  Windows CI section below); `core/ext4_image.c`'s libext2fs dependency,
  once thought to have no Windows story at all, is built from source for
  mingw-w64 by `scripts/build_e2fsprogs_windows.sh`.
- **Other OSes**: a plain `Update_nam.img` (the simplest output path).

## Layout

- `core/` -- the portable pipeline: `fit_image` (FDT/FIT read+write),
  `elf_patch` (Anxiety OD vtable hijack), `qml_patch` (knob relabel),
  `ext4_image` (libext2fs-backed dump/inject/verify), `xz_codec`
  (liblzma-backed compress/decompress), `launcher_script`, `model_targets`.
- `blobs/` -- `libnam_hook.so` / `libnam_preload.so` / `trampoline_gonk.bin`,
  cross-compiled from `patch/*.cpp`. These never depend on user input --
  only on `patch/*.cpp` -- so end users never need the ARM toolchain
  themselves. **Pure build artifacts, deliberately NOT committed to git**
  (the repo's own root `.gitignore` already excludes `*.so`/`*.bin`): CMake
  rebuilds them automatically via `scripts/build_blobs.sh` (Docker + the
  same ARM cross-toolchain in `docker/Dockerfile`) whenever `patch/*.cpp`/
  `nam_core` changes, and every build target depends on that rebuild --
  nothing here ever trusts a stale or missing checked-in binary. Requires
  Docker running locally the first time (or whenever `patch/*.cpp`
  changes); a no-op on every build after that until something changes
  again.
- `core/http_download` (libcurl-backed download) and `core/zip_reader`
  (miniz-backed zip extraction/full-tree extraction) round out the GUI's
  download path. `core/patch_pipeline` is the actual end-to-end pipeline
  (parse FIT -> decompress rootfs -> dump/patch/inject via ext4_image ->
  recompress -> rebuild FIT -> round-trip verify) -- both the CLI and the
  GUI app call this exact same function, no duplicated logic.
- `tools/gui_core_cli.c` -- milestone-1 CLI, local dev/debugging tool.
  Thin wrapper around `core/patch_pipeline`.
- `app/main.c` -- the GUI itself: pick a model -> downloads the stock Mac
  updater zip (a plain zip, no 7z SFX involved, unlike the Windows updater,
  so no LZMA SDK dependency yet at this milestone) -> runs
  `core/patch_pipeline` on a background thread, log/progress shown live ->
  writes the final result (see above).
- `app/mac_package.c` (macOS only, milestone 4) -- extracts the whole
  downloaded `.app` (not just `Update.img`, via `nam_zip_extract_all`),
  swaps in the patched image, copies the bundle to the output path, and
  ad-hoc codesigns it (shells out to `cp`/`codesign`, both always present
  on macOS -- the one deliberate exception to "no shelled-out tools" here).
- `app/win_repack.c` (Windows target, milestone 5) -- strips the
  (now-invalid) Authenticode signature pointer from the PE header, then
  extracts/recreates the embedded 7z archive with the patched `Update.img`
  swapped in. The plain-C LZMA SDK has no 7z *writer* (archive creation is
  only in 7-Zip's much larger C++ codebase), so this shells out to a
  7z-compatible CLI for extract/create -- the deliberate exception on this
  platform, mirroring `codesign`/`cp` on macOS. Production packaging should
  bundle a small `7za` binary so end users install nothing (see the `TODO`
  in `app/main.c`); dev builds just need `7z`/`7zz` on `PATH`.
- `third_party/sha1/` -- vendored public-domain SHA-1 (needed to reproduce
  the FIT image hash nodes a real `mkimage` computes).
- `third_party/miniz/` -- vendored MIT-licensed miniz (zip reading).
- `third_party/nuklear/` -- vendored public-domain Nuklear + its SDL2
  renderer backend (both single-header, from the upstream
  Immediate-Mode-UI/Nuklear repo).
- `scripts/build_blobs.sh` / `scripts/build_blobs_native.sh` -- rebuild
  `blobs/*` (see above); `.github/workflows/gui-build.yml` runs the same
  `_native.sh` script in CI.
- `scripts/build_e2fsprogs_windows.sh` -- builds the 5 e2fsprogs libraries
  (`com_err`/`ext2fs`/`e2p`/`uuid`/`blkid`) for a Windows (mingw-w64)
  target from source (no vcpkg/MSYS2 package exists); run automatically
  by `CMakeLists.txt` on `WIN32`. See "Full Windows support" below.
- `scripts/fetch_nam_core.sh` -- vendors `nam_core/` (NeuralAmpModelerCore +
  Eigen headers) from pinned-commit GitHub/GitLab tarballs, no git
  submodule; safe to re-run (no-ops if already populated).

## Building

```sh
cmake -S . -B build
cmake --build build
```

Requires `e2fsprogs` (libext2fs), `xz` (liblzma), `curl` (libcurl), and
`sdl2` dev packages -- `brew install e2fsprogs xz curl sdl2` on macOS,
`apt install e2fslibs-dev liblzma-dev libcurl4-openssl-dev libsdl2-dev`
(package names vary by distro) on Linux. If SDL2 isn't found, the
`headrush-nam-gui` target is skipped with a warning and `gui-core-cli`
still builds. Also requires **Docker running** the first time (or
whenever `patch/*.cpp`/`nam_core` changes) to (re)build `blobs/*` -- see
the `blobs/` entry above.

## Running

CLI (local dev/debugging tool only, not built or shipped by CI):

```sh
build/gui-core-cli <stock Update.img> <out.img> --model pedalboard
```

GUI -- can be run from anywhere, not just the repo root:

```sh
build/headrush-nam-gui
```

Both binaries are fully self-contained: `blobs/*` (compiled once by
`scripts/build_blobs.sh`/`build_blobs_native.sh` via the ARM
cross-toolchain) gets embedded directly into the executable as C byte
arrays at build time (`tools/bin2c.c` generates the array sources, wired
into `CMakeLists.txt`; see `core/patch_pipeline.c`, which references them
via `core/nam_blobs_embedded.h` instead of reading files from disk). No
external files, no directory layout to preserve -- a double-clicked or
arbitrarily-relocated binary just works.

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
  real ELF/QML layouts; output byte-identical to `patch/patch_gonkulator.py` /
  `patch/patch_qml_labels.py`, including the refuse-on-mismatch guards.
- `ext4_image`: dump/inject/remove-and-recreate cycles cross-checked
  against real `debugfs`/`e2fsck -fn` (clean both before and after).
- `xz_codec`: cross-checked against real `xz -d`/`xz -t` both directions.
- `launcher_script`: byte-identical to the old Python pipeline's
  `build_launcher_script()`, including its refuse-path.

**Full pipeline, run against real HeadRush Pedalboard 2.7 firmware,**
compared against the old Python pipeline's own output for the same input:

- Patched `/usr/Evil/Evil`: **byte-identical** between the two pipelines.
- Patched launcher script: **byte-identical**.
- `libnam_preload.so` as injected: **byte-identical**.
- `libnam_hook.so`: identical except for `.note.gnu.build-id` (expected --
  ELF build-id is non-deterministic across separate compiler invocations
  even from identical source; harmless, not loaded by address/content).
- `e2fsck -fn` on the resulting rootfs: clean.

This is the real correctness gate for milestone 1 and it passed against
production firmware, not just synthetic fixtures.

### Milestone 3 (GUI download path)

- `zip_reader`: extracted `Update.img` from the real, freshly-downloaded
  Mac updater zip; **byte-identical** to what `ditto -x -k` extracts from
  the same zip.
- `http_download`: exercised against both the real firmware CDN and a
  small real HTTPS URL, with live progress callbacks confirmed firing
  correctly through to 100%.

### Milestone 4 (macOS .app packaging)

Run against a real, freshly-downloaded Pedalboard 2.7 Mac updater zip, with
a real patched `Update.img` from the milestone-1 pipeline:

- `nam_zip_extract_all`: extracted the *entire* zip (not just
  `Update.img`); resulting file tree, every Unix permission bit, and the
  extracted `Update.img`'s bytes are all **identical** to `ditto -x -k`'s
  own extraction of the same zip.
- `nam_mac_package_app`: swapped in the patched `Update.img`, copied the
  bundle, and ad-hoc codesigned it -- `codesign -v` on the result exits 0
  (valid signature), the main executable's `rwxr-xr-x` bit survived the
  copy, and the packaged `Update.img` is byte-identical to what the
  pipeline produced.

### Milestone 5 (Windows .exe repack)

Run against the real, freshly-downloaded Pedalboard 2.7 **Windows** updater
`.exe.zip`, compared directly against the old Python pipeline's own output
for the same input:

- `nam_win_extract_stock_img`: extracted the stock `Update.img` from the
  real `.exe`'s embedded 7z archive; **byte-identical** to a direct `7z x`
  extraction.
- `nam_win_repack_updater`: repacked with a real patched `Update.img`.
  The PE stub through the Authenticode-zeroed header (everything before
  the 7z archive) is **byte-identical** to the old Python tool's output.
  Verification extracted **all 5 files from both** `.exe`s and confirmed
  every one matches byte-for-byte, including the patched `Update.img`.
  Both outputs also pass `7z t` (archive integrity test) independently.

### Milestone 6 (blob build-on-the-fly)

`scripts/build_blobs.sh` run for real against Docker (not just inspected):
produces a valid ARM32 EABI5 stripped shared object and a 32-byte
trampoline, wired into CMake via `add_custom_command`/`add_dependencies` on
both `gui-core-cli` and `headrush-nam-gui`. Verified all three states of
the dependency graph: a clean build (no blobs present) triggers the Docker
rebuild; a no-op rebuild (nothing changed) does `Built target` for
everything with no Docker invocation at all; and touching
`patch/nam_hook.cpp` correctly triggers exactly one rebuild.

`.github/workflows/gui-build.yml`'s `build-blobs` job (installs the same
Bootlin toolchain directly, no Docker needed since the CI runner is
already Linux) runs `build_blobs_native.sh` directly on `ubuntu-latest`.

`build-linux`, `build-macos` (universal arm64+x86_64 via Rosetta 2), and
`build-windows` (MSYS2 MINGW64) all build `headrush-nam-gui` for real on
their respective GitHub-hosted runners, and `release` uploads all three
binaries plus a raw `Update_nam.img`-only Linux build as GitHub Release
assets on `v*` tags.

### Full Windows support: libext2fs for mingw-w64

No vcpkg or MSYS2/MinGW *binary package* for libext2fs exists. But the
source builds cleanly for mingw-w64: `configure.ac` already special-cases
`mingw*` hosts, automatically selecting `windows_io.c` (a real Win32
`CreateFile`/`ReadFile`/`WriteFile`-based I/O backend that already ships in
upstream e2fsprogs) as `default_io_manager` instead of `unix_io.c`.

Only `lib/ss` (the interactive command-subsystem library `debugfs`'s own
shell uses -- `fork()`/`wait()`/`sigprocmask()`/`sigset_t`, none of which
exist on Windows) fails to build, and it doesn't matter: this project
never links `lib/ss` at all, it only calls `ext2fs_*` functions directly.
`scripts/build_e2fsprogs_windows.sh` builds exactly the 5 needed
subdirectories.

`core/ext4_image.c` uses `default_io_manager` (a portable `#ifdef _WIN32`
macro already provided by e2fsprogs' own `ext2_io.h`) instead of hardcoding
`unix_io_manager`.

`app/win_repack.c`'s `system()` calls use real `cmd.exe` syntax (this file
only ever compiles for `_WIN32`): double-quoted paths, `if not exist X
mkdir X`, `>nul`, `cd /d`.

`.github/workflows/gui-build.yml`'s `build-windows` job builds under
MSYS2's **MINGW64** environment (`msys2/setup-msys2`, `msystem: MINGW64`)
-- native mingw-w64 gcc, producing a normal Windows `.exe` that depends
only on the mingw-w64 runtime, not MSYS's own POSIX emulation layer.
`curl`/`xz`/`SDL2` all have ready-made `mingw-w64-x86_64-*` packages;
`scripts/build_e2fsprogs_windows.sh` runs directly in that same shell to
provide the one library that doesn't. `CMakeLists.txt` builds it
automatically on `WIN32` the same "build once per checkout, never commit"
way `blobs/*` already works.
