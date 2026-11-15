// room_spring_reverb.h -- two more reverbs for the Reverb pedal: "Room" and "Spring". Own implementations from the
// published structures; real-time safe like dattorro_reverb.h (fixed buffers, no allocation, no locks), zero-initialised
// statics only need init() before use. Mono in, stereo out. Controls (all 0..1): mix (equal-power), decay, damping.
//
// Room: a small feedback delay network (Jot & Chaigne 1991, as fdn_reverb.h's Hall) with very short, mutually prime
// delays (4..14 ms), two input diffusers and eight early-reflection taps spread over 7..67 ms. RT60 0.15 .. 1.8 s.
// Stable by construction (orthogonal Householder matrix, every loop element below unity gain).
//
// Spring: two spring "tanks" (left / right). Each is a feedback loop of a dispersion chain (32 first-order allpass
// sections, which delay low frequencies more than high ones: the chirp of a real spring), a delay and a damping
// lowpass, fed from a dull (about 4.5 kHz) input. RT60 0.6 .. 5 s. Allpass sections have unit gain, so the loop gain
// (set from the RT60 over the full loop delay, dispersion included) alone decides stability.
#ifndef NAM_ROOM_SPRING_REVERB_H
#define NAM_ROOM_SPRING_REVERB_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "dattorro_reverb.h"

namespace room
{
using dattorro::Allpass;
using dattorro::Line;

constexpr int kLines = 8;
constexpr int kTaps = 8;

struct Room
{
  Allpass<256> d1, d2;
  Line<2048> line[kLines];
  Line<4096> er;
  uint32_t len[kLines];
  uint32_t er_tap[kTaps];
  float lp[kLines], gain[kLines];
  float lfo_ph[kLines], lfo_inc[kLines], mod_depth;
  float dcx, dcy, scale;
  float mix_s, decay_s, damp_s, comp_s;
  bool ready;

  static constexpr uint32_t kBase[kLines] = {197, 251, 311, 373, 433, 509, 587, 661};
  static constexpr uint32_t kErMs[kTaps] = {7, 11, 17, 23, 31, 41, 53, 67};
  static constexpr float kErGain[kTaps] = {0.80f, -0.70f, 0.60f, -0.55f, 0.45f, -0.40f, 0.30f, -0.25f};

  // Wet level compensation over (decay, damping) knob positions 0, .25, .5, .75, 1 (see reverb_lab --table room)
  static constexpr float kComp[5][5] = {{1.6373f, 1.6312f, 1.6114f, 1.5767f, 1.5189f},
                                        {1.6164f, 1.6062f, 1.5728f, 1.5151f, 1.4188f},
                                        {1.5900f, 1.5746f, 1.5252f, 1.4415f, 1.3043f},
                                        {1.5576f, 1.5362f, 1.4695f, 1.3586f, 1.1837f},
                                        {1.5188f, 1.4907f, 1.4067f, 1.2685f, 1.0629f}};

  void init(double sample_rate)
  {
    scale = static_cast<float>(sample_rate / 48000.0);
    clear();
    d1.len = scaled(37, 255);
    d2.len = scaled(61, 255);
    for (int i = 0; i < kLines; ++i)
    {
      len[i] = scaled(kBase[i], 2040);
      lfo_inc[i] = static_cast<float>(2.0 * M_PI * (0.11 + 0.057 * i) / sample_rate);
      lfo_ph[i] = static_cast<float>(i) * 0.7853982f;
    }
    for (int i = 0; i < kTaps; ++i)
      er_tap[i] = scaled(kErMs[i] * 48u, 4090);
    mod_depth = 0.8f * scale; // a hint of movement only: a room should not chorus
    ready = true;
  }

  void clear()
  {
    d1.line.clear();
    d2.line.clear();
    er.clear();
    for (int i = 0; i < kLines; ++i)
    {
      line[i].clear();
      lp[i] = 0.0f;
      gain[i] = 0.0f;
    }
    dcx = dcy = 0.0f;
    mix_s = decay_s = damp_s = comp_s = -1.0f;
  }

  static float comp_of(float decay_knob, float damp_knob)
  {
    const float a = std::min(std::max(decay_knob, 0.0f), 1.0f) * 4.0f, b = std::min(std::max(damp_knob, 0.0f), 1.0f) * 4.0f;
    const int i = std::min(static_cast<int>(a), 3), j = std::min(static_cast<int>(b), 3);
    const float fa = a - static_cast<float>(i), fb = b - static_cast<float>(j);
    const auto lg = [](float x) { return std::log(x); };
    const float lo = lg(kComp[i][j]) * (1.0f - fb) + lg(kComp[i][j + 1]) * fb;
    const float hi = lg(kComp[i + 1][j]) * (1.0f - fb) + lg(kComp[i + 1][j + 1]) * fb;
    return std::exp(lo * (1.0f - fa) + hi * fa);
  }

  // RT60 in seconds for decay knob 0..1 (0.15 s .. 1.8 s, exponential)
  static float rt60_of(float knob) { return 0.15f * std::pow(12.0f, knob); }

  void tick(float x, float damp_a, float* out_l, float* out_r)
  {
    const float hp = x - dcx + 0.995f * dcy;
    dcx = x;
    dcy = hp;
    er.write(hp);
    float el = 0.0f, eR = 0.0f;
    for (int i = 0; i < kTaps; ++i)
    {
      const float t = er.tap(er_tap[i]) * kErGain[i];
      if (i & 1)
        eR += t;
      else
        el += t;
    }
    float v = d1.process(hp, 0.6f);
    v = d2.process(v, 0.6f);

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
    const float k = sum * (2.0f / kLines);
    float ol = 0.0f, orr = 0.0f;
    for (int i = 0; i < kLines; ++i)
    {
      line[i].write(t[i] - k + ((i & 1) ? -v : v) * 0.35f);
      if (i & 1)
        orr += (i & 2) ? -t[i] : t[i];
      else
        ol += (i & 2) ? -t[i] : t[i];
    }
    *out_l = ol * 0.5f + el * 0.5f;
    *out_r = orr * 0.5f + eR * 0.5f;
  }

  void set_decay(float rt60)
  {
    const float fs = scale * 48000.0f;
    for (int i = 0; i < kLines; ++i)
      gain[i] = std::pow(10.0f, -3.0f * static_cast<float>(len[i]) / (rt60 * fs));
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    const float rt_t = rt60_of(decay_knob);
    const float damp_t = 0.25f + 0.75f * damp_knob * damp_knob;
    const float comp_t = comp_of(decay_knob, damp_knob);
    if (mix_s < 0.0f)
    {
      mix_s = mix;
      decay_s = rt_t;
      damp_s = damp_t;
      comp_s = comp_t;
    }
    const float k = 1.0f - std::exp(-1.0f / (0.02f * 48000.0f * scale));
    int upd = 0;
    for (int32_t i = 0; i < frames; ++i)
    {
      mix_s += k * (mix - mix_s);
      decay_s += k * (rt_t - decay_s);
      damp_s += k * (damp_t - damp_s);
      comp_s += k * (comp_t - comp_s);
      if ((upd++ & 31) == 0)
        set_decay(decay_s);
      const float dl = in[0][i];
      const float dr = channels > 1 && in[1] ? in[1][i] : dl;
      float wl, wr;
      tick(0.5f * (dl + dr) + 1e-20f, damp_s, &wl, &wr);
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
  uint32_t scaled(uint32_t base, uint32_t cap) const
  {
    return std::min<uint32_t>(cap, std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(static_cast<double>(base) * scale))));
  }
};
} // namespace room

namespace spring
{
using dattorro::Line;

// Spring: the parametric model of Valimaki, Parker and Abel, "Parametric spring reverberation effect" (JAES 58(7/8), 2010;
// see also Parker, "Efficient dispersion generation structures for spring reverb emulation", EURASIP JASP 2011).
// Written from the papers' structure; the starting values (two springs of 56 and 69 ms, ~4.4 kHz cutoff, chirp
// coefficient up to ~0.45, cascade of 100 stretched allpass sections, a 24-section "splash" path, panned 80/20) follow the
// description of the MIT-licensed nam-grid project's Spring Reverb, no code from it.
//
// Each spring is a feedback loop: a cascade of kStages first-order allpass sections whose delay is stretched from 1 to K
// samples, A(z) = (a + z^-K) / (1 + a z^-K) (K = fs / 8800 puts the dispersion up to the ~4.4 kHz cutoff; low frequencies
// are delayed less than high ones: each echo sweeps from lows to highs, the "drip"), then a delay and a lowpass at the cutoff. A short plain-allpass
// cascade high-passed at the cutoff adds the "splash" of the high band. Allpass sections have unit gain, so only the loop
// gain (set from the RT60 over the whole loop delay, taking the cascade's group delay at 1 kHz) decides stability.
constexpr int kSplash = 24; // plain sections of the splash path
constexpr int kSprings = 2;
constexpr int kMaxK = 8; // stretch factor capacity (K = fs / 8800: 5 at 48 kHz, 3 at 24 kHz)

// R = rate divisor (the loops carry nothing above the ~4.4 kHz cutoff, so they can run at fs / R: the cost per input
// sample falls by R), N = stretched sections per spring (the chirp's length in time is proportional to N).
template <int R, int N>
struct SpringT
{
  Line<8192> d[kSprings];
  float xh[kSprings][N][kMaxK], yh[kSprings][N][kMaxK];
  float sx[kSplash], sy[kSplash]; // splash path states (one-sample allpass sections)
  uint32_t len[kSprings];         // the delay in front of the lowpass, the cascade's DC delay already taken off
  uint32_t k_pos;                 // position inside the K-sample histories (shared: all sections advance together)
  float lp1[kSprings], lp2[kSprings], gain[kSprings], fb[kSprings];
  float b0, b1, b2, a1, a2; // loop lowpass (RBJ biquad) coefficients, shared by both springs
  float splash_fb;
  float dcx, dcy;
  float acc; // low-rate input accumulator (boxcar lowpass) and its phase
  int phase;
  float prev_l, prev_r, cur_l, cur_r; // low-rate outputs, linearly interpolated back up
  float scale, drip, fs_lo;
  int K;
  float mix_s, decay_s, damp_s, comp_s;
  bool ready;

  static constexpr uint32_t kEchoMs[kSprings] = {56, 69};
  // the table below was measured on the full-rate, 100-section model; fewer sections / half rate are quieter
  static constexpr float kRateTrim = (R == 1 && N == 100) ? 1.0f : 1.43f;

  // Wet level compensation over (decay, damping) knob positions 0, .25, .5, .75, 1 (see reverb_lab --table spring)
  static constexpr float kComp[5][5] = {{2.8813f, 2.8601f, 2.8394f, 2.8194f, 2.8005f},
      {2.8205f, 2.7808f, 2.7421f, 2.7057f, 2.6720f},
      {2.7346f, 2.6714f, 2.6110f, 2.5551f, 2.5052f},
      {2.6209f, 2.5318f, 2.4491f, 2.3725f, 2.3072f},
      {2.4776f, 2.3634f, 2.2615f, 2.1656f, 2.0867f}};

  void init(double sample_rate)
  {
    scale = static_cast<float>(sample_rate / 48000.0);
    fs_lo = static_cast<float>(sample_rate / R);
    K = std::min(kMaxK, std::max(1, static_cast<int>(std::lround(fs_lo / 8800.0))));
    drip = 0.42f; // chirp coefficient: the audible chirp grows up to ~0.45, above that the dispersion passes the cutoff
    clear();
    ready = true;
    set_cutoff(4000.0f);
    set_decay(1.5f);
  }

  void clear()
  {
    std::memset(xh, 0, sizeof xh);
    std::memset(yh, 0, sizeof yh);
    for (int i = 0; i < kSplash; ++i)
      sx[i] = sy[i] = 0.0f;
    for (int s = 0; s < kSprings; ++s)
    {
      d[s].clear();
      lp1[s] = lp2[s] = fb[s] = gain[s] = 0.0f;
    }
    k_pos = 0;
    splash_fb = dcx = dcy = acc = 0.0f;
    phase = 0;
    prev_l = prev_r = cur_l = cur_r = 0.0f;
    mix_s = decay_s = damp_s = comp_s = -1.0f;
  }

  static float comp_of(float decay_knob, float damp_knob)
  {
    const float a = std::min(std::max(decay_knob, 0.0f), 1.0f) * 4.0f, b = std::min(std::max(damp_knob, 0.0f), 1.0f) * 4.0f;
    const int i = std::min(static_cast<int>(a), 3), j = std::min(static_cast<int>(b), 3);
    const float fa = a - static_cast<float>(i), fb = b - static_cast<float>(j);
    const auto lg = [](float x) { return std::log(x); };
    const float lo = lg(kComp[i][j]) * (1.0f - fb) + lg(kComp[i][j + 1]) * fb;
    const float hi = lg(kComp[i + 1][j]) * (1.0f - fb) + lg(kComp[i + 1][j + 1]) * fb;
    return std::exp(lo * (1.0f - fa) + hi * fa);
  }

  // RT60 in seconds for decay knob 0..1 (0.5 s .. 4 s, exponential)
  static float rt60_of(float knob) { return 0.5f * std::pow(8.0f, knob); }

  // group delay (samples at fs_lo) of one stretched section A(z) = (a + z^-K) / (1 + a z^-K) at angular frequency w
  float section_delay(float w) const
  {
    const float a = drip;
    return static_cast<float>(K) * (1.0f - a * a) / (1.0f + 2.0f * a * std::cos(static_cast<float>(K) * w) + a * a);
  }

  // loop lowpass: RBJ biquad, Q 0.707, cutoff fc Hz (cookbook formulas)
  void set_cutoff(float fc)
  {
    const float w0 = 2.0f * static_cast<float>(M_PI) * std::min(fc, 0.45f * fs_lo) / fs_lo;
    const float cw = std::cos(w0), alpha = std::sin(w0) / (2.0f * 0.7071f);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f - cw) * 0.5f / a0;
    b1 = (1.0f - cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void set_decay(float rt60)
  {
    const float w1k = 2.0f * static_cast<float>(M_PI) * 1000.0f / fs_lo;
    const float chain_1k = N * section_delay(w1k);
    // the delay in front of the lowpass is the echo time minus what the cascade already adds at the low band (300 Hz)
    const float chain_dc = N * section_delay(2.0f * static_cast<float>(M_PI) * 300.0f / fs_lo);
    for (int s = 0; s < kSprings; ++s)
    {
      const float echo = static_cast<float>(kEchoMs[s]) * 0.001f * fs_lo;
      len[s] = std::min<uint32_t>(8000, static_cast<uint32_t>(std::max(2.0f, echo - chain_dc)));
      const float loop = static_cast<float>(len[s]) + chain_1k + 2.0f; // what a 1 kHz component sees per round trip
      gain[s] = std::pow(10.0f, -3.0f * loop / (rt60 * fs_lo));
    }
  }

  // one low-rate step: input x (already lowpassed), outputs the wet stereo pair
  void step(float x, float* out_l, float* out_r)
  {
    const uint32_t kp = k_pos;
    float o[kSprings];
    for (int s = 0; s < kSprings; ++s)
    {
      float v = x * 0.35f + fb[s];
      for (int i = 0; i < N; ++i)
      {
        const float xo = xh[s][i][kp], yo = yh[s][i][kp]; // x[n-K], y[n-K]
        const float y = drip * v + xo - drip * yo;
        xh[s][i][kp] = v;
        yh[s][i][kp] = y;
        v = y;
      }
      d[s].write(v);
      const float y = d[s].read(len[s]);
      const float lpo = b0 * y + lp1[s];
      lp1[s] = b1 * y - a1 * lpo + lp2[s];
      lp2[s] = b2 * y - a2 * lpo;
      fb[s] = lpo * gain[s];
      o[s] = y;
    }
    k_pos = (kp + 1 >= static_cast<uint32_t>(K)) ? 0u : kp + 1;
    // splash: a short plain-allpass cascade, a little feedback, the high band's diffuse part
    float sp = x * 0.3f + splash_fb * 0.55f;
    for (int i = 0; i < kSplash; ++i)
    {
      const float a = 0.6f;
      const float y = a * sp + sx[i] - a * sy[i];
      sx[i] = sp;
      sy[i] = y;
      sp = y;
    }
    splash_fb = sp;
    const float splash = sp * 0.25f;
    *out_l = 0.8f * o[0] + 0.2f * o[1] + splash;
    *out_r = 0.2f * o[0] + 0.8f * o[1] - splash;
  }

  // full-rate sample in, full-rate wet pair out
  void tick(float x, float* out_l, float* out_r)
  {
    const float hp = x - dcx + 0.995f * dcy;
    dcx = x;
    dcy = hp;
    acc += hp;
    if (++phase >= R)
    {
      prev_l = cur_l;
      prev_r = cur_r;
      step(acc * (1.0f / R), &cur_l, &cur_r);
      acc = 0.0f;
      phase = 0;
    }
    // linear interpolation between the last two low-rate outputs (the content is below the ~4.4 kHz cutoff)
    const float f = static_cast<float>(phase) * (1.0f / R);
    *out_l = prev_l + (cur_l - prev_l) * f;
    *out_r = prev_r + (cur_r - prev_r) * f;
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    const float rt_t = rt60_of(decay_knob);
    const float damp_t = 2500.0f + 3500.0f * damp_knob; // loop lowpass cutoff Hz
    const float comp_t = comp_of(decay_knob, damp_knob) * kRateTrim;
    if (mix_s < 0.0f)
    {
      mix_s = mix;
      decay_s = rt_t;
      damp_s = damp_t;
      comp_s = comp_t;
      set_cutoff(damp_s);
      set_decay(decay_s);
    }
    const float k = 1.0f - std::exp(-1.0f / (0.02f * 48000.0f * scale));
    int upd = 0;
    for (int32_t i = 0; i < frames; ++i)
    {
      mix_s += k * (mix - mix_s);
      decay_s += k * (rt_t - decay_s);
      damp_s += k * (damp_t - damp_s);
      comp_s += k * (comp_t - comp_s);
      if ((upd++ & 31) == 0)
      {
        set_decay(decay_s);
        set_cutoff(damp_s);
      }
      const float dl = in[0][i];
      const float dr = channels > 1 && in[1] ? in[1][i] : dl;
      float wl, wr;
      tick(0.5f * (dl + dr) + 1e-20f, &wl, &wr);
      wl *= comp_s;
      wr *= comp_s;
      const float a = mix_s * 1.5707963f;
      const float dg = std::cos(a), wg = std::sin(a);
      out[0][i] = dl * dg + (channels > 1 ? wl : 0.5f * (wl + wr)) * wg;
      if (channels > 1 && out[1])
        out[1][i] = dr * dg + wr * wg;
    }
  }
};

using Spring = SpringT<2, 60>;      // what the pedal runs: half rate, 60 sections per spring
using SpringFull = SpringT<1, 100>; // the full model, for comparison in the lab
} // namespace spring

#endif
