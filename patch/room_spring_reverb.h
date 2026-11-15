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

constexpr int kStages = 32;
constexpr int kSprings = 2;

struct Spring
{
  Line<4096> d[kSprings];
  float x1[kSprings][kStages], y1[kSprings][kStages];
  uint32_t len[kSprings];
  float lp[kSprings], gain[kSprings], fb[kSprings];
  float in_lp, dcx, dcy;
  float scale;
  float mix_s, decay_s, damp_s, comp_s;
  bool ready;

  static constexpr float kDisp = 0.70f;                    // allpass coefficient: low frequencies are delayed more
  static constexpr uint32_t kBase[kSprings] = {1613, 1297}; // delays (48 kHz) besides the dispersion
  // total loop delay in samples at 48 kHz: delay + the dispersion chain's DC delay ((1+a)/(1-a) per section) + 1
  static constexpr float kChain = kStages * (1.0f + kDisp) / (1.0f - kDisp);

  static constexpr float kComp[5][5] = {{3.0661f, 3.0165f, 2.8900f, 2.7289f, 2.5556f},
                                        {2.9961f, 2.9172f, 2.7221f, 2.4852f, 2.2415f},
                                        {2.9163f, 2.8050f, 2.5399f, 2.2358f, 1.9388f},
                                        {2.8294f, 2.6834f, 2.3522f, 1.9965f, 1.6668f},
                                        {2.7353f, 2.5534f, 2.1652f, 1.7755f, 1.4311f}};

  void init(double sample_rate)
  {
    scale = static_cast<float>(sample_rate / 48000.0);
    clear();
    for (int s = 0; s < kSprings; ++s)
      len[s] = std::min<uint32_t>(4090, std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(kBase[s] * scale))));
    ready = true;
  }

  void clear()
  {
    for (int s = 0; s < kSprings; ++s)
    {
      d[s].clear();
      for (int i = 0; i < kStages; ++i)
        x1[s][i] = y1[s][i] = 0.0f;
      lp[s] = fb[s] = 0.0f;
      gain[s] = 0.0f;
    }
    in_lp = dcx = dcy = 0.0f;
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

  // RT60 in seconds for decay knob 0..1 (0.6 s .. 5 s, exponential)
  static float rt60_of(float knob) { return 0.6f * std::pow(5.0f / 0.6f, knob); }

  void tick(float x, float damp_a, float* out_l, float* out_r)
  {
    const float hp = x - dcx + 0.995f * dcy;
    dcx = x;
    dcy = hp;
    in_lp += 0.45f * (hp - in_lp); // springs are dull
    float o[kSprings];
    for (int s = 0; s < kSprings; ++s)
    {
      float v = in_lp * 0.5f + fb[s];
      for (int i = 0; i < kStages; ++i)
      {
        const float y = kDisp * v + x1[s][i] - kDisp * y1[s][i];
        x1[s][i] = v;
        y1[s][i] = y;
        v = y;
      }
      d[s].write(v);
      const float y = d[s].read(len[s]);
      lp[s] += damp_a * (y - lp[s]);
      fb[s] = lp[s] * gain[s];
      o[s] = y;
    }
    *out_l = o[0] + 0.3f * o[1];
    *out_r = o[1] + 0.3f * o[0];
  }

  void set_decay(float rt60)
  {
    const float fs = scale * 48000.0f;
    for (int s = 0; s < kSprings; ++s)
    {
      const float loop = static_cast<float>(len[s]) + kChain * scale + 1.0f;
      gain[s] = std::pow(10.0f, -3.0f * loop / (rt60 * fs));
    }
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    const float rt_t = rt60_of(decay_knob);
    const float damp_t = 0.08f + 0.6f * damp_knob * damp_knob;
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
};
} // namespace spring

#endif
