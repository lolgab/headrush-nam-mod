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
//   nam_process_gonk(...) -- CURRENT hijack design, name kept from its
//     original (superseded) Gonkulator target for minimal churn. Replaces a
//     REAL engine object's process() vtable slot in place, on the shared
//     class -- so every instance of that pedal type loses its real function
//     board-wide. Tried against Gonkulator (dead code, no UI page --
//     nam_process_gonk was simply never invoked), then Volume (works
//     mechanically, but rejected: user needs Volume's real function, tied
//     to the expression pedal). Now targets ANXIETY OD (v1) -- see
//     patch_gonkulator.py for the full derivation -- the user's own choice
//     of a pedal they're fine sacrificing board-wide. ABI:
//       void process(EngineObj* this, uint32_t param2, float** input,
//                     uint32_t numChannels, float** output, int32_t numFrames,
//                     uint32_t* flags, void* ctx)
//     input/output are arrays of per-channel float* (numChannels is 1 or 2).
//
//   nam_process_naml(...) -- additive design, currently UNREACHABLE (no UI
//     path constructs its new pedal type/menu entry -- see README.md).
//     Same 8-arg ABI as nam_process_gonk (the new engine object is a
//     byte-for-byte clone of Gonkulator's own layout, see BREAKTHROUGH
//     #6/patch_namloader.py), but this is a genuinely NEW, independent
//     object graph -- whatever real class it's cloned from is never written
//     to. Also handles Input/Output trim (see nam_set_input_trim/
//     nam_set_output_trim below), which nam_process_gonk's hijack design
//     can't offer (no spare vtable slots on an existing, shared class).
//
// All three are loaded via dlopen() by nam_preload.cpp's constructor, which
// writes the corresponding function pointer into each trampoline's hook_slot
// (see trampoline.S/trampoline_gonk.S/trampoline_naml.S/trampoline_trim.S).
//
// Model loading happens once, off the audio thread, on first process() call.
// Until it completes, audio passes through dry (never silence, never garbage).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

namespace
{

// Raw pthread_create/pthread_detach instead of std::thread everywhere in
// this file. This library is compiled with -static-libstdc++/-static-libgcc
// (needed: the real device's libstdc++.so.6 is a GCC ~10 build, GLIBCXX
// 3.4.28 max, too old for symbols this cross-toolchain's libstdc++
// generates -- confirmed, dynamic linking fails outright with "GLIBCXX_
// 3.4.32 not found"), but Evil itself and libnam_preload.so both use the
// SYSTEM libstdc++.so.6 dynamically. That split -- one self-contained
// static copy of libstdc++ internals in this .so, a separate dynamic copy
// shared by the rest of the process -- segfaults (confirmed via a minimal
// QEMU reproduction) the moment std::thread's constructor runs from inside
// this library: its internal thread bookkeeping isn't the same instance the
// rest of the process's pthread/exception machinery expects. Plain
// pthread_create is a C API with no such internal C++ static state to
// duplicate, so it sidesteps the conflict entirely.
template <typename F>
void spawn_detached(F&& fn)
{
  auto* ctx = new std::decay_t<F>(std::forward<F>(fn));
  auto* trampoline = +[](void* arg) -> void* {
    auto* f = static_cast<std::decay_t<F>*>(arg);
    (*f)();
    delete f;
    return nullptr;
  };
  pthread_t tid;
  if (pthread_create(&tid, nullptr, trampoline, ctx) != 0)
  {
    delete ctx;
    return;
  }
  pthread_detach(tid);
}

// TODO: confirm Evil's actual engine sample rate (48000 assumed; check
// /Engine/... property tree or IRLoaderController's Reset() call site).
constexpr double kSampleRate = 48000.0;

// -ffast-math's crtfastmath.o sets the VFP/NEON flush-to-zero bit, but only
// on whichever thread happens to run this .so's static initializers --
// typically whatever thread calls dlopen() on us, NOT Evil's own
// already-running real-time audio thread, and NOT any thread this library
// spawns itself via pthread_create (FPSCR/FPCR is per-thread state, never
// inherited -- confirmed as the exact mechanism behind
// https://github.com/mixxxdj/mixxx/issues/16126, an almost identical
// real-hardware symptom: "full-volume digital noise" once a missing FZ bit
// let a denormal stall a real-time audio callback past its deadline).
// Without this, a WaveNet's internal hidden state naturally decays toward
// (but never quite reaches) zero during the ~100ms duck-and-switch
// fade-to-silence and during the silent prewarm blocks below -- exactly
// where denormals are most likely -- and denormal float arithmetic on ARM
// takes a slow microcoded path, a real source of the residual switch-time
// noise this is meant to close (on top of the fade/prewarm already in
// place). Cheap enough (a couple of instructions) to just call
// unconditionally at the top of every function that runs DSP::process(),
// on every thread that might do so -- there's no "set once, applies
// everywhere" version of this bit.
inline void flush_denormals_to_zero()
{
#if defined(__arm__)
  uint32_t fpscr;
  asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
  fpscr |= (1u << 24); // FZ
  asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(__aarch64__)
  uint64_t fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ull << 24); // FZ
  asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
  // Host/x86 test builds: deliberately a no-op -- SSE's own FTZ/DAZ
  // (MXCSR) is a different register this isn't attempting to manage, and
  // denormal stalls there don't reproduce the real-hardware symptom this
  // exists for.
}

// Every *.nam model this library ever constructs/prewarms/benchmarks
// (preload_models_in_background's validity check, and
// switch_model_in_background's full construct-calibrate-prewarm) happens on
// a thread WE spawned, competing for the same single ARM core as Evil's own
// real-time audio thread -- this device has no SMP to keep them apart.
// Reported on real hardware as several distinct glitches landing right at
// model construction time, both on the very first Anxiety OD engage (the
// first-ever model load) and on every later switch (which redoes this same
// construct-prewarm work for whichever model is newly selected, cache or no
// cache -- see g_calibration_cache's own comment on what IS cached).
// Lowering this thread's niceness costs nothing (this work is not latency-
// sensitive -- nothing is listening for its result except the fade logic,
// which is happy to wait) and gives the kernel's scheduler every reason to
// always favor Evil's audio thread under contention, on Linux this is
// per-thread state (setpriority(PRIO_PROCESS, 0, ...) with who=0 resolves to
// the CALLING thread's own kernel task, not the whole process -- unlike
// POSIX's nominal process-wide semantics), so this can never lower priority
// for Evil's own threads, only threads this library spawns itself.
inline void lower_background_thread_priority()
{
  setpriority(PRIO_PROCESS, 0, 19); // 19 = lowest (nicest) niceness
}

// Folder scanned for *.nam files. Originally piggybacked "Impulse
// Responses" (matching how real IRs work), but Evil's own IR-folder sync
// logic purges anything it doesn't recognize as a real IR (i.e. non-.wav)
// whenever the USB drive gets reconnected -- confirmed on real hardware,
// .nam files placed there vanish the next time the USB transfer view
// reopens. Own dedicated sibling folder instead, untouched by that sync.
// Overridable via NAM_MODEL_DIR for host-side / emulated testing only.
std::filesystem::path model_dir()
{
  if (const char* env = std::getenv("NAM_MODEL_DIR"))
    return std::filesystem::path(env);
  return "/media/az01-internal/Evil/usb_mnt/NAM";
}

// Legacy single-fixed-file path, used only by the superseded nam_process()
// (IRLoader reference hook). Overridable via NAM_MODEL_PATH.
const char* model_path()
{
  if (const char* env = std::getenv("NAM_MODEL_PATH"))
    return env;
  return "/media/az01-internal/Evil/usb_mnt/NAM/model.nam";
}

// Knob raw-value range assumed for normalizing into a model index. Current
// hijack target is AnxietyOD -- Drive/Tone/Level are all plain 0-100%
// knobs (unlike Gonkulator's hijacked "Rate" knob, which was 200-2000 Hz as
// a ring-mod carrier frequency -- kept as a cautionary note: getting this
// range wrong silently clamps every real knob position to the same index,
// via std::clamp(t, 0.0f, 1.0f) in knob_value_to_index()). Override via
// NAM_KNOB_MIN/NAM_KNOB_MAX if this still isn't exactly right.
//
// CONFIRMED via live-hardware wide-memory diffing: the real range is
// [0.0, 1.0] (normalized), not [0, 100] -- an earlier assumption made when
// the (wrong) +0x2ac offset was still believed to be the model-select
// field. All three real knobs default to 0.5 and were observed ranging
// 0.0-1.0 when swept to their extremes.
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
  return 1.0f;
}

// Moved here (from the Input/Output trim section further down) so
// nam_process_gonk can use these directly -- see its own comment on why
// Tone/Level are now read straight from the engine object instead of via a
// separate injected setter.
float trim_max_db()
{
  if (const char* env = std::getenv("NAM_TRIM_MAX_DB"))
    return std::strtof(env, nullptr);
  return 12.0f;
}

// raw01 is the knob's own [0,1] range directly (NOT pre-converted to
// bipolar) -- 0.0=true silence, 0.5=unity/0dB, 1.0=+trim_max_db() boost.
// Originally symmetric bipolar [-1,+1] mapped 0% to -trim_max_db() (still
// clearly audible, e.g. -12dB), not silence -- confirmed on real hardware
// this doesn't match the expected "0% = mute" behavior of a trim/volume
// control. Below center is a plain linear amplitude fade to true zero
// (matches how a physical volume knob feels down near its minimum); at or
// above center it's the original dB-boost curve.
float trim_gain(float raw01)
{
  if (raw01 <= 0.0f)
    return 0.0f;
  if (raw01 < 0.5f)
    return raw01 / 0.5f;
  const float bipolar_above_center = (raw01 - 0.5f) * 2.0f; // 0..1
  const float db = bipolar_above_center * trim_max_db();
  return std::pow(10.0f, db / 20.0f);
}

// Every *.nam file under model_dir() is read and JSON-parsed exactly ONCE,
// the first time Anxiety OD is actually engaged, and kept in memory for the
// rest of the boot -- model switching after that never touches the
// filesystem at all, it just re-constructs a nam::DSP from the already-
// in-memory JSON.
//
// Deliberately triggered LAZILY, not eagerly at Evil startup. Tried eager
// TWICE now, both reverted on real hardware -- see nam_preload.cpp's own
// comment (right where the eager call would go) for both failure modes:
// an unconditional-every-boot filesystem scan made the USB-transfer view
// hang worse, and separately, just spawning this thread that early (from
// the LD_PRELOAD constructor, before Evil's own main() runs) broke boot
// entirely. Keeping this conditional on real pedal use (as it always was
// pre-both-attempts) avoids both failure modes -- the "never re-scan on
// every switch" benefit (see g_preload_started below) doesn't need eager
// triggering to work, only lazy-but-cached.
struct CachedModel
{
  std::string display_name; // filename only, for sorting
  nlohmann::json config;
};

// Calibration result cache, indexed the same way as g_cached_models (one
// entry per cached model, sized once preload finishes). Kept separate from
// CachedModel itself since std::atomic isn't movable and g_cached_models is
// move-assigned as a whole when preload finishes. -1.0 = "not yet
// calibrated for this model". Real hardware showed glitches/occasional
// full-device reboots when repeatedly switching models -- every switch was
// redoing the full quality-tier benchmark from scratch (which itself
// reconstructs the internal WaveNet sub-model 2x per channel, once per tier
// tested), real, avoidable CPU/memory churn on every single switch. Caching
// per-model means only the FIRST time a given model is selected pays that
// cost; switching back to an already-seen model is instant. A plain mutex
// is fine here (calibration is rare -- once per distinct model ever
// selected -- never on the real-time audio path).
std::vector<double> g_calibration_cache;
std::mutex g_calibration_cache_mutex;

std::vector<CachedModel> g_cached_models;
std::atomic<bool> g_models_ready{false};
std::atomic<bool> g_preload_started{false};

void preload_models_in_background()
{
  bool expected = false;
  if (!g_preload_started.compare_exchange_strong(expected, true))
    return; // already started (or done) -- only ever run once per boot

  spawn_detached([]() {
    flush_denormals_to_zero(); // brand-new thread -- see its own comment
    lower_background_thread_priority(); // ditto -- see its own comment
    std::vector<CachedModel> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(model_dir(), ec))
    {
      if (ec)
        break;
      if (!entry.is_regular_file(ec))
        continue;
      // Skip macOS AppleDouble sidecar files ("._realname.nam") -- Finder
      // creates these alongside real files when copying to a FAT/exFAT USB
      // drive (metadata/resource fork it can't store natively). They carry
      // the same .nam extension but binary, non-JSON content that throws
      // when parsed. They also sort first alphabetically ('.' < any
      // letter), so left unfiltered one would silently become index 0.
      const std::string filename = entry.path().filename().string();
      if (filename.rfind("._", 0) == 0)
        continue;
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      if (ext != ".nam")
        continue;
      try
      {
        std::ifstream in(entry.path());
        nlohmann::json j;
        in >> j;
        // Fully test-construct the model here, not just parse its JSON --
        // a file can have perfectly valid JSON syntax but a corrupt/
        // malformed weight architecture that only throws once nam::get_dsp
        // actually tries to build the DSP graph from it. Confirmed on real
        // hardware: one such file caused occasional full-device reboots
        // when a switch happened to land on it, even though preload's
        // JSON-parse-only check let it through. Discarded immediately --
        // this is purely a validity check, the real DSP gets built fresh
        // per channel at actual switch time (see switch_model_in_background).
        // A file that fails this never enters g_cached_models at all, so
        // with N *.nam files and one corrupt, the pedal behaves as if there
        // are only N-1 -- exactly like the corrupt file was never there.
        { auto discard = nam::get_dsp(j); }
        found.push_back(CachedModel{filename, std::move(j)});
      }
      catch (...)
      {
        // Bad/corrupt file -- skip it, don't let one bad file block every
        // other model from loading.
      }
    }

    std::sort(found.begin(), found.end(),
              [](const CachedModel& a, const CachedModel& b) { return a.display_name < b.display_name; });

    g_calibration_cache.assign(found.size(), -1.0);
    g_cached_models = std::move(found);
    g_models_ready.store(true, std::memory_order_release);
  });
}

// Fixed 101-position knob (0%..100%, one .nam file per whole percent) instead
// of the old scheme of dividing the sweep into N equal zones sized to however
// many files were found. With N files present, only steps 0..N-1 (the first
// N% of the sweep) select a file; steps N..100 are deliberately unmapped and
// must render as silence (see nam_process_gonk's fade_state==0 branch and its
// fade-to-silence swap) rather than falling back to the previous/nearest
// file -- that would silently hide the fact that no file is assigned there.
constexpr int kKnobSteps = 101;

// Returns:
//   >= 0  -- file index to load (equal to the step number itself: step N
//            plays g_cached_models[N]).
//   -1    -- valid, resolved position, but this step has no file assigned
//            (step number >= file_count) -- caller must produce silence.
//   -2    -- no *.nam files available at all -- caller should stay in the
//            pre-ready dry-passthrough state, same as before this scheme.
int knob_value_to_index(float raw, int file_count)
{
  if (file_count <= 0)
    return -2;
  const float lo = knob_min();
  const float hi = knob_max();
  float t = (hi > lo) ? (raw - lo) / (hi - lo) : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  int step = static_cast<int>(t * static_cast<float>(kKnobSteps - 1) + 0.5f);
  step = std::clamp(step, 0, kKnobSteps - 1);
  return (step < file_count) ? step : -1;
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

  std::atomic<bool> switching{false};
  std::atomic<int> active_index{-1};
  // NaN sentinel guarantees the very first process() call always triggers an
  // initial load attempt, regardless of whatever raw value the knob starts
  // at.
  std::atomic<float> last_seen_knob_raw{std::numeric_limits<float>::quiet_NaN()};
  // Debounce for switch_model_in_background -- see its own comment.
  std::atomic<int64_t> last_switch_attempt_ms{0};
  // Knob-settle tracking (see nam_process_gonk's own comment on the
  // "3 buzzes in a row" cascade this fixes) -- audio-thread-only, no atomic
  // needed beyond what's already required for cross-thread visibility of
  // active_index/pending_index elsewhere.
  int candidate_index = -1;
  int64_t candidate_since_ms = 0;

  // Duck-and-switch model transition (see nam_process_gonk's own comment).
  // The background load thread builds the new model into pending_dsp/
  // pending_index and only sets pending_ready -- it never touches dsp[]
  // directly. Only the AUDIO THREAD ever writes dsp[]/active_index/
  // fade_progress, which also fixes a latent thread-safety bug: previously
  // the background thread swapped s.dsp[ch] directly while the audio thread
  // could be mid-call on the very same object.
  std::unique_ptr<nam::DSP> pending_dsp[kMaxChannels];
  int pending_index = -1; // set before pending_ready; read only after
  std::atomic<bool> pending_ready{false};
  // 0=steady, 1=fading out the old model, 2=fading in the new one. Atomic
  // because switch_model_in_background (background thread) reads it to
  // avoid starting a new switch while a transition is still in flight;
  // fade_progress itself is audio-thread-only, no atomic needed.
  std::atomic<int> fade_state{0};
  int32_t fade_progress = 0;

  // Which engine object (this_) this slot belongs to -- see state_for().
  std::atomic<void*> owner_this{nullptr};
};

// Anxiety OD's process() is hijacked on the SHARED CLASS VTABLE -- every
// instance of the pedal anywhere on the board calls into the same
// nam_process_gonk. A single global ModelState (the original design, when
// only one instance was ever tested) meant a second Anxiety OD instance
// would share the SAME loaded nam::DSP objects -- feeding two logically
// independent audio streams into one WaveNet's internal hidden state/history
// buffers as if they were one continuous stream. Confirmed on real hardware:
// adding a second instance produced "crazy noise", exactly the symptom of
// corrupted streaming-model state. Fixed by keying state per engine-object
// pointer instead of a single singleton.
//
// Lock-free fixed-size table (not a mutex-protected map): claimed by the
// first process() call from a never-before-seen this_, kept PERMANENTLY for
// the rest of the boot -- slots are never released, even after the owning
// pedal is deleted. This means the real consumption isn't "how many Anxiety
// OD instances exist at once" but "how many DISTINCT instances have EVER
// existed this boot", which climbs every time a pedal is added+deleted+
// re-added (e.g. during exploratory testing/undo-redo), not just from
// genuinely concurrent instances. Confirmed on real hardware: a single test
// session produced 11 distinct this_ pointers -- kMaxInstances=4 (sized for
// "a few pedals on a board at once") silently collapsed instances 5+ onto
// one shared fallback slot, reintroducing the exact cross-instance audio
// corruption the per-instance design was built to prevent. Sized generously
// now for a long exploratory session, not just a static board -- each
// unclaimed slot costs a few bytes of atomics, negligible.
constexpr int kMaxInstances = 64;
ModelState g_instances[kMaxInstances];

ModelState& state_for(void* this_)
{
  for (auto& inst : g_instances)
  {
    if (inst.owner_this.load(std::memory_order_acquire) == this_)
      return inst;
  }
  for (auto& inst : g_instances)
  {
    void* expected = nullptr;
    if (inst.owner_this.compare_exchange_strong(expected, this_, std::memory_order_acq_rel))
      return inst;
  }
  return g_instances[kMaxInstances - 1]; // all slots claimed -- degrade, don't crash
}

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

  spawn_detached([&s]() {
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
  });
}

// Minimum time between switch attempts, regardless of how often process()
// sees a changed raw value. Defensive: if a knob-value field ever turns out
// to be some other, continuously-changing internal parameter (not a stable
// UI knob), every process() call would otherwise see raw != prev and fire a
// fresh switch attempt on every single audio block. A human turning a
// physical knob does not need more than a few switch attempts per second;
// anything faster than this is either noise or a wrong-offset assumption,
// not real input.
constexpr int64_t kMinSwitchIntervalMs = 250;

// How long the knob must stay resolved to the SAME zone before a switch is
// even attempted for it -- see nam_process_gonk's own comment on the
// "several buzzes in a row on one knob turn" cascade this fixes. A real
// turning gesture sweeps through every zone between start and end; without
// this, each zone briefly passed through could start (and sometimes finish
// and get installed, each with its own full duck-and-switch fade) before the
// knob moved on. 150ms is comfortably longer than the brief hand-deceleration
// pauses within a single continuous turn, but short enough not to feel
// laggy once the knob actually stops.
constexpr int64_t kKnobSettleMs = 150;

int64_t now_ms()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A2 models expose a discrete set of quality tiers (SlimmableModel::
// SetSlimmableSize, 0.0=minimum/"Lite" .. 1.0=maximum/"Full" -- see
// https://www.tone3000.com/guides/nam-a2-the-complete-guide). Forcing 0.0
// always (the first working fix for the real-hardware glitching) is
// needlessly conservative for models with headroom to spare. This benchmarks
// every tier this specific model actually offers (GetSlimmableSizeBreakpoints
// tells us how many exist -- not all models have the same count) by running
// real inference on silent synthetic blocks at the real hardware's observed
// block size, from highest quality down, and keeps the highest tier whose
// measured time stays safely under the real-time budget. Runs entirely on
// the background load thread (SetSlimmableSize is documented "thread-safe,
// not real-time-safe"), never the audio thread, so this adds zero real-time
// risk -- worst case it takes a bit longer to finish loading.
constexpr int kCalibrationBlockSize = 48; // matches observed real hardware numFrames
constexpr int kCalibrationIters = 8;
// Require measured time under 50% of budget, not 100% (originally 70%,
// tightened after real-hardware testing -- see below). Real playing
// conditions have more jitter/contention than a clean synthetic benchmark,
// and the whole point of this feature is avoiding glitches, not chasing the
// exact edge of what's possible.
constexpr double kCalibrationSafetyMargin = 0.5;

// Every Anxiety OD instance anywhere on the board shares the SAME per-block
// real-time deadline (they're all processed within the same audio callback)
// -- but each instance's calibration used to assume it was the ONLY user of
// that budget. Confirmed on real hardware: instance A calibrated fine alone
// (691us against a 1000us budget, passed its own 70%-margin check), but once
// instance B was also active, A's real measured cost was 1503us -- both
// instances' individually-"safe" costs simply added up past the shared
// deadline. Dividing the budget by how many instances are currently claimed
// (see state_for()) doesn't retroactively fix an already-loaded instance's
// choice, but ensures every NEW calibration accounts for the others that
// already exist.
int count_claimed_instances()
{
  int n = 0;
  for (auto& inst : g_instances)
    if (inst.owner_this.load(std::memory_order_acquire) != nullptr)
      ++n;
  return n;
}

double calibrate_slimmable_quality(nam::DSP& dsp, nam::SlimmableModel& slimmable)
{
  const int concurrent_instances = std::max(1, count_claimed_instances());
  const double budget_us = static_cast<double>(kCalibrationBlockSize) / kSampleRate * 1e6 / concurrent_instances;
  const auto breakpoints = slimmable.GetSlimmableSizeBreakpoints();
  const size_t num_tiers = breakpoints.size() + 1;

  // A silent buffer risks underestimating real cost if the model (or Eigen's
  // own codepaths) has any signal-dependent fast path for near-zero input --
  // a low-amplitude sine is closer to a real, continuously-varying guitar
  // signal and won't trigger such a shortcut.
  std::vector<float> test_signal(static_cast<size_t>(kCalibrationBlockSize));
  for (int i = 0; i < kCalibrationBlockSize; ++i)
    test_signal[static_cast<size_t>(i)] =
      0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 220.0 * i / kSampleRate));
  std::vector<float> scratch(static_cast<size_t>(kCalibrationBlockSize), 0.0f);
  float* in_arr[1] = {test_signal.data()};
  float* out_arr[1] = {scratch.data()};

  for (size_t tier = num_tiers; tier-- > 0;) // highest quality first
  {
    const double ratio = (static_cast<double>(tier) + 0.5) / static_cast<double>(num_tiers);
    slimmable.SetSlimmableSize(ratio);
    dsp.Reset(kSampleRate, kCalibrationBlockSize); // re-warm at this tier before timing

    const auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < kCalibrationIters; ++iter)
      dsp.process(in_arr, out_arr, kCalibrationBlockSize);
    const auto t1 = std::chrono::steady_clock::now();
    const double avg_us = static_cast<double>(
                             std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count())
                           / static_cast<double>(kCalibrationIters);

    if (avg_us < budget_us * kCalibrationSafetyMargin)
      return ratio;
  }
  return 0.0; // nothing fit safely -- fall back to minimum/Lite
}

// Kicks off (at most one concurrent, rate-limited) background switch:
// re-constructs a nam::DSP from the already-in-memory cached JSON (see
// g_cached_models) and atomically swaps it in. Touches no filesystem at
// all -- only runs on a detached worker thread regardless.
void switch_model_in_background(ModelState& s, float raw)
{
  const int64_t now = now_ms();
  const int64_t last = s.last_switch_attempt_ms.load(std::memory_order_relaxed);
  if (now - last < kMinSwitchIntervalMs)
    return; // debounced -- too soon since the last attempt
  s.last_switch_attempt_ms.store(now, std::memory_order_relaxed);

  if (!g_models_ready.load(std::memory_order_acquire))
  {
    preload_models_in_background(); // no-op if already started -- see its own comment
    return; // not finished yet -- next raw-change (or retry, see
            // nam_process_gonk) will try again
  }

  // Don't start a new background load while a previous one's result is
  // still being duck-and-switched in by the audio thread, or is sitting
  // unpicked-up in the pending slot -- see ModelState's own comment on why
  // dsp[]/pending_dsp[] are each owned by exactly one thread.
  if (s.fade_state.load(std::memory_order_relaxed) != 0 || s.pending_ready.load(std::memory_order_acquire))
    return;

  bool expected = false;
  if (!s.switching.compare_exchange_strong(expected, true))
    return; // a switch is already in flight; the next process() call that
            // still sees a changed value will retry once this one finishes

  spawn_detached([&s, raw]() {
    // Everything in this thread body runs inside this try/catch, on purpose:
    // an exception escaping a detached thread's entry function calls
    // std::terminate() *without* unwinding (the Reset guard below would never
    // run), which not only kills the whole process but was observed to hang
    // rather than cleanly abort under QEMU-user ARM emulation.
    flush_denormals_to_zero(); // brand-new thread -- see its own comment
    lower_background_thread_priority(); // ditto -- see its own comment
    try
    {
      struct Reset
      {
        std::atomic<bool>& flag;
        ~Reset() { flag.store(false, std::memory_order_release); }
      } reset{s.switching};

      const int idx = knob_value_to_index(raw, static_cast<int>(g_cached_models.size()));
      if (idx == -2 || idx == s.active_index.load(std::memory_order_acquire))
        return; // no files found at all, or knob moved but landed back on the same state

      if (idx == -1)
      {
        // This step has no file assigned -- target is silence. Nothing to
        // build/prewarm/calibrate; just stage the silent target so the audio
        // thread ducks out the current model and swaps to nothing (see
        // nam_process_gonk's fade-complete swap and fade_state==0 branch).
        for (int i = 0; i < kMaxChannels; ++i)
          s.pending_dsp[i].reset();
        s.pending_index = idx;
        s.pending_ready.store(true, std::memory_order_release);
        return;
      }
      const nlohmann::json& config = g_cached_models[static_cast<size_t>(idx)].config;

      std::unique_ptr<nam::DSP> loaded[kMaxChannels];
      double chosen_ratio = 1.0;
      constexpr int32_t kPrewarmSamples = 16384;
      constexpr int32_t kPrewarmBlockSize = 128;
      for (int i = 0; i < kMaxChannels; ++i)
      {
        auto& d = loaded[i];
        d = nam::get_dsp(config);
        d->Reset(kSampleRate, 128); // establish sample rate/buffer size FIRST --
                                     // see the explicit manual prewarm below for
                                     // why this must happen before, not after,
                                     // any calibration/warm-up processing.
        // A2 models can run "Full" (desktop CPU) or "Lite" (pedalboard-class
        // hardware) at a large cost difference for the same capture -- see
        // https://www.tone3000.com/guides/nam-a2-the-complete-guide -- via
        // this runtime SlimmableModel interface (0.0=minimum/Lite,
        // 1.0=maximum/Full, and SlimmableWavenet defaults to 1.0/Full unless
        // told otherwise). Real-hardware profiling confirmed Full mode
        // running ~50% over this device's real-time per-block budget even
        // with -O3/-ffast-math -- a genuine compute ceiling this device's
        // ARM core can't close, not a bug. Non-slimmable models (plain
        // WaveNet/LSTM/etc, dynamic_cast fails) are unaffected -- this is a
        // no-op for them.
        if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(d.get()))
        {
          // Calibrate once (channel 0's instance -- same model/config, so
          // the same tier is safe for every channel) instead of forcing the
          // lowest tier always; apply the result to every channel afterward.
          // Cached per-model (see g_calibration_cache's own comment) --
          // real hardware showed glitches/occasional full-device reboots
          // when repeatedly switching, and redoing this full benchmark (2x
          // internal sub-model reconstruction per tier tested) on every
          // single switch was pure avoidable churn once a model has already
          // been calibrated once.
          if (i == 0)
          {
            std::lock_guard<std::mutex> lock(g_calibration_cache_mutex);
            double& cached = g_calibration_cache[static_cast<size_t>(idx)];
            if (cached < 0.0)
              cached = calibrate_slimmable_quality(*d, *slimmable);
            else
              slimmable->SetSlimmableSize(cached);
            chosen_ratio = cached;
          }
          else
            slimmable->SetSlimmableSize(chosen_ratio);
        }

        // calibrate_slimmable_quality (above, channel-0/cache-miss path only)
        // calls dsp.Reset(kSampleRate, kCalibrationBlockSize) once per tier
        // while benchmarking, which -- same as any Reset() call -- resizes
        // mMaxBufferSize to that tier's benchmark block size (48), not back to
        // the 128 set at the top of this loop. Confirmed via host-side wav
        // testing (no device/emulation needed to hit this): the very first
        // time ANY SlimmableModel-architecture .nam file is ever selected
        // (first cache miss), the manual prewarm loop right below then calls
        // process() with kPrewarmBlockSize=128 against a model whose
        // mMaxBufferSize is still 48 from calibration's last Reset --
        // `assert(num_frames <= mMaxBufferSize)` in wavenet/model.cpp fires
        // immediately. Worse than a normal crash: this runs on a
        // pthread_create'd thread (see spawn_detached's own comment), so
        // assert()'s abort() is NOT a catchable C++ exception -- the
        // surrounding try/catch never sees it, and abort() takes down the
        // WHOLE PROCESS, not just this thread. Almost certainly a real
        // contributor to the "occasional full-device reboots when repeatedly
        // switching models" noted above (that comment blamed only CPU/timing
        // churn) -- and matches the "occasional" pattern exactly, since only
        // a first-time (cache-miss) selection of a given slimmable model hits
        // it; switching back to an already-calibrated one does not re-Reset
        // and never shrinks the buffer. Unconditional and harmless for
        // non-slimmable models too (mMaxBufferSize is already 128 for them;
        // this is a no-op resize to the same value).
        d->Reset(kSampleRate, kPrewarmBlockSize);

        // Explicit manual prewarm: process silence through the model to let
        // its internal WaveNet history/hidden state settle BEFORE any real
        // (or duck-and-switch fade-in) audio ever reaches it. DSP::Reset()
        // normally does this automatically via prewarm(), but
        // SlimmableWavenet explicitly overrides GetPrewarmSamples() to
        // return 0 (no automatic prewarm at all) -- confirmed in
        // NAM/wavenet/slimmable.h. Without this, the model's FIRST real
        // samples (right at the start of the fade-in) come from a "cold"
        // model with empty history, audible as a click/glitch on real
        // hardware. ~340ms of silence at 48kHz is comfortably more than any
        // realistic guitar-amp WaveNet's receptive field.
        {
          std::vector<float> silence(kPrewarmBlockSize, 0.0f);
          std::vector<float> scratch(kPrewarmBlockSize, 0.0f);
          float* in_arr[1] = {silence.data()};
          float* out_arr[1] = {scratch.data()};
          for (int32_t done = 0; done < kPrewarmSamples; done += kPrewarmBlockSize)
            d->process(in_arr, out_arr, kPrewarmBlockSize);
        }
      }
      // Stage into pending_dsp, NOT dsp[] directly -- the audio thread
      // (nam_process_gonk) is the only thing that ever installs into dsp[],
      // once it's ready to duck-and-switch. See ModelState's own comment.
      for (int i = 0; i < kMaxChannels; ++i)
        s.pending_dsp[i] = std::move(loaded[i]);
      s.pending_index = idx;
      s.pending_ready.store(true, std::memory_order_release);
    }
    catch (...)
    {
      // Bad/corrupt cached config, unsupported architecture, etc. -- keep
      // whatever model was previously active (or stay in passthrough if
      // none has loaded yet). s.switching is still reset by the Reset guard
      // during this catch's unwind.
    }
  });
}

} // namespace

// SUPERSEDED reference implementation -- see file header. Not used by the
// shipped design, kept for the proven IRLoader-hook fallback path.
extern "C" void nam_process(void* this_)
{
  flush_denormals_to_zero(); // see its own comment -- Evil's real-time audio
                              // thread, not one we spawned ourselves
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

// Current design: replaces AnxietyOD engine's process(). See file header for
// the confirmed ABI. flags/ctx are intentionally left untouched -- the
// original's tail-flush/latency bookkeeping on *flags is not replicated in
// this v1 (an overdrive is not a reverb/delay with a real tail, so this is
// expected to be a safe simplification, but is an explicit known
// limitation, not a verified-safe one).
//
// Model selection: Drive's raw float value (this_+0x534) is read directly
// off the engine object -- confirmed via live-hardware wide-memory diffing
// (see patch_gonkulator.py's docstring and git history for the full
// derivation). No second trampoline/vtable-patch is needed just for knob
// tracking. Tone (0x548) and Level (0x520) feed input/output trim; bypass
// is this_+0x26b. See nam_process_gonk below for how each is used.

// Length of each half (fade-out, fade-in) of a model-switch transition.
// ~100ms at 48kHz. Originally 20ms (960 samples); real-hardware testing
// still showed a click at that length, requested to be made longer. Also
// combined with a fix for a separate, likely bigger contributor: the new
// model wasn't actually warmed up before the fade-in reached it (see the
// manual prewarm block in switch_model_in_background) -- a cold WaveNet
// model's first samples are a real source of audible artifacts independent
// of fade length. True crossfading (running old and new models
// simultaneously and blending outputs) was considered and rejected: this
// device's ARM core is already tightly budgeted for a single model (see
// calibrate_slimmable_quality's own comment), and running two at once
// risks a worse dropout than the glitch this is meant to fix. Duck-and-
// switch never runs two models at once, at the cost of a brief dip instead
// of a true blend.
//
// Overridable via NAM_FADE_LEN_SAMPLES -- host-side wav testing only, to
// sweep this value against real .nam models without a hardware/QEMU round
// trip each time.
//
// Default kept at the real-hardware-confirmed 4800 (100ms). Host-side
// measurement (dlopen'd libnam_hook.so, real nam_process_gonk calls, real
// .nam models, real wav, no device/QEMU) suggested click size stays flat
// down to ~40 samples, and a build with the default dropped to 128
// (~2.7ms) was flashed to real hardware -- it reintroduced audible
// distortion on model switch that the 4800-sample default did not have.
// The device's real audio thread jitter/analog output stage evidently
// doesn't behave like the host-side simulation here, so do not lower this
// default again without re-confirming on real hardware, not just host wav
// sweeps.
int32_t fade_len_samples()
{
  if (const char* env = std::getenv("NAM_FADE_LEN_SAMPLES"))
    return std::max(1, std::atoi(env));
  return 4800;
}

extern "C" void nam_process_gonk(void* this_, uint32_t /*param2*/, float** input, uint32_t numChannels,
                                  float** output, int32_t numFrames, uint32_t* /*flags*/, void* /*ctx*/)
{
  flush_denormals_to_zero(); // see its own comment -- Evil's real-time audio
                              // thread, not one we spawned ourselves
  if (numFrames <= 0 || !input || !output || numChannels == 0)
    return;
  if (numChannels > kMaxChannels)
    numChannels = kMaxChannels;

  auto& s = state_for(this_); // per-engine-instance state -- see its own comment
  const int32_t kFadeLenSamples = fade_len_samples();

  const float drive_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + 0x534); // model select
  const float tone_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + 0x548);   // input trim
  const float level_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + 0x520);  // output trim
  const uint8_t bypass_byte = *(reinterpret_cast<uint8_t*>(this_) + 0x26b);

  // Bypass is checked further down now, right before the first place it
  // actually needs to change behavior (the produced audio) -- NOT here.
  // Checking it here and returning early used to mean the knob-change
  // detection/switch-trigger/pending-pickup block below never ran at all
  // while the pedal sat bypassed, so a model never even started loading
  // until the user's first non-bypass engage -- the exact moment most
  // likely to be heard, since real audio is already flowing by then. Anxiety
  // OD commonly sits bypassed on a board from power-on until footswitched,
  // so this was effectively still a lazy-at-first-use load in disguise, not
  // eager-at-boot. Running this block regardless of bypass costs nothing
  // (no audio is produced from it either way) and means the load/duck-and-
  // switch/prewarm all happen in the background WHILE bypassed, finishing
  // (or getting much closer to finished) before the user ever un-bypasses.
  s.last_seen_knob_raw.store(drive_raw, std::memory_order_release);
  bool ready = s.ready.load(std::memory_order_acquire);
  if (!ready)
  {
    // Unconditional retry every call while not yet ready -- no settle
    // delay needed here, nothing is playing yet to protect from a
    // premature switch, so get the first model in as fast as possible.
    // switch_model_in_background's own kMinSwitchIntervalMs debounce (and
    // its `switching` in-flight guard) already makes calling this on
    // every block cheap and safe.
    //
    // Deliberately NOT gated on drive_raw having changed since the last
    // call (an earlier version was, "if changed-or-first-ever-call") --
    // confirmed on real hardware as a real "model never loads" bug: if
    // the knob sits perfectly still from power-on (the common case -- no
    // footswitch engaged yet, nobody touching Drive), the very first
    // process() call kicks off preload_models_in_background() and
    // returns (see switch_model_in_background's own !g_models_ready
    // branch) without yet having a model to install. With the old
    // change-gated retry, no later call would ever try again, since
    // drive_raw never differs from the value already stored as `prev` --
    // permanently stuck dry-passthrough. Retrying unconditionally instead
    // means the very next call after preload finishes picks it up.
    switch_model_in_background(s, drive_raw);
  }
  else
  {
    // Only start a background load once the knob has RESOLVED TO, AND
    // STAYED ON, the same zone for kKnobSettleMs -- see that constant's own
    // comment. Without this, a real turning gesture sweeping from one zone
    // to a distant one passes through every zone in between, and each one
    // it merely passed through could still start its own load; if that load
    // happened to finish before the knob moved on again, the drop-if-stale
    // check below has nothing to catch (the knob briefly WAS at that zone
    // when it was picked up) and it gets installed with a full audible
    // duck-and-switch fade -- heard on real hardware as several distinct
    // "buzz" transitions in a row from one single knob turn.
    const int target_idx = knob_value_to_index(drive_raw, static_cast<int>(g_cached_models.size()));
    if (target_idx != s.candidate_index)
    {
      s.candidate_index = target_idx;
      s.candidate_since_ms = now_ms();
    }
    else if (target_idx != -2 && target_idx != s.active_index.load(std::memory_order_acquire)
             && now_ms() - s.candidate_since_ms >= kKnobSettleMs)
    {
      switch_model_in_background(s, drive_raw);
    }
  }

  // Pick up a finished background load, if any. Only the audio thread ever
  // installs into dsp[] -- see ModelState's own comment.
  if (s.fade_state.load(std::memory_order_relaxed) == 0 && s.pending_ready.load(std::memory_order_acquire))
  {
    // The knob can move on while a load (esp. a first-time-this-boot
    // calibration, which is much slower than a cached one) is still in
    // flight -- discovered on real hardware as a fast knob sweep producing
    // several audible duck-and-switch cycles in a row, each landing on a
    // model the knob had already moved past by the time it was ready. Only
    // ever install the load that still matches where the knob actually is
    // right now; a stale one is silently dropped (no fade played for it)
    // and immediately replaced by a fresh request for the current target.
    const int current_idx = knob_value_to_index(drive_raw, static_cast<int>(g_cached_models.size()));
    if (ready && current_idx != -2 && current_idx != s.pending_index)
    {
      for (int i = 0; i < kMaxChannels; ++i)
        s.pending_dsp[i].reset();
      s.pending_ready.store(false, std::memory_order_release);
      switch_model_in_background(s, drive_raw);
    }
    else if (!ready)
    {
      // First-ever load for this instance -- nothing currently playing to
      // duck, so install directly and skip the fade entirely.
      for (int i = 0; i < kMaxChannels; ++i)
        s.dsp[i] = std::move(s.pending_dsp[i]);
      s.active_index.store(s.pending_index, std::memory_order_release);
      s.pending_ready.store(false, std::memory_order_release);
      s.ready.store(true, std::memory_order_release);
      ready = true;
    }
    else
    {
      s.fade_state.store(1, std::memory_order_relaxed); // start fading OUT the current model
      s.fade_progress = 0;
    }
  }

  // Bypass takes precedence over everything from here on, including
  // "not ready yet" -- dry passthrough unconditionally. Deliberately checked
  // here, AFTER the load/switch/pending-pickup block above, not before it --
  // see that block's own comment.
  if (bypass_byte != 0 || !ready)
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
      if (input[ch] && output[ch])
        std::memcpy(output[ch], input[ch], static_cast<size_t>(numFrames) * sizeof(float));
    return;
  }

  // Tone/Level are [0,1] with 0.5=center/unity, 0.0=silence -- trim_gain()
  // takes that range directly.
  const float in_gain = trim_gain(tone_raw);
  const float out_gain = trim_gain(level_raw);

  // Clamp the model's input feed to [-1,1] -- trim_gain() can boost input up
  // to +trim_max_db() (~4x at full boost), and a NAM model fed signal well
  // outside the level range it was captured/trained at responds with harsh,
  // sustained-sounding artifacts of its own (not a graceful overdrive), not
  // just a scaled-up version of its normal output. Different models have
  // different natural input sensitivity, so a boost that was fine for the
  // previous model can push a newly-switched-to model out of its trained
  // range -- reported on real hardware as distortion that persists after a
  // model switch, not just a transient at the switch itself.
  for (uint32_t ch = 0; ch < numChannels; ++ch)
    if (input[ch] && output[ch])
      for (int32_t i = 0; i < numFrames; ++i)
        output[ch][i] = std::clamp(input[ch][i] * in_gain, -1.0f, 1.0f);

  const int fade_state = s.fade_state.load(std::memory_order_relaxed);
  const bool silent_active = s.active_index.load(std::memory_order_relaxed) < 0;
  if (fade_state == 0)
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
      if (!input[ch] || !output[ch])
        continue;
      if (silent_active)
      {
        // This knob step has no file assigned -- render silence, not the dry
        // signal the input-clamp loop above just wrote into output[ch].
        std::fill(output[ch], output[ch] + numFrames, 0.0f);
        continue;
      }
      float* in_arr[1] = {output[ch]};
      float* out_arr[1] = {output[ch]};
      s.dsp[ch]->process(in_arr, out_arr, numFrames);
    }
  }
  else if (fade_state == 1) // fading OUT the current (about-to-be-replaced) model
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
      if (!input[ch] || !output[ch])
        continue;
      if (silent_active)
      {
        // Already silent (this step had no file assigned) -- nothing to duck
        // out of, and s.dsp[ch] is null. Just hold silence through the fade
        // window so the swap below fires on schedule.
        std::fill(output[ch], output[ch] + numFrames, 0.0f);
        continue;
      }
      float* in_arr[1] = {output[ch]};
      float* out_arr[1] = {output[ch]};
      s.dsp[ch]->process(in_arr, out_arr, numFrames);
      for (int32_t i = 0; i < numFrames; ++i)
      {
        const float g = 1.0f - std::min(1.0f, static_cast<float>(s.fade_progress + i) / kFadeLenSamples);
        output[ch][i] *= g;
      }
    }
    s.fade_progress += numFrames;
    if (s.fade_progress >= kFadeLenSamples)
    {
      // Faded to silence -- safe to swap the model now, no audible click.
      for (int i = 0; i < kMaxChannels; ++i)
        s.dsp[i] = std::move(s.pending_dsp[i]);
      s.active_index.store(s.pending_index, std::memory_order_release);
      s.pending_ready.store(false, std::memory_order_release);
      s.fade_progress = 0;
      // If the new target is itself silence (no file assigned to this step),
      // there's no model to fade IN -- go straight back to steady state,
      // where the fade_state==0 branch above already renders silence.
      s.fade_state.store(s.pending_index < 0 ? 0 : 2, std::memory_order_relaxed);
    }
  }
  else // fade_state == 2: fading IN the newly-installed model
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
      if (!input[ch] || !output[ch])
        continue;
      float* in_arr[1] = {output[ch]};
      float* out_arr[1] = {output[ch]};
      s.dsp[ch]->process(in_arr, out_arr, numFrames);
      for (int32_t i = 0; i < numFrames; ++i)
      {
        const float g = std::min(1.0f, static_cast<float>(s.fade_progress + i) / kFadeLenSamples);
        output[ch][i] *= g;
      }
    }
    s.fade_progress += numFrames;
    if (s.fade_progress >= kFadeLenSamples)
    {
      s.fade_progress = 0;
      s.fade_state.store(0, std::memory_order_relaxed); // transition complete
    }
  }

  // Same reasoning as the input clamp above: different .nam models have
  // different inherent output loudness, so an out_gain boost that didn't
  // clip the previous model can push a louder new model's output well past
  // +-1.0 -- with nothing downstream to catch it, that's raw float overflow
  // reaching the audio hardware, heard as sustained distortion for as long
  // as that model + trim setting are active, not just a switch-moment glitch.
  for (uint32_t ch = 0; ch < numChannels; ++ch)
    if (input[ch] && output[ch])
      for (int32_t i = 0; i < numFrames; ++i)
        output[ch][i] = std::clamp(output[ch][i] * out_gain, -1.0f, 1.0f);
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
  flush_denormals_to_zero(); // see its own comment -- Evil's real-time audio
                              // thread, not one we spawned ourselves
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

// Per-instance variants of the above, for nam_process_gonk's hijack path,
// which is keyed by engine-object pointer (state_for()), not the single
// state()/state_naml() singletons the four functions above read. Test/debug
// only, same as the rest of this section.
extern "C" int nam_debug_is_switching_for(void* this_)
{
  return state_for(this_).switching.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int nam_debug_active_index_for(void* this_)
{
  return state_for(this_).active_index.load(std::memory_order_acquire);
}

extern "C" int nam_debug_fade_state_for(void* this_)
{
  return state_for(this_).fade_state.load(std::memory_order_acquire);
}

extern "C" int nam_debug_pending_ready_for(void* this_)
{
  return state_for(this_).pending_ready.load(std::memory_order_acquire) ? 1 : 0;
}
