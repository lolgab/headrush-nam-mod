/* tempdir.c -- see tempdir.h. */
#include "tempdir.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

#ifdef _WIN32

#include <windows.h>

bool nam_make_temp_dir(char* out_path, size_t out_path_size, char* err, size_t err_size)
{
  char temp_path[MAX_PATH];
  DWORD len = GetTempPathA(sizeof(temp_path), temp_path);
  if (len == 0 || len > sizeof(temp_path))
  {
    set_err(err, err_size, "GetTempPathA failed (error %lu)", (unsigned long)GetLastError());
    return false;
  }

  /* GetTempFileNameA's well-known "make a unique directory" idiom: it
   * atomically creates and reserves a uniquely-named FILE for us (so no
   * two concurrent callers can collide on the same name), which we then
   * delete and immediately recreate as a directory instead. */
  char candidate[MAX_PATH];
  if (GetTempFileNameA(temp_path, "nam", 0, candidate) == 0)
  {
    set_err(err, err_size, "GetTempFileNameA failed (error %lu)", (unsigned long)GetLastError());
    return false;
  }
  DeleteFileA(candidate);

  if (!CreateDirectoryA(candidate, NULL))
  {
    set_err(err, err_size, "CreateDirectoryA %s failed (error %lu)", candidate, (unsigned long)GetLastError());
    return false;
  }

  if (strlen(candidate) >= out_path_size)
  {
    set_err(err, err_size, "temp directory path too long for caller's buffer");
    return false;
  }
  strcpy(out_path, candidate);
  return true;
}

void nam_remove_dir_recursive(const char* path)
{
  char cmd[NAM_TEMPDIR_MAX_PATH + 32];
  snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
  (void)system(cmd); /* best-effort cleanup, matches the callers' prior behavior */
}

#else /* POSIX */

/* <unistd.h> must be included before <stdlib.h> here -- on this project's
 * target SDKs, mkdtemp()'s declaration in <stdlib.h> is only visible once
 * <unistd.h> has already established POSIX-extension visibility; without
 * it, mkdtemp() fails to compile even under -std=gnu11 (confirmed: adding
 * _DEFAULT_SOURCE alone, the usual glibc fix, was NOT sufficient on
 * Apple's libc -- this include order is what actually works on both). */
#include <unistd.h>
#include <stdlib.h>

bool nam_make_temp_dir(char* out_path, size_t out_path_size, char* err, size_t err_size)
{
  static const char TEMPLATE[] = "/tmp/headrush-nam-build-XXXXXX";
  if (out_path_size < sizeof(TEMPLATE))
  {
    set_err(err, err_size, "caller's buffer too small for the temp directory template");
    return false;
  }
  memcpy(out_path, TEMPLATE, sizeof(TEMPLATE));
  if (!mkdtemp(out_path))
  {
    set_err(err, err_size, "mkdtemp failed");
    return false;
  }
  return true;
}

void nam_remove_dir_recursive(const char* path)
{
  char cmd[NAM_TEMPDIR_MAX_PATH + 16];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  (void)system(cmd); /* best-effort cleanup, matches the callers' prior behavior */
}

#endif
