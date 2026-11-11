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
//     of a pedal they're fine sacrificing board-wide. In the optional
//     "up to 4 instances" build, the SAME hook function is also wired to
//     Anxiety OD V2's separate engine vtable (own hijack, own hook_slot,
//     own env var -- see nam_preload.cpp) -- it dispatches per-instance by
//     engine-object pointer (state_for()) regardless of which pedal class
//     called in, so no V2-specific code exists here at all. ABI:
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

#include <dlfcn.h>
#include <cstdarg>
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
#include <sys/uio.h>
#include <type_traits>
#include <map>
#include <unordered_map>
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

// dB-scale trim for the additive NAM pedals: -Inf .. +12 dB over the fader's 0..1 range, 0 dB at the default
// (0.5). Below center the gain falls linearly in dB down to -60 dB at the very bottom (a plain dB taper), and the
// last few percent fade on to true silence, so the bottom of the travel is "-Inf".
float trim_gain_db(float raw01)
{
  if (raw01 <= 0.0f)
    return 0.0f;
  float db;
  float extra = 1.0f;
  if (raw01 < 0.5f)
  {
    db = -60.0f * (1.0f - raw01 * 2.0f);
    if (raw01 < 0.03f)
      extra = raw01 / 0.03f;
  }
  else
  {
    db = (raw01 - 0.5f) * 2.0f * trim_max_db();
  }
  return extra * std::pow(10.0f, db / 20.0f);
}

// The FX-Loop's SEND/RETURN faders (-60..+12 dB, "%.1f dB") -- what the additive NAM pedals' Level/Tone show once
// their translators were cloned from those (nam_eng_init): dB is linear in the fader position.
float trim_gain_fx(float raw01)
{
  if (raw01 <= 0.004f)
    return 0.0f;
  return std::pow(10.0f, (-60.0f + 72.0f * std::min(raw01, 1.0f)) / 20.0f);
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
// move-assigned as a whole when preload finishes. ratio<0.0 = "not yet
// calibrated for this model". Real hardware showed glitches/occasional
// full-device reboots when repeatedly switching models -- every switch was
// redoing the full quality-tier benchmark from scratch (which itself
// reconstructs the internal WaveNet sub-model 2x per channel, once per tier
// tested), real, avoidable CPU/memory churn on every single switch. Caching
// per-model means only the FIRST time a given model is selected pays that
// cost; switching back to an already-seen model is instant. A plain mutex
// is fine here (calibration is rare -- once per distinct model ever
// selected -- never on the real-time audio path).
//
// `epoch` records which g_topology_epoch generation a cached ratio was
// computed against. A ratio computed when only 2 instances shared the
// board's CPU budget is too generous once a 3rd/4th instance joins and the
// per-instance share shrinks -- see g_topology_epoch's own comment. Tagging
// each cache entry with the epoch it was computed under (instead of
// clearing the whole cache on every topology change, which would need a
// mutex lock from the audio thread) means the existing cache-miss path
// below transparently recomputes the FIRST time -- for any reason, a live
// switch or a topology-triggered forced reload -- any model is touched
// after the epoch it was cached under goes stale.
struct CalibrationEntry
{
  double ratio = -1.0;
  int epoch = -1;
};
std::vector<CalibrationEntry> g_calibration_cache;
std::mutex g_calibration_cache_mutex;

// Bumped every time state_for() claims a brand-new instance slot (i.e. a
// pedal instance calls in for the very first time this boot) -- see
// count_claimed_instances()/calibrate_slimmable_quality() for why the
// shared per-block CPU budget shrinks every time another instance joins.
// Lock-free (plain atomic increment, no mutex) so bumping it from the audio
// thread (state_for() runs there) never risks blocking on a background
// thread holding g_calibration_cache_mutex.
std::atomic<int> g_topology_epoch{0};

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

    g_calibration_cache.assign(found.size(), CalibrationEntry{});
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
  std::atomic<int> cal_state{-1}; // last Hi-Lo / Gain state seen (for the log line on a change)
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

  // Topology epoch (see g_topology_epoch) this instance last completed a
  // load/reload under. Compared against the live g_topology_epoch in
  // nam_process_gonk's steady-state path; a mismatch means another
  // instance has joined the board since, shrinking everyone's shared
  // per-block budget, so this instance is forced through the same duck-
  // and-reload cycle a knob-driven switch uses to recalibrate its quality
  // tier. Only ever written by the audio thread (at install time), so
  // relaxed load/store would suffice, but kept acquire/release to match
  // the rest of this struct's cross-thread-visible fields.
  std::atomic<int> calibrated_epoch{-1};
  // Set (audio-thread-only, no atomic needed -- same thread that reads it)
  // when the CURRENT fade-out/reload cycle was armed by a topology-epoch
  // staleness check rather than a genuine knob-driven model change. Bypasses
  // the "knob settled back on the same model, no background work needed"
  // shortcut so the cycle actually reaches switch_model_in_background and
  // recalculates the quality tier. Cleared only once the reload is actually
  // installed (not merely attempted), so the existing fade_state==3 retry
  // safety-net still re-requests a forced reload if the first attempt was
  // lost (debounced / lost the `switching` CAS).
  bool force_recalibrate = false;

  // Which engine object (this_) this slot belongs to -- see state_for().
  std::atomic<void*> owner_this{nullptr};

  // Diagnostics (additive NAM pedals): process() calls seen, for the journal heartbeat in nam_process_common.
  std::atomic<uint32_t> calls{0};

  // Additive NAM pedals: set once the Level/Tone faders' translators were cloned from the FX-Loop's dB ones, so the
  // trim gain must follow that display's curve (see trim_gain_fx).
  std::atomic<bool> disp_db{false};
  std::atomic<bool> is_eng{false};
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
    {
      // A brand-new instance just joined the shared per-block CPU budget --
      // see g_topology_epoch's own comment. Lock-free increment only; the
      // per-model cache itself is left alone here (no mutex from the audio
      // thread) and instead goes stale lazily, tagged by epoch.
      g_topology_epoch.fetch_add(1, std::memory_order_release);
      return inst;
    }
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
//
// `force_reload_same_model`: bypasses the "knob landed back on the model
// already active -- nothing to do" early-return below, for a topology-
// triggered retroactive recalibration (see ModelState::force_recalibrate)
// that deliberately wants to reconstruct the SAME model, so its quality
// tier gets recomputed against the (now smaller) per-instance budget --
// see calibrate_slimmable_quality/g_topology_epoch.
void switch_model_in_background(ModelState& s, float raw, bool force_reload_same_model = false)
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
  // dsp[]/pending_dsp[] are each owned by exactly one thread. fade_state==3
  // (already muted, waiting on an earlier request) is allowed through: the
  // pickup block's stale-pending-drop path calls back in here to replace a
  // load the knob has already moved past, while staying muted throughout.
  const int fade_state_now = s.fade_state.load(std::memory_order_relaxed);
  if ((fade_state_now != 0 && fade_state_now != 3) || s.pending_ready.load(std::memory_order_acquire))
    return;

  bool expected = false;
  if (!s.switching.compare_exchange_strong(expected, true))
    return; // a switch is already in flight; the next process() call that
            // still sees a changed value will retry once this one finishes

  spawn_detached([&s, raw, force_reload_same_model]() {
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
      if (idx == -2 || (idx == s.active_index.load(std::memory_order_acquire) && !force_reload_same_model))
        return; // no files found at all, or knob moved but landed back on the same state
                // (unless a forced recalibration deliberately wants this same model rebuilt)

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
            auto& entry = g_calibration_cache[static_cast<size_t>(idx)];
            const int live_epoch = g_topology_epoch.load(std::memory_order_acquire);
            // Stale-by-epoch counts as a miss too, not just never-calibrated
            // -- see CalibrationEntry's own comment.
            if (entry.ratio < 0.0 || entry.epoch != live_epoch)
            {
              entry.ratio = calibrate_slimmable_quality(*d, *slimmable);
              entry.epoch = live_epoch;
            }
            else
              slimmable->SetSlimmableSize(entry.ratio);
            chosen_ratio = entry.ratio;
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
          // Real-hardware symptom: audible cracks/artifacts during this
          // window even though nam_process_gonk now mutes the audio thread
          // (fade_state==3, plain silence, no dsp->process() calls at all)
          // for the ENTIRE duration of this loop -- so the audio thread
          // itself has nothing expensive to do, yet still isn't getting
          // scheduled reliably. On a single ARM core, a tight, syscall-free
          // loop only actually gets preempted at a scheduler tick boundary
          // (can be several ms on an embedded kernel's HZ), regardless of
          // this thread's niceness -- niceness affects how MUCH CPU share a
          // thread gets over time, not how promptly it yields the CPU
          // mid-burst. Real hardware's own audio callback period is ~1ms
          // (kCalibrationBlockSize=48 "matches observed real hardware
          // numFrames"), so any uninterrupted burst much longer than that
          // risks missing its wakeup -- an underrun/dropout, heard as a
          // crack. Yielding every 8 blocks (~21ms bursts) was too coarse;
          // yield after EVERY block instead (~2.7ms bursts, close to the
          // real audio period) so the gap between forced yields is never
          // much bigger than one real audio callback's worth of work.
          for (int32_t done = 0; done < kPrewarmSamples; done += kPrewarmBlockSize)
          {
            d->process(in_arr, out_arr, kPrewarmBlockSize);
            usleep(500);
          }
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

// Model-switch transition used to use one LINEAR ramp length for both
// halves (fade-out and fade-in). A linear ramp's gain is flat (slope 0)
// right up until the fade starts, then jumps to a constant nonzero slope --
// a discontinuity in the slope itself, which is an impulsive, broadband
// ("click"/"zz") artifact independent of whatever the model is doing.
// Shortening that old single length 4800->128 samples (~2.7ms) was flashed
// to real hardware and made the click WORSE, not better -- consistent with
// this theory (steeper ramp = bigger slope jump = louder click), not with
// "fade wasn't short enough yet". Do not reintroduce a single-length linear
// ramp; see raised_cosine_ramp() below, which has zero slope at both
// endpoints and removes the discontinuity regardless of length.
//
// Fade-out and fade-in are independently sized, both 100ms:
// - Fade-out: tried short (~5ms) once the ramp-shape fix above stopped it
//   clicking on its own, but real hardware still showed artifacts -- see
//   nam_process_gonk's own comment: the background load/calibrate/prewarm is
//   now deliberately NOT started until fade-out has fully finished (rather
//   than concurrently with it), so a slower, unhurried 100ms duck costs
//   nothing extra in when the new model becomes available, and avoids
//   whatever a too-fast duck of a still-live model was doing on its own.
// - Fade-in (100ms): the new model was
//   prewarmed on SILENCE (see switch_model_in_background's manual prewarm
//   block) before this fade-in ever starts, so it's not truly "cold" --
//   still, ramping the model's INPUT, not just its output (see the
//   fade_state==2 branch below), avoids handing it an instant full-amplitude
//   step, which is a plausible source of a small transient/overshoot even
//   from a silence-settled state. 100ms is a starting guess, not yet
//   hardware-confirmed at this specific value -- shorten via
//   NAM_FADE_IN_LEN_SAMPLES for host-side comparison, but only trust a real
//   flash+listen test before shipping a change here.
//
// Both overridable via NAM_FADE_OUT_LEN_SAMPLES / NAM_FADE_IN_LEN_SAMPLES --
// host-side wav testing only, to sweep these without a hardware/QEMU round
// trip each time. Do not lower the shipped defaults without re-confirming on
// real hardware, not just host wav sweeps -- see the linear-ramp history
// above for why host-side sweeps alone were previously misleading.
int32_t fade_out_len_samples()
{
  if (const char* env = std::getenv("NAM_FADE_OUT_LEN_SAMPLES"))
    return std::max(1, std::atoi(env));
  return 4800; // 100ms at 48kHz
}

int32_t fade_in_len_samples()
{
  if (const char* env = std::getenv("NAM_FADE_IN_LEN_SAMPLES"))
    return std::max(1, std::atoi(env));
  return 4800; // 100ms at 48kHz
}

// Equal-power-ish raised-cosine ramp: 0 at progress<=0, 1 at progress>=len,
// smooth (zero slope) at both ends. Used for both fade-out (as 1-this) and
// fade-in so neither transition has the linear ramp's slope discontinuity.
float raised_cosine_ramp(int32_t progress, int32_t len)
{
  const float x = std::clamp(static_cast<float>(progress) / static_cast<float>(len), 0.0f, 1.0f);
  return 0.5f * (1.0f - std::cos(static_cast<float>(M_PI) * x));
}

// Where the three knobs and the bypass byte live inside the hijacked engine object, plus how the trims are
// curved. Anxiety OD's and the additive NAM pedals' engines (a Glorious Drive clone) have the same shape at
// different offsets.
struct KnobLayout
{
  uint32_t model, input, output, bypass, hilo;
  bool db_trim;
};

// Hi-Lo switch = input calibration + output loudness normalization on/off (GitHub issue #20). When on, the input is scaled so the model's own
// calibrated input level (nam::DSP::GetInputLevel(), dBu at 0 dBFS in the training capture) lines up with the
// interface's level -- the same rule the NAM plugin uses: gain_dB = interface_dBu - model_dBu. Models without an
// input level in their metadata are left alone. The Hi-Lo field's offset (one 0x14 slot after Tone, like Drive/Tone/
// Level) is NOT hardware-verified for Anxiety OD; on the additive NAM pedals it is (their "Gain" Low/High switch is the
// byte at 0x57c, 0 = Low, 1 = High = calibration on). NAM_HILO_OFFSET overrides it, the heartbeat log prints the raw word.
float calibration_interface_dbu()
{
  if (const char* env = std::getenv("NAM_CAL_DBU"))
    return std::strtof(env, nullptr);
  return 12.2f; // NAM plugin default
}

float calibration_max_db()
{
  return 24.0f; // keep a bogus metadata value from turning into a wall of noise
}

// The switch is a two-state enum: its low byte is 0 / 1, and other flags share the same word (the additive pedals read
// 0x00010000 off and 0x00010001 on), so the byte is tested, not the whole word. A float 1.0f is accepted too.
bool hilo_is_on(const void* this_, uint32_t offset)
{
  if (const char* env = std::getenv("NAM_HILO_OFFSET"))
    offset = static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
  uint32_t word;
  std::memcpy(&word, static_cast<const uint8_t*>(this_) + offset, sizeof word);
  float f;
  std::memcpy(&f, &word, sizeof f);
  return (word & 0xffu) == 1u || (std::isfinite(f) && f > 0.5f && f < 1.5f);
}

// Output side of the same switch: normalize the model's own loudness (GetLoudness(), dB) to a common target so
// switching models keeps roughly the same volume. Same rule as the NAM plugin (target -18 dB).
float calibration_target_loudness_db()
{
  if (const char* env = std::getenv("NAM_CAL_LOUDNESS_DB"))
    return std::strtof(env, nullptr);
  return -18.0f;
}

float calibration_output_gain(nam::DSP* dsp)
{
  if (!dsp || !dsp->HasLoudness())
    return 1.0f;
  const float db = std::clamp(calibration_target_loudness_db() - static_cast<float>(dsp->GetLoudness()),
                              -calibration_max_db(), calibration_max_db());
  return std::pow(10.0f, db / 20.0f);
}

float calibration_gain(nam::DSP* dsp)
{
  if (!dsp || !dsp->HasInputLevel())
    return 1.0f;
  const float db = std::clamp(calibration_interface_dbu() - static_cast<float>(dsp->GetInputLevel()),
                              -calibration_max_db(), calibration_max_db());
  return std::pow(10.0f, db / 20.0f);
}

static void nam_process_common(void* this_, float** input, uint32_t numChannels, float** output, int32_t numFrames,
                               const KnobLayout& kl);

extern "C" void nam_process_gonk(void* this_, uint32_t /*param2*/, float** input, uint32_t numChannels,
                                  float** output, int32_t numFrames, uint32_t* /*flags*/, void* /*ctx*/)
{
  static const KnobLayout kAnxiety = {0x534, 0x548, 0x520, 0x26b, 0x55c, false};
  nam_process_common(this_, input, numChannels, output, numFrames, kAnxiety);
}

// The additive NAM pedals (see core/spy_patch.h): their engine is a Glorious Drive clone whose three 0..1 knobs
// sit at +0x540 (Level = output), +0x554 (Drive = model) and +0x568 (Tone = input) -- confirmed on hardware:
// the engine creates them in Level, Drive, Tone order, like Anxiety OD's.
extern "C" void nam_process_eng(void* this_, uint32_t /*param2*/, float** input, uint32_t numChannels,
                                 float** output, int32_t numFrames, uint32_t* /*flags*/, void* /*ctx*/)
{
  static const KnobLayout kEng = {0x554, 0x568, 0x540, 0x26b, 0x57c, true};
  nam_process_common(this_, input, numChannels, output, numFrames, kEng);
}

static void nam_process_common(void* this_, float** input, uint32_t numChannels, float** output, int32_t numFrames,
                               const KnobLayout& kl)
{
  flush_denormals_to_zero(); // see its own comment -- Evil's real-time audio
                              // thread, not one we spawned ourselves
  if (numFrames <= 0 || !input || !output || numChannels == 0)
    return;
  if (numChannels > kMaxChannels)
    numChannels = kMaxChannels;

  auto& s = state_for(this_); // per-engine-instance state -- see its own comment
  {
    // Heartbeat to the journal: the first call, then roughly every 16 s of audio. Temporary bring-up diagnostics for
    // the additive NAM pedals (is the hook reached at all, with which knob values, is a model active?).
    const uint32_t n = s.calls.fetch_add(1, std::memory_order_relaxed);
    if (kl.db_trim && n == 0)
      s.is_eng.store(true, std::memory_order_relaxed);
    if (kl.db_trim && (n == 0 || (n & 0x1fff) == 0))
    {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(this_);
      std::fprintf(stderr,
                   "[nam_eng] this=%p call#%u ch=%u frames=%d model=%.3f in=%.3f out=%.3f bypass=%u ready=%d active=%d "
                   "models=%zu hilo=0x%08x\n",
                   this_, n, numChannels, numFrames, *reinterpret_cast<const float*>(b + kl.model),
                   *reinterpret_cast<const float*>(b + kl.input), *reinterpret_cast<const float*>(b + kl.output),
                   b[kl.bypass], s.ready.load() ? 1 : 0, s.active_index.load(), g_cached_models.size(),
                   *reinterpret_cast<const uint32_t*>(b + kl.hilo));
    }
  }
  const int32_t kFadeOutLenSamples = fade_out_len_samples();
  const int32_t kFadeInLenSamples = fade_in_len_samples();

  float drive_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + kl.model); // model select
  if (kl.db_trim && g_models_ready.load(std::memory_order_acquire) && !g_cached_models.empty())
  {
    // Additive pedals: the knob reads as the file number 1..n (nearest step, the same rounding the editor uses for the
    // number it shows and that the name below it uses); the stepping code below only sees the zone's whole-percent position.
    const int n = static_cast<int>(g_cached_models.size());
    const int zone = std::clamp(static_cast<int>(std::lround(std::clamp(drive_raw, 0.0f, 1.0f) * static_cast<float>(n - 1))), 0, n - 1);
    drive_raw = static_cast<float>(zone) / static_cast<float>(kKnobSteps - 1);
  }
  const float tone_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + kl.input);   // input trim
  const float level_raw = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this_) + kl.output); // output trim
  const uint8_t bypass_byte = *(reinterpret_cast<uint8_t*>(this_) + kl.bypass);

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
    switch_model_in_background(s, drive_raw, s.force_recalibrate);
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
      // Start ducking the about-to-be-replaced model out RIGHT NOW --
      // deliberately NOT calling switch_model_in_background here yet. That
      // background construct/calibrate/prewarm work (still real, audible
      // artifacts even at a fast duck when it overlapped a still-live old
      // model, per real-hardware testing) is now only kicked off once this
      // fade-out has fully completed and the old model is truly silent --
      // see the fade_state==1 completion block below, which calls
      // switch_model_in_background itself once fade_progress reaches
      // kFadeOutLenSamples.
      if (s.fade_state.load(std::memory_order_relaxed) == 0)
      {
        s.fade_state.store(1, std::memory_order_relaxed);
        s.fade_progress = 0;
      }
    }

    // Retroactive quality recalibration: the shared per-instance CPU budget
    // only shrinks when a NEW instance joins the board (see
    // g_topology_epoch) -- nothing above notices that an already-playing,
    // already-steady instance's quality tier was chosen for a larger, now
    // stale budget. If this instance is steady (fade_state==0, nothing
    // already in flight -- including whatever the knob-settle check above
    // may have just armed this same call) and has a real model loaded,
    // force it through the exact same duck-out/reload/duck-in cycle a
    // knob-driven switch uses, just re-targeting its OWN current model --
    // switch_model_in_background's epoch-tagged cache check (see
    // CalibrationEntry) then recalculates its tier against the new,
    // smaller budget instead of reusing the stale cached one.
    if (s.fade_state.load(std::memory_order_relaxed) == 0 && s.active_index.load(std::memory_order_acquire) >= 0
        && s.calibrated_epoch.load(std::memory_order_acquire) != g_topology_epoch.load(std::memory_order_acquire))
    {
      s.force_recalibrate = true;
      s.fade_state.store(1, std::memory_order_relaxed);
      s.fade_progress = 0;
    }
  }

  // Safety net against a permanent-mute trap: fade_state==3 is only ever
  // entered alongside a single one-shot switch_model_in_background call (see
  // the fade_state==1 completion block below). If that ONE call fails to
  // ever deliver a result -- debounced by kMinSwitchIntervalMs, lost a CAS
  // race on `switching` to an unrelated overlapping attempt, or the
  // background thread's try/catch swallowed a load exception (corrupt
  // config, bad_alloc, unsupported architecture) -- nothing else in this
  // function ever retries it: the pickup block below only acts once
  // pending_ready is already true, and the knob-settle branch above only
  // arms a new transition from fade_state==0, not 3. Without this, the
  // instance stays muted forever, matching the real-world report of an
  // instance going silent and never recovering even across NAM-file changes
  // or new rigs. Mirrors the unconditional per-call retry already used for
  // the !ready bootstrap case above; switch_model_in_background's own
  // debounce/CAS guards make calling it every block here cheap and safe.
  if (s.fade_state.load(std::memory_order_relaxed) == 3 && !s.pending_ready.load(std::memory_order_acquire))
    switch_model_in_background(s, drive_raw, s.force_recalibrate);

  // Pick up a finished background load, if any. Only the audio thread ever
  // installs into dsp[] -- see ModelState's own comment. fade_state==3 means
  // the old model already faded all the way out and the background thread
  // was kicked off only then (see fade_state==1's completion above);
  // fade_state==0 covers only the first-ever load (nothing was playing to
  // duck, so no fade-out/mute step exists for it).
  const int fs_for_pickup = s.fade_state.load(std::memory_order_relaxed);
  if ((fs_for_pickup == 0 || fs_for_pickup == 3) && s.pending_ready.load(std::memory_order_acquire))
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
      switch_model_in_background(s, drive_raw, s.force_recalibrate);
      // If fs_for_pickup==3 we're already muted -- stay that way until the
      // fresh request above lands here again. If fs_for_pickup==0, nothing
      // audible is playing here either way (see comment above), so no fade
      // is needed just for dropping the stale load.
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
      s.calibrated_epoch.store(g_topology_epoch.load(std::memory_order_acquire), std::memory_order_release);
      s.force_recalibrate = false;
    }
    else // fs_for_pickup == 3, ready == true, not stale -- the only remaining case
    {
      // Old model already faded all the way out before this load was even
      // requested (see fade_state==1's completion above) -- swap in the new
      // model and fade it straight IN. No fade-OUT needed here.
      for (int i = 0; i < kMaxChannels; ++i)
        s.dsp[i] = std::move(s.pending_dsp[i]);
      s.active_index.store(s.pending_index, std::memory_order_release);
      s.pending_ready.store(false, std::memory_order_release);
      s.fade_progress = 0;
      s.fade_state.store(s.pending_index < 0 ? 0 : 2, std::memory_order_relaxed);
      s.calibrated_epoch.store(g_topology_epoch.load(std::memory_order_acquire), std::memory_order_release);
      s.force_recalibrate = false;
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
  const bool fx_curve = kl.db_trim && s.disp_db.load(std::memory_order_relaxed);
  float in_gain = fx_curve ? trim_gain_fx(tone_raw) : kl.db_trim ? trim_gain_db(tone_raw) : trim_gain(tone_raw);
  float out_gain = fx_curve ? trim_gain_fx(level_raw) : kl.db_trim ? trim_gain_db(level_raw) : trim_gain(level_raw);
  // dsp[0] is non-null whenever a real model is active (see silent_active below); the model is only swapped while
  // faded out / muted, so reading its metadata here is safe.
  const bool cal_on = hilo_is_on(this_, kl.hilo);
  if (kl.db_trim && static_cast<int>(cal_on) != s.cal_state.exchange(static_cast<int>(cal_on), std::memory_order_relaxed))
    std::fprintf(stderr, "[nam_eng] %p calibration %s\n", this_, cal_on ? "ON (Gain High)" : "off (Gain Low)");
  if (s.active_index.load(std::memory_order_relaxed) >= 0 && cal_on)
  {
    in_gain *= calibration_gain(s.dsp[0].get());
    out_gain *= calibration_output_gain(s.dsp[0].get());
  }

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
        const float g = 1.0f - raised_cosine_ramp(s.fade_progress + i, kFadeOutLenSamples);
        output[ch][i] *= g;
      }
    }
    s.fade_progress += numFrames;
    if (s.fade_progress >= kFadeOutLenSamples)
    {
      s.fade_progress = 0;
      // Old model is now fully silent -- re-check the knob's CURRENT
      // position (it can wander during the 100ms duck) before doing
      // anything else.
      const int current_idx = knob_value_to_index(drive_raw, static_cast<int>(g_cached_models.size()));
      if (current_idx == s.active_index.load(std::memory_order_acquire) && !s.force_recalibrate)
      {
        // Knob settled back on the model we just faded out -- it's still
        // loaded in dsp[] (never touched during the duck), so just fade it
        // back IN directly. No background work needed at all. (Skipped when
        // force_recalibrate is set -- a topology-triggered recalibration
        // deliberately wants this same model rebuilt at a recalculated
        // quality tier, not just faded back in unchanged.)
        s.fade_state.store(s.active_index.load(std::memory_order_relaxed) < 0 ? 0 : 2,
                            std::memory_order_relaxed);
      }
      else
      {
        // Only NOW -- with the old model fully silent, guaranteed no
        // overlap with anything the audio thread is doing -- kick off the
        // new model's construct/calibrate/prewarm. See fade_state==3 below
        // for the muted wait, and switch_model_in_background's own guard,
        // which allows starting from fade_state==3.
        s.fade_state.store(3, std::memory_order_relaxed);
        switch_model_in_background(s, drive_raw, s.force_recalibrate);
      }
    }
  }
  else if (fade_state == 3) // muted: old model already ducked out, new one still loading
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
      if (input[ch] && output[ch])
        std::fill(output[ch], output[ch] + numFrames, 0.0f);
  }
  else // fade_state == 2: fading IN the newly-installed model
  {
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
      if (!input[ch] || !output[ch])
        continue;
      // Fade the model's INPUT in too, not just its output: the new model
      // was just prewarmed on silence (see switch_model_in_background), so
      // its internal state is cold. Feeding it a smooth ramp-up instead of
      // an instant full-amplitude step avoids shocking a nonlinear model
      // (WaveNet/LSTM) with a discontinuous input it never saw in training.
      // The output ramp is kept too (belt-and-suspenders against any
      // residual near-zero-input bias/noise floor in the model itself).
      for (int32_t i = 0; i < numFrames; ++i)
        output[ch][i] *= raised_cosine_ramp(s.fade_progress + i, kFadeInLenSamples);
      float* in_arr[1] = {output[ch]};
      float* out_arr[1] = {output[ch]};
      s.dsp[ch]->process(in_arr, out_arr, numFrames);
      for (int32_t i = 0; i < numFrames; ++i)
      {
        const float g = raised_cosine_ramp(s.fade_progress + i, kFadeInLenSamples);
        output[ch][i] *= g;
      }
    }
    s.fade_progress += numFrames;
    if (s.fade_progress >= kFadeInLenSamples)
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

// ---- additive NAM pedals: dB faders and file names in the editor ---------------------------------------------------
//
// The pedals' Level / Tone faders should read like the FX-Loop's RETURN / SEND ("%.1f dB", -60..+12), and Drive
// should read the selected .nam file. All three are translator ("desc") objects reached from the Patch node's
// children (see docs/TECHNICAL.md). nam_eng_init is called by the node-registration hook (hook C) every time the
// property nodes get activated, on the UI thread, with the {storage, name, path} triples of the extra pedals, the
// count, the property-tree builder, and (from NAM_ENG_TAB_ADDR) the table the engine stub fills. It only starts a
// watcher thread; the watcher walks the tree with fault-tolerant reads (a wrong guess about the layout just logs and
// gives up -- the pedals keep their percent display and their own dB curve), clones the translator fields and keeps
// the Drive format string in step with the knob.
namespace
{
bool safe_read(uintptr_t addr, void* out, size_t n)
{
  if (addr < 0x10000)
    return false;
  iovec l{out, n}, r{reinterpret_cast<void*>(addr), n};
  return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == static_cast<ssize_t>(n);
}

bool rd_u32(uintptr_t a, uint32_t* v) { return safe_read(a, v, 4); }

bool rd_cstr(uintptr_t a, size_t len, std::string* out)
{
  if (len == 0 || len > 60)
    return false;
  std::string t(len, '\0');
  if (!safe_read(a, &t[0], len))
    return false;
  for (char c : t)
    if (c < 0x20 || c > 0x7e)
      return false;
  *out = std::move(t);
  return true;
}

// QString (Qt 5, 32-bit): d -> QArrayData{ref,size,alloc,offset}; UTF-16 data at d+offset. ASCII only.
bool rd_qstring(uintptr_t qs, std::string* out)
{
  uint32_t d, size, off;
  if (!rd_u32(qs, &d) || !rd_u32(d + 4, &size) || !rd_u32(d + 12, &off) || size == 0 || size > 80)
    return false;
  std::string t;
  for (uint32_t i = 0; i < size; ++i)
  {
    uint16_t c;
    if (!safe_read(d + off + 2 * i, &c, 2) || c < 0x20 || c > 0x7e)
      return false;
    t.push_back(static_cast<char>(c));
  }
  *out = std::move(t);
  return true;
}

constexpr size_t kDescSize = 0x58;

std::string lower(std::string s)
{
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Triple
{
  uint32_t storage, name, path;
};

struct EngTab
{
  uint32_t count;
  struct E
  {
    uint32_t engine, qname;
  } e[8];
};

struct Target
{
  std::string name;                  // "NAM 2"
  std::string path;                  // "NAM 2/"
  uintptr_t level[2] = {0, 0}, tone[2] = {0, 0}, drive[2] = {0, 0}; // desc objects, per mixer
};

struct DisplayState
{
  uintptr_t builder = 0;
  std::vector<Triple> triples;
  uintptr_t eng_tab = 0;
  std::atomic<bool> started{false};
  std::atomic<uintptr_t> latest_builder{0};
};
DisplayState g_disp;

void clone_desc(uintptr_t dst, uintptr_t src)
{
  uint8_t d[kDescSize];
  if (!safe_read(src, d, kDescSize))
    return;
  // defaults, min, max, step and the inline format string; the vtable / bookkeeping stay the destination's own
  std::memcpy(reinterpret_cast<void*>(dst + 0x18), d + 0x18, kDescSize - 0x18);
}

void set_desc_format(uintptr_t desc, const std::string& name)
{
  // Starts with the number like every other fader's text ("%.0f ..."): the knob logic may read the displayed text
  std::string f = "%.0f ";
  for (char c : name)
  {
    if (f.size() >= 28)
      break;
    if (c == '%')
      f += "%%";
    else
      f.push_back(c);
  }
  if (f.size() > 31)
    f.resize(31);
  char buf[32] = {};
  std::memcpy(buf, f.data(), f.size());
  std::memcpy(reinterpret_cast<void*>(desc + 0x38), buf, 32);
}

// File names the Drive knob chooses from: the validated cache once it exists, else (the first ~25 s after boot while
// every model is test-built) a plain listing of the folder in the same order
std::vector<std::string> drive_names()
{
  std::vector<std::string> out;
  if (g_models_ready.load(std::memory_order_acquire))
  {
    for (const auto& m : g_cached_models)
      out.push_back(m.display_name);
    return out;
  }
  static std::vector<std::string> quick;
  static std::chrono::steady_clock::time_point at;
  const auto now = std::chrono::steady_clock::now();
  if (quick.empty() || now - at > std::chrono::seconds(2))
  {
    quick.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(model_dir(), ec))
    {
      if (ec)
        break;
      const std::string fn = entry.path().filename().string();
      if (fn.rfind("._", 0) == 0 || !entry.is_regular_file(ec))
        continue;
      std::string ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      if (ext == ".nam")
        quick.push_back(fn);
    }
    std::sort(quick.begin(), quick.end());
    at = now;
  }
  return quick;
}

// Drive as a plain model number 1..n: the editor's own number formatter needs no per-value text, so nothing can lag
bool set_desc_index(uintptr_t desc, const std::vector<std::string>& names)
{
  const int n = static_cast<int>(names.size());
  const float mn = 1.0f, mx = static_cast<float>(std::max(n, 2)), step = 1.0f;
  float cur[3] = {}, val = 0.f;
  char old_fmt[32] = {};
  if (!safe_read(desc + 0x28, cur, 12) || !safe_read(desc + 0x38, old_fmt, 31) || !safe_read(desc + 0x18, &val, 4))
    return false;
  // the prop keeps its own current (normalized) value at +0x18: the selected file is known without the engine
  std::string f = "%.0f";
  if (n > 0 && val >= 0.0f && val <= 1.0f)
  {
    const int z = std::clamp(static_cast<int>(std::lround(val * static_cast<float>(n - 1))), 0, n - 1);
    std::string name = names[static_cast<size_t>(z)];
    if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".nam")
      name.resize(name.size() - 4);
    if (name.size() > 6 && std::isdigit(static_cast<unsigned char>(name[0])) && name.compare(3, 3, " - ") == 0)
      name.erase(0, 6);
    f += ' ';
    for (char c : name)
    {
      if (f.size() >= 28)
        break;
      if (c == '%')
        f += "%%";
      else
        f.push_back(c);
    }
  }
  if (f.size() > 31)
    f.resize(31);
  if (cur[0] == mn && cur[1] == mx && cur[2] == step && f == old_fmt)
    return false;
  std::memcpy(reinterpret_cast<void*>(desc + 0x28), &mn, 4);
  std::memcpy(reinterpret_cast<void*>(desc + 0x2c), &mx, 4);
  std::memcpy(reinterpret_cast<void*>(desc + 0x30), &step, 4);
  char buf[32] = {};
  std::memcpy(buf, f.data(), f.size());
  std::memcpy(reinterpret_cast<void*>(desc + 0x38), buf, 32);
  std::fprintf(stderr, "[nam_disp] drive desc %#lx -> '%s' (was '%s')\n", static_cast<unsigned long>(desc), f.c_str(), old_fmt);
  return true;
}

// The pedals' props are MixerCtrl property objects: +0x30 std::string{ptr,len} path ("/Engine/MixerCtrl/NAM/Level",
// also .../MixerCtrl2/...), +0x2c shared-ptr control block whose +0x20 is the translator desc. They are found by
// sweeping the heap for the path text and then for the pointers to it (what the core-dump scripts did).
struct HeapRegion
{
  uintptr_t lo, hi;
};

std::vector<HeapRegion> heap_regions(bool wide = false)
{
  std::vector<HeapRegion> out;
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f)
    return out;
  char line[512];
  while (std::fgets(line, sizeof(line), f))
  {
    unsigned long lo, hi;
    char perm[8] = {};
    if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3 || perm[0] != 'r' || perm[1] != 'w')
      continue;
    if (std::strstr(line, "[heap]"))
      out.push_back({lo, hi});
    else if (wide && hi - lo <= (96u << 20) && !std::strchr(line, '/') && !std::strchr(line, '['))
      out.push_back({lo, hi}); // anonymous: malloc arenas of other threads
  }
  std::fclose(f);
  return out;
}

struct PropHit
{
  size_t needle;
  uintptr_t prop;
};

// every MixerCtrl prop whose path is one of `texts`
// `engines`: also reports (in *engine_refs) every word holding one of those engine pointers -- module+0x270
std::vector<PropHit> find_props(const std::vector<std::string>& texts, const std::vector<uint32_t>& engines,
                                std::vector<PropHit>* engine_refs)
{
  const auto regions = heap_regions();
  constexpr size_t kChunk = 1 << 20, kOverlap = 256;
  std::vector<uint8_t> buf(kChunk + kOverlap);
  std::unordered_map<std::string, size_t> by_text;
  for (size_t i = 0; i < texts.size(); ++i)
    by_text[texts[i]] = i;
  std::unordered_map<uint32_t, size_t> text_at; // address of a path's NUL-terminated text -> index
  for (const auto& r : regions)
    for (uintptr_t a = r.lo; a < r.hi; a += kChunk)
    {
      const size_t n = std::min<size_t>(kChunk + kOverlap, r.hi - a);
      if (!safe_read(a, buf.data(), n))
        continue;
      // one search for the common prefix, then an exact lookup of each candidate string
      static const char kPrefix[] = "/Engine/MixerCtrl";
      size_t o = 0;
      while (o + sizeof(kPrefix) <= n)
      {
        const void* hit = memmem(buf.data() + o, n - o, kPrefix, sizeof(kPrefix) - 1);
        if (!hit)
          break;
        o = static_cast<size_t>(static_cast<const uint8_t*>(hit) - buf.data());
        if (o < kChunk)
        {
          const char* c = reinterpret_cast<const char*>(buf.data() + o);
          const size_t maxlen = std::min<size_t>(80, n - o);
          const size_t len = strnlen(c, maxlen);
          if (len < maxlen)
          {
            auto it = by_text.find(std::string(c, len));
            if (it != by_text.end())
              text_at[static_cast<uint32_t>(a + o)] = it->second;
          }
        }
        ++o;
      }
    }
  std::vector<PropHit> out;
  if (text_at.empty() && engines.empty())
    return out;
  uint32_t text_lo = ~0u, text_hi = 0;
  for (const auto& kv : text_at)
  {
    text_lo = std::min(text_lo, kv.first);
    text_hi = std::max(text_hi, kv.first);
  }
  // a prop's path text is allocated right next to the prop: look only around each text found
  constexpr uintptr_t kWin = 0x8000;
  std::vector<uintptr_t> wins;
  for (const auto& kv : text_at)
    wins.push_back(kv.first > kWin ? kv.first - kWin : 0);
  std::sort(wins.begin(), wins.end());
  uintptr_t covered = 0;
  std::vector<uint8_t> wbuf(2 * kWin + 16);
  std::unordered_map<uint32_t, bool> seen_prop;
  for (uintptr_t lo : wins)
  {
    if (lo + 2 * kWin <= covered)
      continue;
    const uintptr_t from = std::max(lo, covered);
    const size_t n = static_cast<size_t>(lo + 2 * kWin - from);
    covered = lo + 2 * kWin;
    if (!safe_read(from, wbuf.data(), n))
      continue;
    for (size_t o = 0; o + 8 <= n; o += 4)
    {
      uint32_t w, len;
      std::memcpy(&w, wbuf.data() + o, 4);
      if (w < text_lo || w > text_hi)
        continue;
      auto it = text_at.find(w);
      if (it == text_at.end())
        continue;
      std::memcpy(&len, wbuf.data() + o + 4, 4);
      if (len == texts[it->second].size() && from + o >= 0x30)
        out.push_back({it->second, from + o - 0x30});
    }
  }
  return out;
}

// the translator desc of a prop, or 0 (the prop's twin object has no control block)
uintptr_t prop_desc(uintptr_t prop)
{
  uint32_t cb, desc;
  if (!rd_u32(prop + 0x2c, &cb) || !cb || !rd_u32(cb + 0x20, &desc) || !desc)
    return 0;
  uint8_t probe[kDescSize];
  if (!safe_read(desc, probe, kDescSize) || probe[0x57] != 0 || !std::memchr(probe + 0x38, 0, 32))
    return 0;
  return desc;
}

// A translator desc really is one: first word a small use count, the four defaults in [0,1], min/max one of the two
// known ranges (percent, or the FX-Loop dB one), and a printable NUL-terminated format string. Writes only happen to
// these: a freshly added pedal's half-built or freed memory also reads as "valid" zeros.
bool desc_sane(uintptr_t desc, uint32_t want_first_word)
{
  uint8_t d[kDescSize];
  if (!safe_read(desc, d, kDescSize))
    return false;
  uint32_t w0;
  std::memcpy(&w0, d, 4);
  (void)want_first_word;
  if (w0 > 4096) // a use count (0 idle, ~6 while the editor holds it)
    return false;
  float f[4], mn, mx;
  std::memcpy(f, d + 0x18, 16);
  std::memcpy(&mn, d + 0x28, 4);
  std::memcpy(&mx, d + 0x2c, 4);
  for (float v : f)
    if (!(v >= 0.0f && v <= 1.0f))
      return false;
  const bool pct = mn == 0.0f && mx == 100.0f, db = mn == -60.0f && mx == 12.0f;
  const bool idx = mn == 1.0f && mx >= 2.0f && mx <= 64.0f; // Drive as a model number
  if (!pct && !db && !idx)
    return false;
  size_t n = 0;
  while (n < 32 && d[0x38 + n])
    ++n;
  if (n == 0 || n >= 32)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (d[0x38 + i] < 0x20 || d[0x38 + i] > 0x7e)
      return false;
  return true;
}

struct Binding
{
  enum Kind { Level, Tone, Drive, DonorReturn, DonorSend } kind;
  std::string pedal; // "NAM 2", empty for donors
  uintptr_t prop, desc;
};

void display_watcher()
{
  lower_background_thread_priority();
  std::string last_text[8];
  std::vector<Binding> bound;
  uint32_t seen_calls[kMaxInstances] = {};
  std::vector<uint32_t> last_engines;
  std::map<uint32_t, std::string> eng_name;
  std::map<uint32_t, int> last_tick;
  bool pending_scan = false;
  int name_retries = 0;
  int since_scan = 1000;
  int last_models = -1;
  std::map<uintptr_t, std::pair<float, size_t>> last_val;
  for (int loops = 0;; ++loops)
  {
    last_val.clear(); // re-verify every desc once per pass (a recreated desc may sit at the same address)
    const auto fast_names = drive_names();
    for (int sub = 0; sub < (loops < 1000 ? 30 : 250); ++sub)
    {
      usleep(2 * 1000);
      // Every knob move leaves the prop pointing at a fresh desc object: follow prop -> current desc each tick (a few
      // 4-byte reads per prop) so the text is patched within ms, not at the next heap sweep
      if (fast_names.empty())
        continue;
      for (const auto& b : bound)
      {
        if (b.kind != Binding::Drive)
          continue;
        const uintptr_t d = prop_desc(b.prop);
        float v;
        if (!d || !safe_read(d + 0x18, &v, 4))
          continue;
        auto& last = last_val[d];
        if (v == last.first && fast_names.size() == last.second)
          continue;
        if (desc_sane(d, 0))
        {
          set_desc_index(d, fast_names);
          last = {v, fast_names.size()};
        }
      }
    }
    ++since_scan;
    // live engines: those whose process() hook ran within the last ~40 passes (a pass-to-pass tick check alone
    // flaps, and every change would re-sweep the heap)
    for (int k = 0; k < kMaxInstances; ++k)
    {
      void* o = g_instances[k].owner_this.load(std::memory_order_acquire);
      const uint32_t c = g_instances[k].calls.load(std::memory_order_relaxed);
      if (o && g_instances[k].is_eng.load(std::memory_order_relaxed))
      {
        const uint32_t e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o));
        if (c != seen_calls[k])
          last_tick[e] = loops;
      }
      seen_calls[k] = c;
    }
    std::vector<uint32_t> engines;
    for (auto it = last_tick.begin(); it != last_tick.end();)
    {
      if (loops - it->second > 80)
        it = last_tick.erase(it);
      else
        engines.push_back((it++)->first);
    }
    const bool engines_changed = engines != last_engines;
    last_engines = engines;
    if (engines.empty() && since_scan < 16)
      continue;

    // props found earlier must still be the same objects (their path pointer and desc chain); else sweep again
    if (engines_changed)
      name_retries = 0;
    const bool missing_name = false;
    bool stale = bound.empty() || engines_changed || pending_scan
                 || since_scan >= 16; // also every ~2 s: pedals added later bring fresh, still-percent translators
    for (const auto& b : bound)
    {
      uint32_t pp;
      if (!rd_u32(b.prop + 0x30, &pp) || prop_desc(b.prop) != b.desc)
        stale = true;
    }
    if (stale)
      pending_scan = true;
    if (pending_scan && since_scan >= (engines_changed || missing_name ? 2 : 16))
    {
      since_scan = 0;
      const auto sweep_t0 = std::chrono::steady_clock::now();
      pending_scan = false;
      if (missing_name)
        ++name_retries;
      std::vector<std::string> texts;
      std::vector<Binding> proto;
      static const char* const kMix[] = {"/Engine/MixerCtrl/", "/Engine/MixerCtrl2/"};
      for (const char* mix : kMix)
      {
        texts.push_back(std::string(mix) + "FX-Loop/Return");
        proto.push_back({Binding::DonorReturn, "", 0, 0});
        texts.push_back(std::string(mix) + "FX-Loop/Send");
        proto.push_back({Binding::DonorSend, "", 0, 0});
        for (const Triple& t : g_disp.triples)
        {
          char nm[64] = {};
          safe_read(t.name, nm, 63);
          for (const auto& pr : {std::pair<const char*, Binding::Kind>{"Level", Binding::Level},
                                 {"Tone", Binding::Tone}, {"Drive", Binding::Drive}})
          {
            texts.push_back(std::string(mix) + nm + "/" + pr.first);
            proto.push_back({pr.second, nm, 0, 0});
          }
        }
      }
      std::vector<PropHit> eng_refs;
      const auto hits = find_props(texts, {}, &eng_refs);
      bound.clear();
      for (const auto& h : hits)
      {
        const uintptr_t d = prop_desc(h.prop);
        if (!d)
          continue;
        Binding b = proto[h.needle];
        b.prop = h.prop;
        b.desc = d;
        bound.push_back(b);
      }
      static size_t last_bound = ~size_t(0);
      if (bound.size() != last_bound)
      {
        last_bound = bound.size();
      std::fprintf(stderr, "[nam_disp] sweep: %zu props with a desc (%zu path hits), %zu engines, %zu engine refs, %lld ms\n",
                   bound.size(), hits.size(), engines.size(), eng_refs.size(),
                   static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - sweep_t0).count()));
      }
    }

    uintptr_t donor_ret = 0, donor_send = 0;
    for (const auto& b : bound)
    {
      char fmt[32] = {};
      safe_read(b.desc + 0x38, fmt, 31);
      if (!std::strstr(fmt, "dB"))
        continue;
      if (b.kind == Binding::DonorReturn)
        donor_ret = b.desc;
      else if (b.kind == Binding::DonorSend)
        donor_send = b.desc;
    }
    if (!donor_ret || !donor_send)
    {
      if (loops % 40 == 1)
        std::fprintf(stderr, "[nam_disp] no FX-Loop donor descs yet (%zu bound)\n", bound.size());
      continue;
    }

    uint32_t want_vt = 0;
    if (!rd_u32(donor_ret, &want_vt) || !desc_sane(donor_ret, want_vt) || !desc_sane(donor_send, want_vt))
      continue;

    // Level <- Return, Tone <- Send (once: skipped when the desc already reads in dB)
    for (const auto& b : bound)
    {
      if (b.kind != Binding::Level && b.kind != Binding::Tone)
        continue;
      char fmt[32] = {};
      safe_read(b.desc + 0x38, fmt, 31);
      if (!std::strstr(fmt, "dB") && prop_desc(b.prop) == b.desc && desc_sane(b.desc, want_vt))
      {
        clone_desc(b.desc, b.kind == Binding::Level ? donor_ret : donor_send);
        std::fprintf(stderr, "[nam_disp] %s %s desc %#lx now dB\n", b.pedal.c_str(),
                     b.kind == Binding::Level ? "Level" : "Tone", static_cast<unsigned long>(b.desc));
      }
    }

    // dB gain curve once every NAM Level / Tone translator reads in dB (instances are not told apart any more:
    // the props are patched wherever they are found, the engines only need the flag)
    bool any_level = false, any_tone = false, all_db = true;
    const auto names_now = drive_names();
    const int n_models = static_cast<int>(names_now.size());
    for (const auto& b : bound)
    {
      if (b.kind == Binding::Drive)
      {
        if (n_models > 0 && prop_desc(b.prop) == b.desc && desc_sane(b.desc, want_vt))
          set_desc_index(b.desc, names_now);
        continue;
      }
      if (b.kind != Binding::Level && b.kind != Binding::Tone)
        continue;
      char fmt[32] = {};
      safe_read(b.desc + 0x38, fmt, 31);
      const bool db = std::strstr(fmt, "dB") != nullptr;
      all_db = all_db && db;
      (b.kind == Binding::Level ? any_level : any_tone) = true;
    }
    const bool use_db = any_level && any_tone && all_db;
    for (int k = 0; k < kMaxInstances; ++k)
      if (g_instances[k].is_eng.load(std::memory_order_relaxed))
        g_instances[k].disp_db.store(use_db, std::memory_order_relaxed);
    if (n_models != last_models)
    {
      last_models = n_models;
      for (int k = 0; k < n_models; ++k)
        std::fprintf(stderr, "[nam_disp] model %d: %s\n", k + 1, names_now[static_cast<size_t>(k)].c_str());
    }
  }
}
} // namespace

extern "C" void nam_eng_init(const uint32_t* triples, uint32_t count, void* builder)
{
  g_disp.latest_builder.store(reinterpret_cast<uintptr_t>(builder));
  bool expected = false;
  if (!g_disp.started.compare_exchange_strong(expected, true))
    return;
  const char* tab = std::getenv("NAM_ENG_TAB_ADDR");
  g_disp.eng_tab = tab ? static_cast<uintptr_t>(std::strtoul(tab, nullptr, 0)) : 0;
  for (uint32_t i = 0; i < count && i < 8; ++i)
    g_disp.triples.push_back(Triple{triples[3 * i], triples[3 * i + 1], triples[3 * i + 2]});
  std::fprintf(stderr, "[nam_disp] init: builder=%p triples=%u eng_tab=%#lx\n", builder, count,
               static_cast<unsigned long>(g_disp.eng_tab));
  spawn_detached([]() { display_watcher(); });
}
