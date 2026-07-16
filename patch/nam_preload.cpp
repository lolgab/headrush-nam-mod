// nam_preload.cpp -- tiny LD_PRELOAD shim.
//
// Loaded before /usr/Evil/Evil starts (set LD_PRELOAD=/usr/Evil/libnam_preload.so
// in evil.service or runevil), this constructor dlopen()s libnam_hook.so and
// wires up whichever hook(s) are configured (each independently, via its own
// env var -- see below), writing the resolved function pointer into the
// corresponding trampoline's hook_slot (injected by patch_evil.py /
// patch_gonkulator.py).
//
// Evil is non-PIE / no ASLR (confirmed: ELF type EXEC, pic=false), so hook_slot
// addresses are fixed, reproducible vaddrs across runs -- no runtime lookup
// needed.
//
// CURRENT (additive) design: NAM_HOOK_SLOT_NAML_ADDR -> nam_process_naml,
// NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR -> nam_set_input_trim,
// NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR -> nam_set_output_trim (all three from
// patch_namloader.py's injected segment). SUPERSEDED designs, kept only as
// reference/fallback: NAM_HOOK_SLOT_GONK_ADDR -> nam_process_gonk (Gonkulator
// hijack -- overwrites the REAL Gonkulator pedal's process() everywhere,
// rejected by the user as a final design) and NAM_HOOK_SLOT_ADDR ->
// nam_process (IRLoader hook -- overwrites real .wav IR loading everywhere).
// Only set a superseded design's env var if you deliberately want that old
// behavior back; see nam_hook.cpp's file header for why each was rejected.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

#define NAM_HOOK_LIB_PATH "/usr/Evil/libnam_hook.so"

namespace
{

// Installs one hook: dlsym `sym_name` from `lib`, write it into the hook_slot
// whose address comes from env var `env_name` (skipped entirely if unset --
// no fallback default, since guessing wrong here silently no-ops the hook
// instead of crashing, which is much harder to notice).
void install_one_hook(void* lib, const char* sym_name, const char* env_name)
{
  const char* env = getenv(env_name);
  if (!env)
  {
    fprintf(stderr, "[nam_preload] %s not set, skipping %s hook\n", env_name, sym_name);
    return;
  }

  void* sym = dlsym(lib, sym_name);
  if (!sym)
  {
    fprintf(stderr, "[nam_preload] dlsym(%s) failed: %s\n", sym_name, dlerror());
    return;
  }

  const uintptr_t slot_addr = static_cast<uintptr_t>(strtoul(env, nullptr, 0));
  volatile auto* slot = reinterpret_cast<volatile uintptr_t*>(slot_addr);
  *slot = reinterpret_cast<uintptr_t>(sym);

  fprintf(stderr, "[nam_preload] hook installed: %s slot@0x%lx = %p\n",
          sym_name, static_cast<unsigned long>(slot_addr), sym);
}

} // namespace

__attribute__((constructor)) static void install_nam_hooks()
{
  void* lib = dlopen(NAM_HOOK_LIB_PATH, RTLD_NOW | RTLD_GLOBAL);
  if (!lib)
  {
    fprintf(stderr, "[nam_preload] dlopen(%s) failed: %s\n", NAM_HOOK_LIB_PATH, dlerror());
    return;
  }

  install_one_hook(lib, "nam_process_naml", "NAM_HOOK_SLOT_NAML_ADDR");
  install_one_hook(lib, "nam_set_input_trim", "NAM_HOOK_SLOT_NAML_TRIM_IN_ADDR");
  install_one_hook(lib, "nam_set_output_trim", "NAM_HOOK_SLOT_NAML_TRIM_OUT_ADDR");
  install_one_hook(lib, "nam_process_gonk", "NAM_HOOK_SLOT_GONK_ADDR");
  install_one_hook(lib, "nam_process", "NAM_HOOK_SLOT_ADDR");
}
