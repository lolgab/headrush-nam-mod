/* blobs_locate.c -- see blobs_locate.h. */
#include "blobs_locate.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static bool file_exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

/* Writes the running executable's own path into `out`. Returns false if
 * the platform API fails or the path doesn't fit. */
static bool get_exe_path(char* out, size_t out_size)
{
#if defined(__APPLE__)
  uint32_t size = (uint32_t)out_size;
  return _NSGetExecutablePath(out, &size) == 0;
#elif defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_size);
  return n > 0 && n < out_size;
#else
  ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
  if (n <= 0)
    return false;
  out[n] = '\0';
  return true;
#endif
}

static void dirname_inplace(char* path)
{
  char* slash = strrchr(path, '/');
#ifdef _WIN32
  char* backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  if (slash)
    *slash = '\0';
  else
    snprintf(path, 2, ".");
}

bool nam_locate_blobs_dir(char* out, size_t out_size, char* err, size_t err_size)
{
  char exe_path[1024];
  char exe_dir[1024];
  char candidate[1200];
  bool have_exe_dir = get_exe_path(exe_path, sizeof(exe_path));
  if (have_exe_dir)
  {
    snprintf(exe_dir, sizeof(exe_dir), "%s", exe_path);
    dirname_inplace(exe_dir);

    /* Distributed layout: blobs/ shipped next to the executable. */
    snprintf(candidate, sizeof(candidate), "%s/blobs/trampoline_gonk.bin", exe_dir);
    if (file_exists(candidate))
    {
      snprintf(out, out_size, "%s/blobs", exe_dir);
      return true;
    }

    /* Local dev build layout: gui/build/<exe>, gui/blobs/. */
    snprintf(candidate, sizeof(candidate), "%s/../blobs/trampoline_gonk.bin", exe_dir);
    if (file_exists(candidate))
    {
      snprintf(out, out_size, "%s/../blobs", exe_dir);
      return true;
    }
  }

  /* Last resort: cwd-relative, for running gui-core-cli by hand from the
   * repo root. */
  if (file_exists("gui/blobs/trampoline_gonk.bin"))
  {
    snprintf(out, out_size, "gui/blobs");
    return true;
  }

  if (err && err_size > 0)
    snprintf(err, err_size, "could not find gui/blobs/trampoline_gonk.bin next to the executable, in "
                            "its parent directory, or in gui/blobs relative to the current directory");
  return false;
}
