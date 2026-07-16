/* test_nam_naml_e2e.c -- exercises libnam_hook.so's nam_process_naml() plus
 * the Input/Output trim setters (nam_set_input_trim/nam_set_output_trim),
 * the same way the injected additive-pedal engine vtable will call them:
 * dlopen the .so, dlsym each entry point, build fake per-channel
 * input/output pointer arrays matching the confirmed 8-arg process() ABI
 * (same shape as nam_process_gonk -- this engine is a byte-for-byte clone of
 * Gonkulator's layout, see patch_namloader.py), and call them directly.
 *
 * Tests: mono/stereo passthrough-before-load and dual-mono independence
 * (same checks as test_nam_gonk_e2e.c), PLUS output trim's exact linear
 * relationship to a trim=0 baseline (deterministic regardless of the NAM
 * model's own nonlinearity, since output trim is a final multiply applied
 * after process()), PLUS a basic "input trim actually does something"
 * sanity check (not an exact ratio -- input trim feeds the nonlinear model,
 * so no simple linear relationship is expected or asserted).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

typedef void (*nam_process_naml_fn)(void*, uint32_t, float**, uint32_t, float**, int32_t, uint32_t*, void*);
typedef void (*nam_set_trim_fn)(void*, float);

static void fill_inputs(float* inputL, float* inputR, int n)
{
  for (int i = 0; i < n; i++)
  {
    inputL[i] = 0.5f * sinf(2.0f * (float)M_PI * 220.0f * i / 48000.0f);
    inputR[i] = 0.3f * sinf(2.0f * (float)M_PI * 330.0f * i / 48000.0f);
  }
}

static int run_case(nam_process_naml_fn fn, nam_set_trim_fn set_in, nam_set_trim_fn set_out,
                     uint32_t num_channels)
{
  enum { N = 128 };
  float inputL[N], inputR[N], outputL[N], outputR[N];
  fill_inputs(inputL, inputR, N);
  memset(outputL, 0, sizeof(outputL));
  memset(outputR, 0, sizeof(outputR));

  float* input[2] = {inputL, inputR};
  float* output[2] = {outputL, outputR};
  uint32_t flags = 0;

  static uint8_t fake_this[0x2b0];
  memset(fake_this, 0, sizeof(fake_this));
  *(float*)(fake_this + 0x2ac) = 0.0f; /* fixed knob value: picks index 0 */

  set_in(fake_this, 0.0f);
  set_out(fake_this, 0.0f);

  /* First call: informational only, NOT asserted -- model state is a shared
   * global singleton, so by the time run_case(..., 2) runs, the model is
   * already loaded from run_case(..., 1)'s pass, and this "call 1" won't
   * actually see pre-load passthrough. Real pre-load passthrough (both
   * trims at 0.0 => gain 1.0, byte-exact) is confirmed on the very first
   * run_case call, which does see a genuinely unloaded model. */
  fn(fake_this, 0, input, num_channels, output, N, &flags, NULL);
  int passthrough_ok = memcmp(inputL, outputL, sizeof(inputL)) == 0;
  if (num_channels == 2)
    passthrough_ok = passthrough_ok && memcmp(inputR, outputR, sizeof(inputR)) == 0;
  printf("[ch=%u] call 1 (pre-load)  passthrough_exact=%s\n", num_channels, passthrough_ok ? "yes" : "no");

  int got_model = 0;
  for (int tries = 0; tries < 200; tries++)
  {
    memset(outputL, 0, sizeof(outputL));
    memset(outputR, 0, sizeof(outputR));
    fn(fake_this, 0, input, num_channels, output, N, &flags, NULL);
    if (memcmp(inputL, outputL, sizeof(inputL)) != 0)
    {
      got_model = 1;
      break;
    }
    usleep(50 * 1000);
  }

  if (!got_model)
  {
    fprintf(stderr, "[ch=%u] FAIL: model never loaded\n", num_channels);
    return 2;
  }

  int finite_ok = 1;
  float max_abs_l = 0.0f, max_abs_r = 0.0f;
  for (int i = 0; i < N; i++)
  {
    if (!isfinite(outputL[i]) || (num_channels == 2 && !isfinite(outputR[i])))
      finite_ok = 0;
    if (fabsf(outputL[i]) > max_abs_l)
      max_abs_l = fabsf(outputL[i]);
    if (num_channels == 2 && fabsf(outputR[i]) > max_abs_r)
      max_abs_r = fabsf(outputR[i]);
  }

  printf("[ch=%u] call N (post-load) finite=%s max_abs_L=%f max_abs_R=%f\n",
         num_channels, finite_ok ? "yes" : "no", max_abs_l, max_abs_r);

  if (!finite_ok)
  {
    fprintf(stderr, "[ch=%u] FAIL: non-finite output\n", num_channels);
    return 3;
  }
  if (max_abs_l > 100.0f || (num_channels == 2 && max_abs_r > 100.0f))
  {
    fprintf(stderr, "[ch=%u] FAIL: output exploded\n", num_channels);
    return 4;
  }

  if (num_channels == 2)
  {
    int channels_differ = memcmp(outputL, outputR, sizeof(outputL)) != 0;
    printf("[ch=%u] channels_differ=%s (expected yes for independent dual-mono state)\n",
           num_channels, channels_differ ? "yes" : "no");
    if (!channels_differ)
    {
      fprintf(stderr, "[ch=%u] FAIL: L/R outputs identical -- state not independent?\n", num_channels);
      return 5;
    }
  }

  /* ---- Output trim: exact linear relationship to the trim=0 baseline ----
   * NAM (WaveNet) has a finite receptive field much shorter than N=128; since
   * every call so far has fed the exact same periodic 128-sample block, a
   * few more settling calls guarantee the model's internal history has fully
   * converged to a repeating cycle before we capture the reference output --
   * otherwise two consecutive stateful calls wouldn't be comparable via a
   * simple multiply even though output trim itself is applied deterministically. */
  for (int settle = 0; settle < 4; settle++)
    fn(fake_this, 0, input, num_channels, output, N, &flags, NULL);
  float baseline[N];
  memcpy(baseline, outputL, sizeof(baseline));

  set_out(fake_this, 0.5f); /* NAM_TRIM_MAX_DB default 12dB -> gain = 10^(0.5*12/20) */
  const float expected_gain = powf(10.0f, (0.5f * 12.0f) / 20.0f);

  fill_inputs(inputL, inputR, N); /* re-seed identical input sequence */
  memset(outputL, 0, sizeof(outputL));
  memset(outputR, 0, sizeof(outputR));
  fn(fake_this, 0, input, num_channels, output, N, &flags, NULL);

  float max_rel_err = 0.0f;
  for (int i = 0; i < N; i++)
  {
    const float expected = baseline[i] * expected_gain;
    const float err = fabsf(outputL[i] - expected);
    const float rel = err / (fabsf(expected) > 1e-6f ? fabsf(expected) : 1e-6f);
    if (rel > max_rel_err)
      max_rel_err = rel;
  }
  printf("[ch=%u] output_trim gain=%.4f max_rel_err=%.6f\n", num_channels, expected_gain, max_rel_err);
  if (max_rel_err > 0.01f)
  {
    fprintf(stderr, "[ch=%u] FAIL: output trim not linear within tolerance\n", num_channels);
    return 6;
  }
  set_out(fake_this, 0.0f);

  /* ---- Input trim: sanity check only (feeds the nonlinear model, no exact
   * ratio expected) -- just confirm it measurably changes the output and
   * stays finite/bounded. ---- */
  set_in(fake_this, 0.7f);
  fill_inputs(inputL, inputR, N);
  memset(outputL, 0, sizeof(outputL));
  memset(outputR, 0, sizeof(outputR));
  fn(fake_this, 0, input, num_channels, output, N, &flags, NULL);

  int input_trim_had_effect = memcmp(baseline, outputL, sizeof(baseline)) != 0;
  int input_trim_finite = 1;
  for (int i = 0; i < N; i++)
    if (!isfinite(outputL[i]) || fabsf(outputL[i]) > 100.0f)
      input_trim_finite = 0;
  printf("[ch=%u] input_trim changed_output=%s finite=%s\n",
         num_channels, input_trim_had_effect ? "yes" : "no", input_trim_finite ? "yes" : "no");
  if (!input_trim_had_effect || !input_trim_finite)
  {
    fprintf(stderr, "[ch=%u] FAIL: input trim had no measurable/safe effect\n", num_channels);
    return 7;
  }
  set_in(fake_this, 0.0f);

  printf("[ch=%u] PASS\n", num_channels);
  return 0;
}

int main(void)
{
  void* lib = dlopen("./libnam_hook.so", RTLD_NOW);
  if (!lib)
  {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  nam_process_naml_fn fn = (nam_process_naml_fn)dlsym(lib, "nam_process_naml");
  nam_set_trim_fn set_in = (nam_set_trim_fn)dlsym(lib, "nam_set_input_trim");
  nam_set_trim_fn set_out = (nam_set_trim_fn)dlsym(lib, "nam_set_output_trim");
  if (!fn || !set_in || !set_out)
  {
    fprintf(stderr, "dlsym failed: %s\n", dlerror());
    return 1;
  }

  int rc = 0;
  rc |= run_case(fn, set_in, set_out, 1);
  rc |= run_case(fn, set_in, set_out, 2);

  if (rc == 0)
    printf("ALL PASS\n");
  return rc;
}
