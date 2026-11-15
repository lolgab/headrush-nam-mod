/* test_reverb_eng.c -- exercises libnam_hook.so's nam_process_eng() for a "Reverb" pedal: a fake engine object
 * (knobs at +0x554 decay, +0x540 mix, +0x568 damping, bypass at +0x26b) registered in a fake engine table
 * (NAM_ENG_TAB_ADDR) under the pedal name "Reverb". Checks that mix 0 is dry, that an impulse rings out (the wet
 * tail is non-silent, finite and decays), that a longer decay knob rings longer, and that bypass is dry. A second,
 * "NAM"-named engine must not be treated as a reverb (not checked here: it would start a model load). Run it like
 * test_nam_eng (see docs/ADDITIVE_PEDAL.md): ARM build, NAM_ENG_TAB_ADDR is set by this program before dlopen. */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void (*proc_fn)(void*, uint32_t, float**, uint32_t, float**, int32_t, uint32_t*, void*);

/* fake Qt 5 QString: d -> {ref, size, alloc, offset}; UTF-16 data at d+offset (32-bit layout, 4 words) */
static struct
{
  uint32_t ref, size, alloc, offset;
  uint16_t text[16];
} g_qdata;
static uint32_t g_qstring; /* the QString: one pointer to g_qdata */
static struct
{
  uint32_t count;
  struct
  {
    uint32_t engine, qname;
  } e[8];
} g_tab;

static double tail_energy(proc_fn fn, uint8_t* eng, float decay, int blocks)
{
  uint32_t flags = 0;
  *(float*)(eng + 0x554) = decay;
  float in[48], out[48];
  double e_late = 0;
  for (int b = 0; b < blocks; b++)
  {
    memset(in, 0, sizeof(in));
    if (b == 0)
      in[0] = 1.0f;
    float* ip[1] = {in};
    float* op[1] = {out};
    fn(eng, 0, ip, 1, op, 48, &flags, NULL);
    for (int i = 0; i < 48; i++)
    {
      if (!isfinite(out[i]))
        return -1;
      if (b >= blocks - 100) /* the last 100 blocks (~0.1 s) */
        e_late += (double)out[i] * out[i];
    }
  }
  return e_late;
}

int main(void)
{
  static uint8_t eng[0x700];
  const char* name = "Reverb 2";
  g_qdata.size = (uint32_t)strlen(name);
  g_qdata.offset = (uint32_t)((uintptr_t)g_qdata.text - (uintptr_t)&g_qdata);
  for (size_t i = 0; i < strlen(name); i++)
    g_qdata.text[i] = (uint16_t)name[i];
  g_qstring = (uint32_t)(uintptr_t)&g_qdata;
  g_tab.count = 1;
  g_tab.e[0].engine = (uint32_t)(uintptr_t)eng;
  g_tab.e[0].qname = (uint32_t)(uintptr_t)&g_qstring;
  char env[32];
  snprintf(env, sizeof(env), "%#lx", (unsigned long)(uintptr_t)&g_tab);
  const int record = getenv("TEST_RECORD") != NULL; /* Reverb builds: the stub calls nam_eng_record at creation */
  if (!record)
    setenv("NAM_ENG_TAB_ADDR", env, 1);

  void* lib = dlopen("/t/libnam_hook.so", RTLD_NOW);
  if (!lib)
  {
    printf("dlopen failed: %s\n", dlerror());
    return 2;
  }
  proc_fn fn = (proc_fn)dlsym(lib, "nam_process_eng");
  if (!fn)
    return 3;
  if (record)
  {
    void (*rec)(void*, const void*) = (void (*)(void*, const void*))dlsym(lib, "nam_eng_record");
    if (!rec)
      return 5;
    rec(eng, &g_qstring);
    /* the caller's QString is a temporary: it holds something else by the time audio runs */
    memset(g_qdata.text, 0, sizeof(g_qdata.text));
    g_qstring = 0;
  }

  int rc = 0;
  *(float*)(eng + 0x540) = 0.0f; /* mix 0 */
  *(float*)(eng + 0x568) = 0.5f;
  float in[48], out[48];
  for (int i = 0; i < 48; i++)
    in[i] = 0.3f * sinf(0.2f * (float)i);
  float* ip[1] = {in};
  float* op[1] = {out};
  uint32_t flags = 0;
  for (int k = 0; k < 10; k++)
    fn(eng, 0, ip, 1, op, 48, &flags, NULL);
  double d = 0;
  for (int i = 0; i < 48; i++)
    d = fmax(d, fabs(out[i] - in[i]));
  printf("mix 0 -> max |out-in| = %g (expect 0)\n", d);
  if (d > 1e-6)
    rc = 1;

  *(float*)(eng + 0x540) = 1.0f; /* mix 1: wet only */
  tail_energy(fn, eng, 0.0f, 1500); /* flush what the mix-0 sine left in the tank */
  double e_short = tail_energy(fn, eng, 0.1f, 600);
  double e_long = tail_energy(fn, eng, 0.9f, 600);
  printf("tail energy after ~0.6 s: decay 0.1 -> %g, decay 0.9 -> %g (expect finite, long > short)\n", e_short, e_long);
  if (e_short < 0 || e_long < 0 || !(e_long > e_short))
    rc = 1;

  eng[0x26b] = 1; /* bypass */
  for (int i = 0; i < 48; i++)
    out[i] = 0;
  fn(eng, 0, ip, 1, op, 48, &flags, NULL);
  printf("bypass dry: %s\n", memcmp(in, out, sizeof(in)) == 0 ? "yes" : "no");
  if (memcmp(in, out, sizeof(in)) != 0)
    rc = 1;

  /* dev control channel: force an algorithm and a decay, inject noise, record 0.5 s; then read the status file */
  eng[0x26b] = 0;
  void (*devstart)(void) = (void (*)(void))dlsym(lib, "nam_devctl_start");
  if (!devstart)
    return 4;
  devstart();
  FILE* c = fopen("/tmp/nam_ctl", "w");
  fputs("algo shimmer\nknob decay 0.7\ninject noise 3 0.3\ncapture 0.5\n", c);
  fclose(c);
  for (int k = 0; k < 4000 && access("/tmp/nam_cap.wav", F_OK) != 0; k++)
  {
    fn(eng, 0, ip, 1, op, 48, &flags, NULL);
    usleep(500);
  }
  usleep(600 * 1000);
  for (int k = 0; k < 400; k++)
  {
    fn(eng, 0, ip, 1, op, 48, &flags, NULL);
    usleep(500);
  }
  char st[1500] = {0};
  FILE* f = fopen("/tmp/nam_dev.out", "r");
  if (f)
  {
    fread(st, 1, sizeof st - 1, f);
    fclose(f);
  }
  printf("--- /tmp/nam_dev.out\n%s---\n", st);
  struct stat sb;
  int wav_ok = stat("/tmp/nam_cap.wav", &sb) == 0 && sb.st_size == 44 + 24000 * 4;
  printf("capture wav: %s\n", wav_ok ? "ok (24000 frames)" : "MISSING/WRONG SIZE");
  if (!strstr(st, "algo_used=shimmer") || !strstr(st, "knob_forced=0.70") || !wav_ok || !strstr(st, "done:"))
    rc = 1;
  return rc;
}
