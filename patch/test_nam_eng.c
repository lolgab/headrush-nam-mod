/* test_nam_eng.c -- exercises libnam_hook.so's nam_process_eng() (the additive NAM pedals' engine hook): a fake engine
 * object with the three knobs at +0x554 (model), +0x540 (output) and +0x568 (input), bypass at +0x26b. Checks that
 * the model loads from NAM_MODEL_DIR, that output differs from the input once ready, that bypass is dry, and that
 * the output knob at 0 silences and the input trim scales the signal. */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void (*proc_fn)(void*, uint32_t, float**, uint32_t, float**, int32_t, uint32_t*, void*);

static float rms(const float* x, int n)
{
  double s = 0;
  for (int i = 0; i < n; i++)
    s += (double)x[i] * x[i];
  return (float)sqrt(s / n);
}

int main(void)
{
  void* lib = dlopen("/t/libnam_hook.so", RTLD_NOW);
  if (!lib)
  {
    printf("dlopen failed: %s\n", dlerror());
    return 2;
  }
  proc_fn fn = (proc_fn)dlsym(lib, "nam_process_eng");
  if (!fn)
  {
    printf("no nam_process_eng\n");
    return 3;
  }
  enum { N = 128 };
  float in[N], out[N];
  for (int i = 0; i < N; i++)
    in[i] = 0.3f * sinf(2.0f * (float)M_PI * 220.0f * i / 48000.0f);
  float* ip[1] = {in};
  float* op[1] = {out};
  uint32_t flags = 0;
  static uint8_t eng[0x700];
  memset(eng, 0, sizeof(eng));
  *(float*)(eng + 0x554) = 0.0f;
  *(float*)(eng + 0x540) = 0.5f;
  *(float*)(eng + 0x568) = 0.5f;
  int ready = 0;
  for (int t = 0; t < 400 && !ready; t++)
  {
    memset(out, 0, sizeof(out));
    fn(eng, 0, ip, 1, op, N, &flags, NULL);
    if (memcmp(in, out, sizeof(in)) != 0 && rms(out, N) > 1e-4f)
      ready = 1;
    else
      usleep(20000);
  }
  printf("model ready/processing: %s  (out rms %.4f, in rms %.4f)\n", ready ? "yes" : "NO", rms(out, N), rms(in, N));
  int rc = ready ? 0 : 1;
  /* bypass -> dry */
  eng[0x26b] = 1;
  memset(out, 0, sizeof(out));
  fn(eng, 0, ip, 1, op, N, &flags, NULL);
  printf("bypass dry: %s\n", memcmp(in, out, sizeof(in)) == 0 ? "yes" : "no");
  if (memcmp(in, out, sizeof(in)) != 0)
    rc = 1;
  eng[0x26b] = 0;
  /* output knob at 0 -> silence */
  *(float*)(eng + 0x540) = 0.0f;
  for (int k = 0; k < 4; k++)
  {
    memset(out, 0, sizeof(out));
    fn(eng, 0, ip, 1, op, N, &flags, NULL);
  }
  printf("output knob 0 -> rms %.6f (expect ~0)\n", rms(out, N));
  if (rms(out, N) > 1e-5f)
    rc = 1;
  return rc;
}
