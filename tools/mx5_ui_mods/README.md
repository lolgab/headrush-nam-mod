# MX5 UI mods (QML source-fallback)

Standalone tools for modifying the HeadRush MX5's on-screen UI, independent of
the NAM mod. Each script takes `<input Update.img> <output Update.img>` and never
touches its input, so mods compose by chaining:

```sh
./build_tuner_tempo.py stock.img out.img
```

Requires `debugfs` (e2fsprogs), `xz`, `mkimage` (u-boot-tools), and Python 3.
Tested against **MX5 firmware 2.7** (`compatible = "inmusic,hg04"`); the scripts
refuse to run on anything else, since every offset is specific to one build of
`Evil`.

## The technique: why editing QML "does nothing", and how to fix that

`Evil` is a Qt 5.15.2 application whose entire UI is QML. It's built with
**qtquickcompiler**, so every `.qml` file is compiled ahead-of-time into QV4
bytecode (a `qv4cdata` unit) and embedded in the binary — *and the original
`.qml` source text is still embedded too*, as a normal Qt resource.

At runtime Qt uses the **compiled unit** and never parses the source. That's why
patching the readable QML in `Evil` has no visible effect (a known dead end —
see `docs/TECHNICAL.md`'s note about the knob-label patch).

But Qt *validates* a compiled unit before using it, and when the unit is
rejected it falls back to **compiling the source at runtime**. That gives a clean
lever:

1. Overwrite the page's `.qml` **source** with your edited version.
2. Corrupt one byte of that page's **compiled unit** magic (`qv4cdata` →
   `Xv4cdata`).

Qt then re-parses your edited source for that page only; every other page keeps
using its compiled unit. **Confirmed on real MX5 hardware.**

This was verified first on a host reproduction (Qt 5.15.3, same compiled-unit
format `0x29` as the device's 5.15.2): with the unit intact the compiled copy
wins and source edits are ignored; with the unit's magic corrupted, the edited
source is used. Then confirmed on the device itself.

### Keep edits the same byte length

The `.qml` source is a length-prefixed Qt resource entry. Changing its length
would require fixing that prefix *and* every subsequent entry's offset in
`qt_resource_struct`. So these mods rewrite the whole file to the **exact
original byte count** — spare room is reclaimed by collapsing indentation and
dropping a comment, then padding with spaces. `build_tuner_tempo.py` enforces
this and refuses to run if the payload length is wrong.

### Finding the offsets for another page or firmware

* **Source text**: the `.qml` files are stored as plain, concatenated text in
  `.rodata`. Search `Evil` for a distinctive literal from the page (e.g.
  `propertyPath`, a `headline:` string, an image path). The blob runs from its
  `import` line to the next NUL.
* **Compiled unit**: search the `qv4cdata` region for the page's distinctive
  string literals **encoded UTF-16-LE** (compiled units store strings as UTF-16
  and, notably, do *not* record their source filename). The unit is the last
  `qv4cdata` magic before those literals; its size is the `uint32` at
  `magic + 0x24`.

## Mods here

| Script | What it does |
|---|---|
| `build_tuner_tempo.py` | Tuner screen: tap the left/right half of the tempo box to step BPM −1/+1 (stock is read-only; only tap-tempo could change it) |

### Tuner tempo edit

The stock tuner page renders tempo read-only:

```qml
TunerBPMInfo { x: 469; y: 263; ...; headline: "Tempo"
               prop: Evil.getProperty("/Engine/TempoCtrl/Tempo") }
```

`payload/Tuner.qml` wraps that same display in a `MouseArea` and writes the
engine property directly:

```qml
MouseArea { x:469; y:263; width:316; height:60;
  property var t: Evil.getP("/Engine/TempoCtrl/Tempo");
  onClicked: t.translator.unnormalized += mouseX < width/2 ? -1 : 1;
  TunerBPMInfo { anchors.fill: parent; ...; prop: parent.t } }
```

Note it uses `.unnormalized +=` (read-modify-write) rather than the engine's
`incValue()`. An earlier attempt with `incValue(±1, 240, 40, …)` jumped straight
to the range extremes (30 / 240) instead of stepping, because its first argument
is a normalized full-scale step, not a BPM delta.

**Not done: the parameter wheel.** Making the wheel adjust tempo the way Flex
Prime does needs a C++ change — the tuner page hardwires the wheel to the
Reference parameter (`focusTunerRef` / `tunerRefFocused` on
`Gui::Pages::TunerPage`), and the generic encoder-focus mechanism
(`/Engine/RedirCtrl/EncoderFocus` + `setEncoderFocus`) is not exposed on that
page. Touch was the reachable path from QML alone.

## Recovery

Flashing custom firmware can brick the device. If a flash goes wrong, hold the
**first two footswitches** while powering on to force firmware-update mode, then
reflash a stock `Update.img`. Keep an unmodified copy. Use at your own risk.
