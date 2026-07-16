// libnam_hook.so -- Neural Amp Modeler (NAM, MIT-licensed,
// https://github.com/sdatkinson/NeuralAmpModelerCore) inference, injected into
// HeadRush Evil.
//
// Three hook entry points, for three different designs (oldest to current):
//
//   nam_process(this)  -- SUPERSEDED reference implementation. Replaces
//     IRLoaderProcessThread::process() (vtable slot 3). Rejected as the
//     shipped design because IRLoaderProcessThread's vtable is shared by
//     every IR Loader slot -- patching it kills real .wav IR loading
//     everywhere, not just in one pedal. Kept only as a proven-working
//     reference/fallback. ABI: this+0x68=float* input, this+0x6c=float*
//     output, this+0x70=int32_t num_samples (mono, this-only, no extra args).
//
//   nam_process_gonk(...) -- SUPERSEDED (Gonkulator hijack). Replaces the
//     REAL Gonkulator engine object's process() (its own vtable slot,
//     file_offset 0x1824914, vaddr 0x182c914, originally 0x2e7910), in
//     place, on the shared class -- so every Gonkulator instance loses its
//     real function board-wide. User explicitly rejected this as the final
//     design (SESSION_NOTES.md "IMPORTANT -- user explicitly rejected this").
//     Kept only as a proven-working fallback/reference now that the additive
//     path exists. ABI confirmed via PyGhidra decompile:
//       void process(EngineObj* this, uint32_t param2, float** input,
//                     uint32_t numChannels, float** output, int32_t numFrames,
//                     uint32_t* flags, void* ctx)
//     input/output are arrays of per-channel float* (numChannels is 1 or 2).
//
//   nam_process_naml(...) -- CURRENT, additive design. Same 8-arg ABI as
//     nam_process_gonk (the new engine object is a byte-for-byte clone of
//     Gonkulator's own layout, see BREAKTHROUGH #6/patch_namloader.py), but
//     this is a genuinely NEW, independent object graph -- Gonkulator's real
//     class/vtable is never written to, so real Gonkulator pedals keep
//     working normally everywhere. Also handles Input/Output trim (see
//     nam_set_input_trim/nam_set_output_trim below).
//
// All three are loaded via dlopen() by nam_preload.cpp's constructor, which
// writes the corresponding function pointer into each trampoline's hook_slot
// (see trampoline.S/trampoline_gonk.S/trampoline_naml.S/trampoline_trim.S).
//
// Model loading happens once, off the audio thread, on first process() call.
// Until it completes, audio passes through dry (never silence, never garbage).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace
{

// TODO: confirm Evil's actual engine sample rate (48000 assumed; check
// /Engine/... property tree or IRLoaderController's Reset() call site).
constexpr double kSampleRate = 48000.0;

// Folder scanned for *.nam files, mirroring how "Impulse Responses" already
// works for real IRs. Overridable via NAM_MODEL_DIR for host-side / emulated
// testing only.
std::filesystem::path model_dir()
{
  if (const char* env = std::getenv("NAM_MODEL_DIR"))
    return std::filesystem::path(env);
  return "/media/az01-internal/Evil/usb_mnt/Impulse Responses";
}

// Legacy single-fixed-file path, used only by the superseded nam_process()
// (IRLoader reference hook). Overridable via NAM_MODEL_PATH.
const char* model_path()
{
  if (const char* env = std::getenv("NAM_MODEL_PATH"))
    return env;
  return "/media/az01-internal/Evil/usb_mnt/Impulse Responses/model.nam";
}

// Knob raw-value range assumed for normalizing into a model index. HeadRush
// knobs commonly display 0-100; NOT yet confirmed against the real device
// (see SESSION_NOTES.md) -- override via NAM_KNOB_MIN/NAM_KNOB_MAX if the
// real range turns out to be different (e.g. 0.0-1.0).
float knob_min()
{
  if (const char* env = std::getenv("NAM_KNOB_MIN"))
    return std::strtof(env, nullptr);
  return 0.0f;
}

float knob_max()
{
  if (const char* env = std::getenv("NAM_KNOB_MAX"))
    return std::strtof(env, nullptr);
  return 100.0f;
}

// Lists *.nam files directly in model_dir(), sorted alphabetically by
// filename. No renaming scheme required -- whatever files are present, in
// whatever names, get indices 0..N-1 in alphabetical order. Re-scanned only
// when the knob value changes (see nam_process_gonk), never on every audio
// block, so directory I/O never happens on the real-time path.
std::vector<std::filesystem::path> scan_models()
{
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  std::filesystem::directory_iterator it(model_dir(), ec);
  if (ec)
    return files; // folder missing / USB not mounted / etc -- empty list

  for (const auto& entry : std::filesystem::directory_iterator(model_dir(), ec))
  {
    if (ec)
      break;
    if (!entry.is_regular_file(ec))
      continue;
    auto ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".nam")
      files.push_back(entry.path());
  }

  std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
    return a.filename().string() < b.filename().string();
  });
  return files;
}

// Maps a raw knob value to a model index: divides the knob's full sweep into
// N equal zones, one per *.nam file currently found (N re-evaluated on every
// switch, so adding/removing files on the USB drive changes the zone count
// live, no fixed/static slot count).
int knob_value_to_index(float raw, int file_count)
{
  if (file_count <= 0)
    return -1;
  const float lo = knob_min();
  const float hi = knob_max();
  float t = (hi > lo) ? (raw - lo) / (hi - lo) : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  int idx = static_cast<int>(t * static_cast<float>(file_count));
  return std::clamp(idx, 0, file_count - 1);
}

// Max channels we ever need to handle (Gonkulator ABI reports 1 or 2).
constexpr int kMaxChannels = 2;

struct ModelState
{
  // One independent nam::DSP instance per channel -- NAM models carry
  // internal state (WaveNet hidden state, history ring buffers), so feeding
  // L then R through a single shared instance would corrupt both channels'
  // continuity. Dual-mono: same model config, independent per-channel state.
  std::unique_ptr<nam::DSP> dsp[kMaxChannels];
  std::atomic<bool> loading{false};
  std::atomic<bool> ready{false};

  // Dynamic (Gonkulator) model-selection state.
  std::atomic<bool> switching{false};
  std::atomic<int> active_index{-1};
  // NaN sentinel guarantees the very first process() call always triggers an
  // initial scan+load, regardless of whatever raw value the knob starts at.
  std::atomic<float> last_seen_knob_raw{std::numeric_limits<float>::quiet_NaN()};
};

// Superseded nam_process()/nam_process_gonk() hooks' state.
ModelState& state()
{
  static ModelState s;
  return s;
}

// Current, additive nam_process_naml() hook's state -- deliberately a
// SEPARATE instance from state() above. The additive engine object and a
// hijacked Gonkulator instance are independent objects (possibly both
// present at once during the transition off the hijack design), so they must
// never share a "currently loaded model"/"currently switching" state.
ModelState& state_naml()
{
  static ModelState s;
  return s;
}

void load_fixed_path_in_background()
{
  auto& s = state();
  bool expected = false;
  if (!s.loading.compare_exchange_strong(expected, true))
    return; // already loading (or loaded) -- don't spawn twice

  std::thread([&s]() {
    try
    {
      const auto path = std::filesystem::path(model_path());
      std::unique_ptr<nam::DSP> loaded[kMaxChannels];
      for (auto& d : loaded)
      {
        d = nam::get_dsp(path);
        // buffer size here is advisory (prewarm sizing); real calls pass the
        // actual per-block num_samples to process() regardless.
        d->Reset(kSampleRate, 128);
      }
      for (int i = 0; i < kMaxChannels; ++i)
        s.dsp[i] = std::move(loaded[i]);
      s.ready.store(true, std::memory_order_release);
    }
    catch (...)
    {
      // Leave ready=false: process() keeps passing audio through dry.
      // (No model file yet, bad JSON, unsupported architecture, etc.)
    }
  }).detach();
}

// Kicks off (at most one concurrent) background switch: re-scan model_dir(),
// recompute the target index from `raw`, and if it differs from the
// currently active model, load it and atomically swap it in. Never touches
// the filesystem or blocks on the audio thread -- this only ever runs on a
// detached worker thread.
void switch_model_in_background(ModelState& s, float raw)
{
  bool expected = false;
  if (!s.switching.compare_exchange_strong(expected, true))
    return; // a switch is already in flight; the next process() call that
            // still sees a changed value will retry once this one finishes

  std::thread([&s, raw]() {
    // Everything in this thread body runs inside this try/catch, on purpose:
    // an exception escaping a detached std::thread's entry function calls
    // std::terminate() *without* unwinding (the Reset guard below would never
    // run), which not only kills the whole process but was observed to hang
    // rather than cleanly abort under QEMU-user ARM emulation. scan_models()
    // and std::filesystem calls can throw (permission errors, races on an
    // unplugging USB drive, etc.), so nothing here is allowed to be outside
    // a catch-all.
    try
    {
      struct Reset
      {
        std::atomic<bool>& flag;
        ~Reset() { flag.store(false, std::memory_order_release); }
      } reset{s.switching};

      auto files = scan_models();
      const int idx = knob_value_to_index(raw, static_cast<int>(files.size()));
      if (idx < 0 || idx == s.active_index.load(std::memory_order_acquire))
        return; // no files found, or knob moved but landed back on the same zone

      std::unique_ptr<nam::DSP> loaded[kMaxChannels];
      for (auto& d : loaded)
      {
        d = nam::get_dsp(files[static_cast<size_t>(idx)]);
        d->Reset(kSampleRate, 128);
      }
      for (int i = 0; i < kMaxChannels; ++i)
        s.dsp[i] = std::move(loaded[i]);
      s.active_index.store(idx, std::memory_order_release);
      s.ready.store(true, std::memory_order_release);
    }
    catch (...)
    {
      // Bad/corrupt file at this index, unsupported architecture, folder
      // missing/unmounted, etc. -- keep whatever model was previously
      // active (or stay in passthrough if none has loaded yet). s.switching
      // is still reset by the Reset guard during this catch's unwind.
    }
  }).detach();
}

} // namespace

// SUPERSEDED reference implementation -- see file header. Not used by the
// shipped design, kept for the proven IRLoader-hook fallback path.
extern "C" void nam_process(void* this_)
{
  auto* self = reinterpret_cast<uint8_t*>(this_);
  float* input = *reinterpret_cast<float**>(self + 0x68);
  float* output = *reinterpret_cast<float**>(self + 0x6c);
  const int32_t n = *reinterpret_cast<int32_t*>(self + 0x70);

  if (n <= 0 || !input || !output)
    return;

  auto& s = state();
  if (!s.ready.load(std::memory_order_acquire))
  {
    load_fixed_path_in_background();
    std::memcpy(output, input, static_cast<size_t>(n) * sizeof(float));
    return;
  }

  float* in_arr[1] = {input};
  float* out_arr[1] = {output};
  s.dsp[0]->process(in_arr, out_arr, n);
}

// Current design: replaces Gonkulator engine's process(). See file header for
// the confirmed ABI. flags/ctx are intentionally left untouched -- the
// original's tail-flush/latency bookkeeping on *flags is not replicated in
// this v1 (Gonkulator is a modulation effect, not a reverb/delay with a real
// tail, so this is expected to be a safe simplification, but is an explicit
// known limitation, not a verified-safe one).
//
// Model selection: this_+0x2ac holds the raw float value of whichever knob
// the DSPModule-level setter thunk (0x28ce00, devirtualizing to this engine
// vtable's slot 15 / 0x1a3d04) last wrote there (see SESSION_NOTES.md) --
// confirmed by static decompile of that setter, NOT re-derived here via a
// second hook. Reading it directly means no second trampoline/vtable-patch is
// needed just for knob tracking.
extern "C" void nam_process_gonk(void* this_, uint32_t /*param2*/, float** input,
                                  uint32_t numChannels, float** output, int32_t numFrames,
                                  uint32_t* /*flags*/, void* /*ctx*/)
{
  if (numFrames <= 0 || !input || !output || numChannels == 0)
    return;
  if (numChannels > kMaxChannels)
    numChannels = kMaxChannels;

  auto& s = state();

  if (this_)
  {
    const float raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + 0x2ac);
    const float prev = s.last_seen_knob_raw.exchange(raw, std::memory_order_acq_rel);
    // NaN-safe inequality: prev != prev is true only for the initial NaN
    // sentinel, guaranteeing the first call always attempts a load.
    if (raw != prev || prev != prev)
      switch_model_in_background(s, raw);
  }

  if (!s.ready.load(std::memory_order_acquire))
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
      if (input[ch] && output[ch])
        std::memcpy(output[ch], input[ch], static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }

  for (uint32_t ch = 0; ch < numChannels; ++ch)
  {
    if (!input[ch] || !output[ch])
      continue;
    float* in_arr[1] = {input[ch]};
    float* out_arr[1] = {output[ch]};
    s.dsp[ch]->process(in_arr, out_arr, numFrames);
  }
}

// ---- Input/Output trim (current, additive design only) ----
//
// Two NEW custom parameter setters, engine vtable slots 14/16 (see
// patch_namloader.py) -- deliberately NOT the shared generic float setter
// (slot 15/0x1a3d04, which stays the model-select knob). Same confirmed
// setter ABI (this=engine object in r0, value in VFP s0), but these don't
// write into the engine object's own memory at all: its internal layout
// beyond the known +0x2ac model-select field isn't fully mapped, so writing
// anywhere else risks colliding with real internal state. Storing here
// instead is exactly as safe as the process() hook's own hook_slot
// indirection and needs no assumptions about engine object internals.
//
// Value convention: NOT yet confirmed against a real knob (no UI wiring
// exists yet -- see SESSION_NOTES.md Phase 3 status). Assumed bipolar,
// roughly [-1, +1], 0 = unity/center, matching the shared setter's own
// bipolar branch-on-sign shape (BREAKTHROUGH #4). Revisit once a real
// control is actually wired to these slots.
struct TrimState
{
  std::atomic<float> input_raw{0.0f};
  std::atomic<float> output_raw{0.0f};
};

TrimState& trim_state()
{
  static TrimState t;
  return t;
}

float trim_max_db()
{
  if (const char* env = std::getenv("NAM_TRIM_MAX_DB"))
    return std::strtof(env, nullptr);
  return 12.0f;
}

float trim_gain(float raw)
{
  const float db = std::clamp(raw, -1.0f, 1.0f) * trim_max_db();
  return std::pow(10.0f, db / 20.0f);
}

extern "C" void nam_set_input_trim(void* /*this_*/, float value)
{
  trim_state().input_raw.store(value, std::memory_order_release);
}

extern "C" void nam_set_output_trim(void* /*this_*/, float value)
{
  trim_state().output_raw.store(value, std::memory_order_release);
}

// Current, additive design: replaces OUR OWN new engine object's process().
// Same 8-arg ABI as nam_process_gonk (byte-for-byte cloned engine layout, see
// patch_namloader.py's BREAKTHROUGH #6 header) but a genuinely separate
// object -- Gonkulator's real class/vtable is never written to. flags/ctx
// left untouched, same known limitation as nam_process_gonk (see its own
// comment above; carries over unchanged since this is the same engine
// shape).
//
// Model selection: identical mechanism to nam_process_gonk (this_+0x2ac,
// engine vtable slot 15's shared setter, unchanged in our cloned vtable) --
// this is the shipped file-picker per the user's decision, not a stopgap.
extern "C" void nam_process_naml(void* this_, uint32_t /*param2*/, float** input,
                                  uint32_t numChannels, float** output, int32_t numFrames,
                                  uint32_t* /*flags*/, void* /*ctx*/)
{
  if (numFrames <= 0 || !input || !output || numChannels == 0)
    return;
  if (numChannels > kMaxChannels)
    numChannels = kMaxChannels;

  auto& s = state_naml();

  if (this_)
  {
    const float raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + 0x2ac);
    const float prev = s.last_seen_knob_raw.exchange(raw, std::memory_order_acq_rel);
    if (raw != prev || prev != prev)
      switch_model_in_background(s, raw);
  }

  auto& t = trim_state();
  const float in_gain = trim_gain(t.input_raw.load(std::memory_order_acquire));
  const float out_gain = trim_gain(t.output_raw.load(std::memory_order_acquire));

  if (!s.ready.load(std::memory_order_acquire))
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
      if (input[ch] && output[ch])
        std::memcpy(output[ch], input[ch], static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }

  for (uint32_t ch = 0; ch < numChannels; ++ch)
  {
    if (!input[ch] || !output[ch])
      continue;
    // Apply input trim before NAM, output trim after -- our own design
    // choice (no further RE needed, per the approved plan).
    for (int32_t i = 0; i < numFrames; ++i)
      output[ch][i] = input[ch][i] * in_gain;
    float* in_arr[1] = {output[ch]};
    float* out_arr[1] = {output[ch]};
    s.dsp[ch]->process(in_arr, out_arr, numFrames);
    for (int32_t i = 0; i < numFrames; ++i)
      output[ch][i] *= out_gain;
  }
}

// Test/debug-only introspection, not used by the real trampoline hook path.
// Lets test harnesses synchronize on "the background switch has actually
// finished" instead of inferring it indirectly from output no longer being
// dry passthrough, which races with a switch still in flight (observed to
// cause a used-after-teardown crash in a test that exited main() while a
// switch thread was still running -- see SESSION_NOTES.md).
extern "C" int nam_debug_is_switching(void)
{
  return state().switching.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int nam_debug_active_index(void)
{
  return state().active_index.load(std::memory_order_acquire);
}

extern "C" int nam_debug_is_switching_naml(void)
{
  return state_naml().switching.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int nam_debug_active_index_naml(void)
{
  return state_naml().active_index.load(std::memory_order_acquire);
}
