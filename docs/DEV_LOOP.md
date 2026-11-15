# Developing without touching the device

Two loops, fastest first. Neither needs anyone at the pedalboard once the build with the Reverb pedals is flashed.

## 1. Offline lab (no device)

`patch/reverb_lab.cpp` renders the reverb algorithms on the Mac and prints what matters: RT60 at three decay settings,
stability after a loud burst at maximum decay, wet level against the decay knob, stereo decorrelation, where a test
tone ends up (octave-up check for Shimmer) and speed. It also writes impulse responses and a plucked-string phrase as
WAV files so they can be listened to.

    c++ -std=c++17 -O2 -o /tmp/reverb_lab patch/reverb_lab.cpp
    /tmp/reverb_lab /tmp/out            # numbers + WAVs in /tmp/out
    /tmp/reverb_lab --table             # regenerates the level-compensation table of the Hall (fdn_reverb.h kComp)

Change an algorithm, run the lab, only then build for ARM.

## 2. The device over USB (`tools/hrdev.py`)

Needs a base image with the **USB console** mod (the separate `mx5-usb-serial-console` fork; the Pedalboard then
enumerates a CDC serial port `/dev/cu.usbmodem*` with a root shell) and our patches on top. Our patcher accepts the
console image as its input:

    build/gui-core-cli "<USB console mod .app>/Contents/Resources/Update.img" out.img --model pedalboard --reverb-pedal
    # then copy the console .app, swap in out.img as Contents/Resources/Update.img, ditto --noextattr + codesign -s -

Flash that once; after that `hrdev deploy` replaces only `libnam_hook.so` (the DSP), which is what changes while
developing algorithms. A changed registry (new pedal ids, picker, engine stub) still needs a flash. Needs pyserial:

    python3 -m venv tools/.venv && tools/.venv/bin/pip install pyserial
    alias hrdev='tools/.venv/bin/python tools/hrdev.py'

macOS hangs on a second `open()` of that port, so the first `hrdev` command starts a small daemon that keeps the
port open (`/tmp/hrdev.sock`, log `/tmp/hrdev.log`); `hrdev stop` ends it. After a replug: `hrdev stop`, then any command.

| Command | What it does |
|---|---|
| `hrdev status` | firmware, which `libnam_hook.so` is active, the last dev status |
| `hrdev deploy` | pushes `blobs/libnam_hook.so`, bind-mounts it over `/usr/Evil/libnam_hook.so`, restarts Evil (about 20 s, no flash, a reboot reverts it) |
| `hrdev sh <cmd>` / `push` / `pull` | shell command, file copy (gzip + base64) |
| `hrdev journal [pattern]` | journal of this boot, default the `[nam_*]` lines |
| `hrdev ctl <lines>` | reverb dev control (below), prints `/tmp/nam_dev.out` |
| `hrdev capture <secs> out.wav` | records the reverb output on the device (max 8 s) and pulls the WAV |

### Reverb dev control

The hook's control thread polls `/tmp/nam_ctl` (see `devctl` in `patch/nam_hook.cpp`). With an engine named "Reverb"
present (add a Reverb pedal to a rig once):

    hrdev ctl "algo shimmer" "knob decay 0.7" "knob mix 0.5" "inject pluck 4 0.4 196"
    hrdev ctl stat                      # algo in use, CPU per block vs. budget, overruns, peak, level
    hrdev capture 6 /tmp/shimmer.wav    # while the injection plays

* `algo plate|hall|shimmer|auto` forces an algorithm (auto: the pedal's Gain Low/High switch).
* `knob decay|mix|damp <0..1|auto>` overrides a knob; `knob auto` releases all.
* `inject impulse|noise|sine|pluck <secs> [amp] [hz]` replaces the guitar with a test signal for that long.
* `capture <secs>`, `dump` (raw bytes of the Reverb engine, to find where other switches live), `reset`, `stat`.

`load_pct` in the status is the average time of one `rv.process()` block against the time the block represents. It
is the number that decides whether an algorithm fits next to four NAM models.

## Detail pages on a live device (`tools/qml_patch.py`)

`qml_patch.py patch <Evil> <regions> --key /Pages/BlockDetails.qml --old '<text>' --new '<text>'` writes the byte
regions of an edited QML source plus its key break, `qml_patch.py apply <regions>.json` pushes them to the device
(verified by md5), builds `/tmp/Evil.<ts>`, bind-mounts it over `/usr/Evil/Evil` and restarts. Pages themselves are
plain files: copy them to `/tmp/ui/` with `hrdev push` and point the Loader there (append `?<timestamp>` so Qt does not
reuse a cached component). Before the first restart after a reboot, the serial shell must not have its working
directory inside `usb_mnt` (the restart would fail to unmount the USB image). See [QML_PAGES.md](QML_PAGES.md).
