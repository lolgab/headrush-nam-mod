/* test_nam_gonk_e2e.c -- exercises libnam_hook.so's nam_process_gonk() the
 * same way the injected Gonkulator trampoline will: dlopen the .so, dlsym
 * nam_process_gonk, build fake per-channel input/output pointer arrays
 * matching the confirmed ABI (this, param2, float** input, numChannels,
 * float** output, numFrames, flags, ctx), and call it directly.
 *
 * Tests both mono (numChannels=1) and stereo (numChannels=2) paths, and
 * confirms per-channel state independence (dual-mono, not a shared model
 * instance bleeding L into R).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

typedef void (*nam_process_gonk_fn)(void*, uint32_t, float**, uint32_t, float**, int32_t, uint32_t*, void*);

/* nam_process_gonk now drives model selection dynamically from this_+0x2ac
 * (the knob raw value) plus a directory scan (NAM_MODEL_DIR) -- there is no
 * more fixed single-file path for this entry point. A real fake `this` with
 * a stable knob value is required (this_=NULL used to work when the hook
 * only supported one fixed file; now it just skips selection entirely and
 * never loads anything, which is correct behavior for this_=NULL but means
 * this test must supply a real fake_this like test_nam_knob_select.c does). */
static int run_case(nam_process_gonk_fn fn, uint32_t num_channels)
{
  enum { N = 128 };
  float inputL[N], inputR[N], outputL[N], outputR[N];
  for (int i = 0; i < N; i++)
  {
    inputL[i] = 0.5f * sinf(2.0f * (float)M_PI * 220.0f * i / 48000.0f);
    inputR[i] = 0.3f * sinf(2.0f * (float)M_PI * 330.0f * i / 48000.0f);
  }
  memset(outputL, 0, sizeof(outputL));
  memset(outputR, 0, sizeof(outputR));

  float* input[2] = {inputL, inputR};
  float* output[2] = {outputL, outputR};
  uint32_t flags = 0;

  static uint8_t fake_this[0x2b0];
  memset(fake_this, 0, sizeof(fake_this));
  *(float*)(fake_this + 0x2ac) = 0.0f; /* fixed knob value: picks index 0 */

  /* First call: model not loaded yet -> expect dry passthrough on all channels. */
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
    /* L and R carry different signals (220Hz vs 330Hz) -- if dual-mono state
     * is correctly independent, outputs should differ (not identical, since
     * inputs differ and a shared/corrupted instance would likely produce
     * either identical or wildly incoherent output). */
    int channels_differ = memcmp(outputL, outputR, sizeof(outputL)) != 0;
    printf("[ch=%u] channels_differ=%s (expected yes for independent dual-mono state)\n",
           num_channels, channels_differ ? "yes" : "no");
    if (!channels_differ)
    {
      fprintf(stderr, "[ch=%u] FAIL: L/R outputs identical -- state not independent?\n", num_channels);
      return 5;
    }
  }

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
  nam_process_gonk_fn fn = (nam_process_gonk_fn)dlsym(lib, "nam_process_gonk");
  if (!fn)
  {
    fprintf(stderr, "dlsym failed: %s\n", dlerror());
    return 1;
  }

  int rc = 0;
  rc |= run_case(fn, 1);
  rc |= run_case(fn, 2);

  if (rc == 0)
    printf("ALL PASS\n");
  return rc;
}
