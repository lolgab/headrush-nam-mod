// dattorro_reverb.h -- plate reverb after Jon Dattorro, "Effect Design, Part 1: Reverberator and Other Filters"
// (J. Audio Eng. Soc., 45(9), 1997), figure 1. Own implementation from the published topology (no third-party code).
//
// Mono in, stereo out. Input: bandwidth lowpass -> 4 series allpass diffusers -> the "tank", two cross-coupled
// loops (modulated allpass -> delay -> damping lowpass -> decay gain -> allpass -> delay -> decay gain). The stereo
// output is a sum of 7 signed taps per side from inside the tank. All delay lengths are Dattorro's, specified at
// 29761 Hz and rescaled to the real sample rate.
//
// Real-time safe: no allocation after init (fixed buffers, sized for up to ~98 kHz), no locks, no exceptions.
// Instances are plain structs of floats, so a zero-initialised static one only needs init() before use.
//
// Controls (all 0..1): mix (equal-power dry/wet), decay (tank feedback), damping (tank lowpass: 0 dark, 1 bright).
#ifndef NAM_DATTORRO_REVERB_H
#define NAM_DATTORRO_REVERB_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dattorro
{
constexpr double kRefRate = 29761.0; // sample rate the delay lengths below are specified at

// Power-of-two circular buffer. age 0 = the sample written last.
template <size_t N>
struct Line
{
  static_assert((N & (N - 1)) == 0, "power of two");
  float buf[N];
  uint32_t wp;
  static constexpr uint32_t kMask = N - 1;

  void clear()
  {
    for (auto& v : buf)
      v = 0.0f;
    wp = 0;
  }
  float tap(uint32_t age) const { return buf[(wp - 1 - age) & kMask]; }
  // value written `delay` writes ago, read before the next write: a plain delay of `delay` samples
  float read(uint32_t delay) const { return buf[(wp - delay) & kMask]; }
  void write(float x)
  {
    buf[wp] = x;
    wp = (wp + 1) & kMask;
  }
  // linear-interpolated read at a fractional delay (>= 1)
  float read_frac(float delay) const
  {
    const uint32_t i = static_cast<uint32_t>(delay);
    const float f = delay - static_cast<float>(i);
    return buf[(wp - i) & kMask] * (1.0f - f) + buf[(wp - i - 1) & kMask] * f;
  }
};

// Schroeder allpass: v = x - g*d, y = d + g*v, v is what gets stored (and tapped).
template <size_t N>
struct Allpass
{
  Line<N> line;
  uint32_t len;
  float process(float x, float g)
  {
    const float d = line.read(len);
    const float v = x - g * d;
    line.write(v);
    return d + g * v;
  }
  float process_mod(float x, float g, float delay)
  {
    const float d = line.read_frac(delay);
    const float v = x - g * d;
    line.write(v);
    return d + g * v;
  }
};

template <size_t N>
struct Delay
{
  Line<N> line;
  uint32_t len;
  float process(float x)
  {
    const float y = line.read(len);
    line.write(x);
    return y;
  }
};

struct Reverb
{
  // capacities: Dattorro's longest line is 4453 samples at 29761 Hz; x3.3 covers rates up to ~98 kHz
  Allpass<512> in1, in2;
  Allpass<2048> in3;
  Allpass<1024> in4;
  Allpass<4096> l_mod, r_mod;
  Delay<16384> l_d1, r_d1, l_d2, r_d2;
  Allpass<8192> l_ap, r_ap;
  float bw_state, dcx, dcy;
  float damp_l, damp_r, fb_l, fb_r;
  float lfo_phase, lfo_inc, mod_depth;
  float scale;
  uint32_t tap_l[7], tap_r[7];
  float mix_s, decay_s, damp_s; // smoothed controls
  bool ready;

  void init(double sample_rate)
  {
    scale = static_cast<float>(sample_rate / kRefRate);
    clear();
    in1.len = len(142);
    in2.len = len(107);
    in3.len = len(379);
    in4.len = len(277);
    l_mod.len = len(672);
    l_d1.len = len(4453);
    l_ap.len = len(1800);
    l_d2.len = len(3720);
    r_mod.len = len(908);
    r_d1.len = len(4217);
    r_ap.len = len(2656);
    r_d2.len = len(3163);
    // left: r_d1 [266, 2974], - r_ap [1913], + r_d2 [1996], - l_d1 [1990], - l_ap [187], - l_d2 [1066]
    const uint32_t tl[7] = {266, 2974, 1913, 1996, 1990, 187, 1066};
    // right: l_d1 [353, 3627], - l_ap [1228], + l_d2 [2673], - r_d1 [2111], - r_ap [335], - r_d2 [121]
    const uint32_t tr[7] = {353, 3627, 1228, 2673, 2111, 335, 121};
    for (int i = 0; i < 7; ++i)
    {
      tap_l[i] = len(tl[i]);
      tap_r[i] = len(tr[i]);
    }
    mod_depth = 8.0f * scale; // Dattorro: +-8 samples at 29761 Hz
    lfo_inc = static_cast<float>(2.0 * M_PI * 1.0 / sample_rate); // ~1 Hz
    ready = true;
  }

  void clear()
  {
    in1.line.clear();
    in2.line.clear();
    in3.line.clear();
    in4.line.clear();
    l_mod.line.clear();
    r_mod.line.clear();
    l_d1.line.clear();
    r_d1.line.clear();
    l_d2.line.clear();
    r_d2.line.clear();
    l_ap.line.clear();
    r_ap.line.clear();
    bw_state = dcx = dcy = 0.0f;
    damp_l = damp_r = fb_l = fb_r = 0.0f;
    lfo_phase = 0.0f;
    mix_s = decay_s = damp_s = -1.0f; // snap to the first control values
  }

  // wet-only stereo output for one input sample
  void tick(float x, float decay, float damp_a, float* out_l, float* out_r)
  {
    // input DC blocker + bandwidth lowpass (Dattorro: 0.9995)
    const float hp = x - dcx + 0.995f * dcy;
    dcx = x;
    dcy = hp;
    bw_state += 0.9995f * (hp - bw_state);
    float v = bw_state;
    v = in1.process(v, 0.75f);
    v = in2.process(v, 0.75f);
    v = in3.process(v, 0.625f);
    v = in4.process(v, 0.625f);

    const float diff2 = std::min(std::max(decay + 0.15f, 0.25f), 0.5f);
    lfo_phase += lfo_inc;
    if (lfo_phase > 6.2831853f)
      lfo_phase -= 6.2831853f;
    const float m_l = std::sin(lfo_phase) * mod_depth;
    const float m_r = std::cos(lfo_phase) * mod_depth;

    float l = v + fb_r;
    l = l_mod.process_mod(l, -0.70f, static_cast<float>(l_mod.len) + m_l);
    l = l_d1.process(l);
    damp_l += damp_a * (l - damp_l);
    l = l_ap.process(damp_l * decay, diff2);
    l = l_d2.process(l);
    const float next_fb_l = l * decay;

    float r = v + fb_l;
    r = r_mod.process_mod(r, -0.70f, static_cast<float>(r_mod.len) + m_r);
    r = r_d1.process(r);
    damp_r += damp_a * (r - damp_r);
    r = r_ap.process(damp_r * decay, diff2);
    r = r_d2.process(r);
    const float next_fb_r = r * decay;
    fb_l = next_fb_l;
    fb_r = next_fb_r;

    *out_l = r_d1.line.tap(tap_l[0]) + r_d1.line.tap(tap_l[1]) - r_ap.line.tap(tap_l[2]) + r_d2.line.tap(tap_l[3])
             - l_d1.line.tap(tap_l[4]) - l_ap.line.tap(tap_l[5]) - l_d2.line.tap(tap_l[6]);
    *out_r = l_d1.line.tap(tap_r[0]) + l_d1.line.tap(tap_r[1]) - l_ap.line.tap(tap_r[2]) + l_d2.line.tap(tap_r[3])
             - r_d1.line.tap(tap_r[4]) - r_ap.line.tap(tap_r[5]) - r_d2.line.tap(tap_r[6]);
  }

  // Processes a block. in/out are per-channel arrays (channels 1 or 2; in-place is fine). Knobs are 0..1.
  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay_knob,
               float damp_knob)
  {
    const float decay_t = 0.25f + 0.68f * decay_knob * decay_knob; // 0.25 .. 0.93 (>= 1 would never decay)
    const float damp_t = 0.05f + 0.95f * damp_knob * damp_knob;    // tank lowpass coefficient, 1 = bright
    if (mix_s < 0.0f)
    {
      mix_s = mix;
      decay_s = decay_t;
      damp_s = damp_t;
    }
    // one-pole smoothing per sample (about 20 ms) so knob moves do not zipper
    const float k = 1.0f - std::exp(-1.0f / (0.02f * 48000.0f * (scale / 1.6128f)));
    for (int32_t i = 0; i < frames; ++i)
    {
      mix_s += k * (mix - mix_s);
      decay_s += k * (decay_t - decay_s);
      damp_s += k * (damp_t - damp_s);
      const float dry_in_l = in[0][i];
      const float dry_in_r = channels > 1 && in[1] ? in[1][i] : dry_in_l;
      float wl, wr;
      tick(0.5f * (dry_in_l + dry_in_r) + 1e-20f, decay_s, damp_s, &wl, &wr);
      constexpr float kWetGain = 0.6f;
      wl *= kWetGain;
      wr *= kWetGain;
      const float a = mix_s * 1.5707963f;
      const float dry_g = std::cos(a), wet_g = std::sin(a);
      out[0][i] = dry_in_l * dry_g + (channels > 1 ? wl : 0.5f * (wl + wr)) * wet_g;
      if (channels > 1 && out[1])
        out[1][i] = dry_in_r * dry_g + wr * wet_g;
    }
  }

private:
  uint32_t len(uint32_t base) const
  {
    return std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(static_cast<double>(base) * scale)));
  }
};
} // namespace dattorro

#endif
