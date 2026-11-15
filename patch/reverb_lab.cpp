// reverb_lab.cpp -- host-side lab for the reverb algorithms (dattorro_reverb.h, fdn_reverb.h): renders impulse
// responses and a plucked-string phrase to WAV and prints the numbers that tell whether an algorithm behaves.
//
//   c++ -std=c++17 -O2 -o reverb_lab patch/reverb_lab.cpp && ./reverb_lab [outdir]
//
// Checks per algorithm: finite output, RT60 from Schroeder backward integration (T20 extrapolated) at several
// decay settings, tail stability after a loud burst, stereo decorrelation (IACC at lag 0), peak level and speed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "dattorro_reverb.h"
#include "fdn_reverb.h"
#include "room_spring_reverb.h"

namespace
{
constexpr double kFs = 48000.0;

struct Algo
{
  const char* name;
  std::function<void()> init;
  std::function<void(float* const*, float* const*, uint32_t, int32_t, float, float, float)> process;
};

dattorro::Reverb* g_plate = new dattorro::Reverb();
fdn::Hall* g_hall = new fdn::Hall();
fdn::Shimmer* g_shim = new fdn::Shimmer();
room::Room* g_room = new room::Room();
spring::Spring* g_spring = new spring::Spring();
spring::SpringFull* g_springf = new spring::SpringFull();

std::vector<Algo> algos()
{
  return {
      {"plate", [] { std::memset(g_plate, 0, sizeof *g_plate); g_plate->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_plate->process(i, o, c, n, m, d, t);
       }},
      {"hall", [] { std::memset(g_hall, 0, sizeof *g_hall); g_hall->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_hall->process(i, o, c, n, m, d, t);
       }},
      {"shimmer", [] { std::memset(g_shim, 0, sizeof *g_shim); g_shim->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_shim->process(i, o, c, n, m, d, t);
       }},
      {"room", [] { std::memset(g_room, 0, sizeof *g_room); g_room->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_room->process(i, o, c, n, m, d, t);
       }},
      {"spring", [] { std::memset(g_spring, 0, sizeof *g_spring); g_spring->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_spring->process(i, o, c, n, m, d, t);
       }},
      {"springfull", [] { std::memset(g_springf, 0, sizeof *g_springf); g_springf->init(kFs); },
       [](float* const* i, float* const* o, uint32_t c, int32_t n, float m, float d, float t) {
         g_springf->process(i, o, c, n, m, d, t);
       }},
  };
}


// spring "drip": when does the first echo of a low band and of a high band peak (band-passed impulse response)
double band_peak_ms(const std::vector<float>& x, double f0)
{
  const double w0 = 2.0 * M_PI * f0 / kFs, al = std::sin(w0) / (2.0 * 4.0), a0 = 1.0 + al;
  const double b0 = al / a0, b2 = -al / a0, a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - al) / a0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0, env = 0, best = 0;
  size_t at = 0;
  const double k = 1.0 - std::exp(-1.0 / (0.004 * kFs));
  for (size_t i = 0; i < static_cast<size_t>(kFs * 0.4); ++i)
  {
    const double in = x[i], y = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = in; y2 = y1; y1 = y;
    env += k * (std::fabs(y) - env);
    if (i > static_cast<size_t>(kFs * 0.02) && env > best) { best = env; at = i; }
  }
  return static_cast<double>(at) / kFs * 1000.0;
}

// run in blocks of 64 like the engine
void run(const Algo& a, const std::vector<float>& in, std::vector<float>& l, std::vector<float>& r, float mix,
         float decay, float damp)
{
  l.assign(in.size(), 0.0f);
  r.assign(in.size(), 0.0f);
  std::vector<float> il(64), ir(64), ol(64), orr(64);
  for (size_t p = 0; p < in.size(); p += 64)
  {
    const int n = static_cast<int>(std::min<size_t>(64, in.size() - p));
    for (int i = 0; i < n; ++i)
      il[i] = ir[i] = in[p + i];
    float* ip[2] = {il.data(), ir.data()};
    float* op[2] = {ol.data(), orr.data()};
    a.process(ip, op, 2, n, mix, decay, damp);
    for (int i = 0; i < n; ++i)
    {
      l[p + i] = ol[i];
      r[p + i] = orr[i];
    }
  }
}

void write_wav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f)
    return;
  const uint32_t n = static_cast<uint32_t>(l.size());
  const uint32_t data = n * 4, riff = 36 + data, rate = static_cast<uint32_t>(kFs), br = rate * 4;
  const uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
  const uint32_t fmt = 16;
  std::fwrite("RIFF", 1, 4, f);
  std::fwrite(&riff, 4, 1, f);
  std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f);
  std::fwrite(&pcm, 2, 1, f);
  std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f);
  std::fwrite(&br, 4, 1, f);
  std::fwrite(&ba, 2, 1, f);
  std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f);
  std::fwrite(&data, 4, 1, f);
  for (uint32_t i = 0; i < n; ++i)
  {
    const int16_t s[2] = {static_cast<int16_t>(std::clamp(l[i], -1.0f, 1.0f) * 32767.0f),
                          static_cast<int16_t>(std::clamp(r[i], -1.0f, 1.0f) * 32767.0f)};
    std::fwrite(s, 2, 2, f);
  }
  std::fclose(f);
}

bool finite(const std::vector<float>& v)
{
  for (float x : v)
    if (!std::isfinite(x))
      return false;
  return true;
}

// RT60 from the Schroeder integral: time between -5 and -25 dB, times 3. Returns -1 if the IR never gets there.
double rt60_of(const std::vector<float>& ir)
{
  std::vector<double> edc(ir.size());
  double acc = 0;
  for (size_t i = ir.size(); i-- > 0;)
  {
    acc += static_cast<double>(ir[i]) * ir[i];
    edc[i] = acc;
  }
  if (acc <= 0)
    return -1;
  double t5 = -1, t25 = -1;
  for (size_t i = 0; i < edc.size(); ++i)
  {
    const double db = 10.0 * std::log10(edc[i] / acc + 1e-30);
    if (t5 < 0 && db <= -5.0)
      t5 = static_cast<double>(i) / kFs;
    if (t25 < 0 && db <= -25.0)
    {
      t25 = static_cast<double>(i) / kFs;
      break;
    }
  }
  return (t5 < 0 || t25 < 0) ? -1 : (t25 - t5) * 3.0;
}

double iacc(const std::vector<float>& l, const std::vector<float>& r)
{
  double ll = 0, rr = 0, lr = 0;
  for (size_t i = 0; i < l.size(); ++i)
  {
    ll += static_cast<double>(l[i]) * l[i];
    rr += static_cast<double>(r[i]) * r[i];
    lr += static_cast<double>(l[i]) * r[i];
  }
  return lr / std::sqrt(ll * rr + 1e-30);
}

double rms_db(const std::vector<float>& v, size_t a, size_t b)
{
  double s = 0;
  for (size_t i = a; i < b && i < v.size(); ++i)
    s += static_cast<double>(v[i]) * v[i];
  return 10.0 * std::log10(s / std::max<size_t>(1, b - a) + 1e-30);
}

// power of a sine at `f` Hz in v[a..b) (Goertzel), dB
double tone_db(const std::vector<float>& v, size_t a, size_t b, double f)
{
  const double w = 2.0 * M_PI * f / kFs, c = 2.0 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (size_t i = a; i < b; ++i)
  {
    const double s0 = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return 10.0 * std::log10(p / static_cast<double>((b - a) * (b - a)) + 1e-30);
}

// plucked string (Karplus-Strong), a stand-in for a guitar note: f0 Hz, `secs` long
void pluck(std::vector<float>& out, size_t start, double f0, double amp, std::mt19937& rng)
{
  const size_t n = static_cast<size_t>(kFs / f0);
  std::vector<float> d(n);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (auto& x : d)
    x = u(rng);
  size_t p = 0;
  for (size_t i = start; i < out.size() && i < start + static_cast<size_t>(kFs * 1.6); ++i)
  {
    const float y = d[p];
    d[p] = 0.4985f * (d[p] + d[(p + 1) % n]);
    p = (p + 1) % n;
    out[i] += static_cast<float>(amp) * y;
  }
}

// --table: measure the stationary-noise wet level of the Hall over the (decay, damping) grid and print the
// corrected kComp block for fdn_reverb.h (paste it in, run again until the printed corrections are ~1.00)
int table(const std::string& which)
{
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> in(static_cast<size_t>(kFs * 40)), l, r;
  for (auto& x : in)
    x = 0.5f * u(rng);
  const double inp = rms_db(in, 0, in.size());
  const auto all = algos();
  const Algo a = which == "room" ? all[3] : which == "spring" ? all[4] : all[1];
  const auto comp = [&](int i, int j) {
    return static_cast<double>(which == "room" ? room::Room::kComp[i][j] : which == "spring" ? spring::Spring::kComp[i][j] : fdn::Hall::kComp[i][j]);
  };
  std::printf("  static constexpr float kComp[5][5] = {\n");
  for (int i = 0; i < 5; ++i)
  {
    std::printf("      {");
    for (int j = 0; j < 5; ++j)
    {
      a.init();
      run(a, in, l, r, 1.0f, 0.25f * static_cast<float>(i), 0.25f * static_cast<float>(j));
      const double lvl = 0.5 * (rms_db(l, in.size() / 2, in.size()) + rms_db(r, in.size() / 2, in.size())) - inp;
      const double fix = std::pow(10.0, (-1.0 - lvl) / 20.0);
      std::printf("%.4ff%s", comp(i, j) * fix, j < 4 ? ", " : "");
    }
    std::printf("}%s\n", i < 4 ? "," : "};");
  }
  return 0;
}
} // namespace

int main(int argc, char** argv)
{
  if (argc > 1 && std::string(argv[1]) == "--table")
    return table(argc > 2 ? argv[2] : "hall");
  const std::string dir = argc > 1 ? argv[1] : ".";
  int fails = 0;
  for (const Algo& a : algos())
  {
    std::printf("== %s\n", a.name);
    // impulse responses at three decay settings, full wet
    for (float decay : {0.2f, 0.5f, 0.8f})
    {
      std::vector<float> in(static_cast<size_t>(kFs * 30), 0.0f), l, r;
      in[0] = 1.0f;
      a.init();
      run(a, in, l, r, 1.0f, decay, 0.6f);
      const bool ok = finite(l) && finite(r);
      double pk = 0;
      for (float x : l)
        pk = std::max(pk, static_cast<double>(std::fabs(x)));
      std::printf("  decay %.1f: rt60 %.2f s  peak %.3f  iacc %.2f  tail(25..30s) %.0f dB  %s\n", decay,
                  rt60_of(l), pk, iacc(l, r), rms_db(l, static_cast<size_t>(kFs * 25), in.size()),
                  ok ? "finite" : "NOT FINITE");
      if (!ok)
        ++fails;
      if (std::string(a.name).rfind("spring", 0) == 0 && decay == 0.5f)
        std::printf("    drip: first echo peaks at %.1f ms (300 Hz) / %.1f ms (1 kHz) / %.1f ms (3.5 kHz)\n",
                    band_peak_ms(l, 300), band_peak_ms(l, 1000), band_peak_ms(l, 3500));
      write_wav(dir + "/ir_" + a.name + "_d" + std::to_string(static_cast<int>(decay * 10)) + ".wav",
                std::vector<float>(l.begin(), l.begin() + static_cast<long>(kFs * 6)),
                std::vector<float>(r.begin(), r.begin() + static_cast<long>(kFs * 6)));
    }
    // stability: loud white noise burst at max decay and full brightness, then silence; must decay
    {
      std::mt19937 rng(7);
      std::uniform_real_distribution<float> u(-1.0f, 1.0f);
      std::vector<float> in(static_cast<size_t>(kFs * 40), 0.0f), l, r;
      for (size_t i = 0; i < static_cast<size_t>(kFs * 0.5); ++i)
        in[i] = 0.9f * u(rng);
      a.init();
      run(a, in, l, r, 1.0f, 1.0f, 1.0f);
      const double hot = rms_db(l, 0, static_cast<size_t>(kFs * 0.5));
      const double end = rms_db(l, static_cast<size_t>(kFs * 35), in.size());
      std::printf("  burst at max decay: during %.0f dB, 35..40 s %.0f dB  %s\n", hot, end,
                  finite(l) && end < hot - 40 ? "stable" : "UNSTABLE");
      if (!(finite(l) && end < hot - 40))
        ++fails;
    }
    // wet level for stationary noise vs decay setting (dB re input): should be flat so the decay knob does not
    // also act as a volume knob
    {
      std::mt19937 rng(11);
      std::uniform_real_distribution<float> u(-1.0f, 1.0f);
      std::vector<float> in(static_cast<size_t>(kFs * 30)), l, r;
      for (auto& x : in)
        x = 0.5f * u(rng);
      const double inp = rms_db(in, 0, in.size());
      std::printf("  noise level re input:");
      for (float decay : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
      {
        a.init();
        run(a, in, l, r, 1.0f, decay, 0.6f);
        std::printf("  d%.2f %+.1f", decay, rms_db(l, in.size() / 2, in.size()) - inp);
      }
      std::printf(" dB\n");
    }
    // pitch: a 330 Hz tone for 3 s then silence; tail power at f, 2f, 4f
    {
      std::vector<float> in(static_cast<size_t>(kFs * 8), 0.0f), l, r;
      for (size_t i = 0; i < static_cast<size_t>(kFs * 3); ++i)
        in[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 330.0 * static_cast<double>(i) / kFs));
      a.init();
      run(a, in, l, r, 1.0f, 0.7f, 0.9f);
      const size_t t0 = static_cast<size_t>(kFs * 3.5), t1 = static_cast<size_t>(kFs * 5.5);
      std::printf("  tail tones: 330 Hz %.0f dB, 660 Hz %.0f dB, 1320 Hz %.0f dB\n", tone_db(l, t0, t1, 330),
                  tone_db(l, t0, t1, 660), tone_db(l, t0, t1, 1320));
    }
    // phrase: three plucked notes, 50 % mix, mid settings; written for listening
    {
      std::mt19937 rng(3);
      std::vector<float> in(static_cast<size_t>(kFs * 8), 0.0f), l, r;
      pluck(in, 0, 164.8, 0.5, rng);
      pluck(in, static_cast<size_t>(kFs * 1.5), 220.0, 0.5, rng);
      pluck(in, static_cast<size_t>(kFs * 3.0), 329.6, 0.5, rng);
      a.init();
      run(a, in, l, r, 0.5f, 0.55f, 0.6f);
      write_wav(dir + "/phrase_" + a.name + ".wav", l, r);
      a.init();
      run(a, in, l, r, 1.0f, 0.55f, 0.6f); // wet only, for listening to the reverb itself
      write_wav(dir + "/wet_" + a.name + ".wav", l, r);
    }
    // speed (host, indicative only)
    {
      std::vector<float> in(static_cast<size_t>(kFs * 10), 0.1f), l, r;
      a.init();
      const auto t0 = std::chrono::steady_clock::now();
      run(a, in, l, r, 0.5f, 0.5f, 0.5f);
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("  speed: %.1f ns/sample (host, %.0fx real time)\n", s / static_cast<double>(in.size()) * 1e9,
                  10.0 / s);
    }
  }
  std::printf("%s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
