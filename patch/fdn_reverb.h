// fdn_reverb.h -- feedback-delay-network reverbs: "Hall" and "Shimmer". Own implementation from the published
// structure (Jot & Chaigne, "Digital delay networks for designing artificial reverberators", AES 90th, 1991;
// Householder feedback matrix after Stautner & Puckette, 1982; Smith, "Physical Audio Signal Processing").
//
// Mono in, stereo out. Input: DC blocker -> 4 series allpass diffusers -> 8 delay lines (mutually prime lengths,
// 22..74 ms, slowly modulated) coupled by an 8x8 Householder matrix (orthogonal, so lossless on its own), each line
// with a gain set from the requested RT60 and a one-pole lowpass (damping, passive, unity at DC). Stable by
// construction: orthogonal matrix, every loop element has |gain| < 1.
//
// Shimmer adds an octave-up pitch shifter (two Hann-windowed read taps sweeping a delay window) on the network
// output, fed back into its input through a soft limiter, so each recirculation climbs an octave.
//
// Real-time safe like dattorro_reverb.h: fixed buffers, no allocation, no locks. Zero-initialised statics only need
// init() before use. Controls (all 0..1): mix (equal-power), decay (RT60), damping (0 dark .. 1 bright).
#ifndef NAM_FDN_REVERB_H
#define NAM_FDN_REVERB_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "dattorro_reverb.h"

namespace fdn
{
using dattorro::Allpass;
using dattorro::Line;

constexpr int kLines = 8;
constexpr double kRefRate = 48000.0;

struct Hall
{
  Allpass<512> d1, d2, d3, d4;
  Line<8192> line[kLines];
  uint32_t len[kLines];  // nominal length of each line in samples at the real rate
  float lp[kLines];      // damping lowpass state
  float gain[kLines];    // per-line decay gain for the current RT60
  float lfo_ph[kLines], lfo_inc[kLines];
  float mod_depth;
  float dcx, dcy;
  float scale;
  float mix_s, decay_s, damp_s, comp_s;
  float shim_l, shim_r; // shimmer: pitch shifted signal injected into the network input (0 for the plain Hall)
  bool ready;

  // delay lengths at 48 kHz: primes, so the line resonances do not stack
  static constexpr uint32_t kBase[kLines] = {1087, 1381, 1663, 1949, 2297, 2713, 3089, 3541};

  void init(double sample_rate)
  {
    scale = static_cast<float>(sample_rate / kRefRate);
    clear();
    d1.len = scaled(113);
    d2.len = scaled(167);
    d3.len = scaled(241);
    d4.len = scaled(331);
    for (int i = 0; i < kLines; ++i)
    {
      len[i] = scaled(kBase[i]);
      // each line gets its own slow LFO (0.07 .. 0.37 Hz) so the modulation does not beat as one
      lfo_inc[i] = static_cast<float>(2.0 * M_PI * (0.07 + 0.043 * i) / sample_rate);
      lfo_ph[i] = static_cast<float>(i) * 0.7853982f;
    }
    mod_depth = 6.0f * scale;
    ready = true;
  }

  void clear()
  {
    d1.line.clear();
    d2.line.clear();
    d3.line.clear();
    d4.line.clear();
    for (int i = 0; i < kLines; ++i)
    {
      line[i].clear();
      lp[i] = 0.0f;
      gain[i] = 0.0f;
    }
    dcx = dcy = 0.0f;
    shim_l = shim_r = 0.0f;
    mix_s = decay_s = damp_s = comp_s = -1.0f;
  }

  // Wet level compensation so the decay and damping knobs do not also act as volume knobs: linear gain on the wet
  // signal over a 5x5 grid of (decay, damping) knob positions 0, .25, .5, .75, 1, bilinear in between. Measured with
  // `reverb_lab --table` (stationary noise, target -1 dB re input); regenerate after changing the network.
  static constexpr float kComp[5][5] = {
      {19.3485f, 17.1343f, 13.1860f, 9.8590f, 7.2180f},
      {10.0592f, 8.8780f, 6.7772f, 5.0047f, 3.5504f},
      {6.7734f, 5.9477f, 4.4881f, 3.2584f, 2.2239f},
      {5.0833f, 4.4381f, 3.3086f, 2.3616f, 1.5592f},
      {3.9903f, 3.4623f, 2.5512f, 1.7946f, 1.1542f}};
  static float comp_of(float decay_knob, float damp_knob)
  {
    const float a = std::min(std::max(decay_knob, 0.0f), 1.0f) * 4.0f, b = std::min(std::max(damp_knob, 0.0f), 1.0f) * 4.0f;
    const int i = std::min(static_cast<int>(a), 3), j = std::min(static_cast<int>(b), 3);
    const float fa = a - static_cast<float>(i), fb = b - static_cast<float>(j);
    // interpolate in the log domain: the grid spans tens of dB
    const auto lg = [](float x) { return std::log(x); };
    const float lo = lg(kComp[i][j]) * (1.0f - fb) + lg(kComp[i][j + 1]) * fb;
    const float hi = lg(kComp[i + 1][j]) * (1.0f - fb) + lg(kComp[i + 1][j + 1]) * fb;
    return std::exp(lo * (1.0f - fa) + hi * fa);
  }

  // RT60 in seconds for decay knob 0..1 (0.3 s .. 14 s, exponential so the knob feels even)
  static float rt60_of(float knob) { return 0.3f * std::pow(14.0f / 0.3f, knob); }

  // Runs the network one sample. `x` is the mono input (already diffused by the caller or not), outputs wet stereo.
  // `inject` is added to every line input (used by the shimmer).
  void tick(float x, float rt60, float damp_a, float inject, float* out_l, float* out_r)
  {
    const float hp = x - dcx + 0.995f * dcy;
    dcx = x;
    dcy = hp;
    float v = hp;
    v = d1.process(v, 0.7f);
    v = d2.process(v, 0.7f);
    v = d3.process(v, 0.7f);
    v = d4.process(v, 0.7f);
    v += inject;

    float t[kLines];
    float sum = 0.0f;
    for (int i = 0; i < kLines; ++i)
    {
      lfo_ph[i] += lfo_inc[i];
      if (lfo_ph[i] > 6.2831853f)
        lfo_ph[i] -= 6.2831853f;
      const float d = static_cast<float>(len[i]) + std::sin(lfo_ph[i]) * mod_depth;
      float y = line[i].read_frac(d);
      lp[i] += damp_a * (y - lp[i]);
      y = lp[i] * gain[i];
      t[i] = y;
      sum += y;
    }
    // Householder: out_i = t_i - (2/N) * sum(t)
    const float k = sum * (2.0f / kLines);
    float ol = 0.0f, orr = 0.0f;
    for (int i = 0; i < kLines; ++i)
    {
      const float fb = t[i] - k;
      // alternating signs decorrelate how the single input excites the lines
      line[i].write(fb + ((i & 1) ? -v : v) * 0.35f);
      // stereo tap: even lines left, odd lines right, signs spread so L and R differ
      const float o = t[i];
      if (i & 1)
        orr += (i & 2) ? -o : o;
      else
        ol += (i & 2) ? -o : o;
    }
    *out_l = ol * 0.5f;
    *out_r = orr * 0.5f;
  }

  void set_decay(float rt60)
  {
    // gain per pass for each line: -60 dB after rt60 seconds => g = 10^(-3 d / (rt60 fs))
    const float fs = scale * 48000.0f;
    for (int i = 0; i < kLines; ++i)
      gain[i] = std::pow(10.0f, -3.0f * static_cast<float>(len[i]) / (rt60 * fs));
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    const float rt_t = rt60_of(decay_knob);
    const float damp_t = 0.2f + 0.8f * damp_knob * damp_knob;
    const float comp_t = comp_of(decay_knob, damp_knob);
    if (mix_s < 0.0f)
    {
      mix_s = mix;
      decay_s = rt_t;
      damp_s = damp_t;
      comp_s = comp_t;
    }
    const float k = 1.0f - std::exp(-1.0f / (0.02f * 48000.0f * (scale)));
    int upd = 0;
    for (int32_t i = 0; i < frames; ++i)
    {
      mix_s += k * (mix - mix_s);
      decay_s += k * (rt_t - decay_s);
      damp_s += k * (damp_t - damp_s);
      comp_s += k * (comp_t - comp_s);
      if ((upd++ & 31) == 0)
        set_decay(decay_s); // pow() per line is too costly to do per sample; 32 samples is far below knob speed
      const float dl = in[0][i];
      const float dr = channels > 1 && in[1] ? in[1][i] : dl;
      float wl, wr;
      tick(0.5f * (dl + dr) + 1e-20f, decay_s, damp_s, 0.0f, &wl, &wr);
      wl *= comp_s;
      wr *= comp_s;
      const float a = mix_s * 1.5707963f;
      const float dg = std::cos(a), wg = std::sin(a);
      out[0][i] = dl * dg + (channels > 1 ? wl : 0.5f * (wl + wr)) * wg;
      if (channels > 1 && out[1])
        out[1][i] = dr * dg + wr * wg;
    }
  }

private:
  uint32_t scaled(uint32_t base) const
  {
    return std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(static_cast<double>(base) * scale)));
  }
};

// Octave-up pitch shifter: a delay line read by two taps whose delay sweeps from kWin down to 0 at one sample per
// sample (ratio 2), each tap windowed by a Hann curve of its own phase so the wrap click is silent; the taps are
// half a window apart so the windows sum to 1.
struct OctaveUp
{
  static constexpr uint32_t kWin = 2048; // about 43 ms at 48 kHz
  Line<4096> line;
  float phase; // 0..1

  void clear()
  {
    line.clear();
    phase = 0.0f;
  }
  float process(float x)
  {
    line.write(x);
    phase += 1.0f / static_cast<float>(kWin); // delay falls by 1 per sample => ratio 2
    if (phase >= 1.0f)
      phase -= 1.0f;
    float y = 0.0f;
    for (int k = 0; k < 2; ++k)
    {
      float p = phase + 0.5f * static_cast<float>(k);
      if (p >= 1.0f)
        p -= 1.0f;
      const float delay = (1.0f - p) * static_cast<float>(kWin) + 1.0f; // sweeps kWin .. 1
      const float w = 0.5f - 0.5f * std::cos(6.2831853f * p);
      y += line.read_frac(delay) * w;
    }
    return y;
  }
};

struct Shimmer
{
  Hall hall;
  OctaveUp up;
  float fb;     // last shifted sample fed back
  float shim_g; // feedback amount at short decay
  static constexpr float kShimRt0 = 2.0f; // RT60 (s) at which the feedback has halved

  void init(double sample_rate)
  {
    hall.init(sample_rate);
    clear();
    shim_g = 3.0f;
  }
  void clear()
  {
    hall.clear();
    up.clear();
    fb = 0.0f;
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    Hall& h = hall;
    const float rt_t = Hall::rt60_of(decay_knob);
    const float damp_t = 0.2f + 0.8f * damp_knob * damp_knob;
    const float comp_t = Hall::comp_of(decay_knob, damp_knob);
    if (h.mix_s < 0.0f)
    {
      h.mix_s = mix;
      h.decay_s = rt_t;
      h.damp_s = damp_t;
      h.comp_s = comp_t;
    }
    const float k = 1.0f - std::exp(-1.0f / (0.02f * 48000.0f * h.scale));
    int upd = 0;
    for (int32_t i = 0; i < frames; ++i)
    {
      h.mix_s += k * (mix - h.mix_s);
      h.decay_s += k * (rt_t - h.decay_s);
      h.damp_s += k * (damp_t - h.damp_s);
      h.comp_s += k * (comp_t - h.comp_s);
      if ((upd++ & 31) == 0)
        h.set_decay(h.decay_s);
      const float dl = in[0][i];
      const float dr = channels > 1 && in[1] ? in[1][i] : dl;
      float wl, wr;
      h.tick(0.5f * (dl + dr) + 1e-20f, h.decay_s, h.damp_s, fb, &wl, &wr);
      // shifted copy of the network output goes back in, through a soft limiter so the loop can never run away
      const float s = up.process(0.5f * (wl + wr));
      const float lim = s / (1.0f + std::fabs(s) * 2.0f);
      fb = lim * shim_g / (1.0f + h.decay_s * (1.0f / kShimRt0)); // less feedback the longer the tail: the loop gain must stay < 1
      wl *= h.comp_s;
      wr *= h.comp_s;
      const float a = h.mix_s * 1.5707963f;
      const float dg = std::cos(a), wg = std::sin(a);
      out[0][i] = dl * dg + (channels > 1 ? wl : 0.5f * (wl + wr)) * wg;
      if (channels > 1 && out[1])
        out[1][i] = dr * dg + wr * wg;
    }
  }
};
} // namespace fdn

#endif
