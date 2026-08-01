/* mac_package.c -- see mac_package.h. */
#ifdef __APPLE__
#include "mac_package.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

/* Wraps `path` in single quotes for safe use in a shell command line,
 * escaping any embedded single quotes (') the POSIX-shell way:
 * close-quote, escaped-quote, reopen-quote. */
static void shell_quote(const char* path, char* out, size_t out_size)
{
  size_t pos = 0;
  if (pos < out_size - 1)
    out[pos++] = '\'';
  for (const char* p = path; *p && pos < out_size - 5; ++p)
  {
    if (*p == '\'')
    {
      out[pos++] = '\'';
      out[pos++] = '\\';
      out[pos++] = '\'';
      out[pos++] = '\'';
    }
    else
    {
      out[pos++] = *p;
    }
  }
  if (pos < out_size - 1)
    out[pos++] = '\'';
  out[pos] = '\0';
}

bool nam_mac_find_app_dir(const char* extracted_dir, char* out_path, size_t out_size)
{
  DIR* d = opendir(extracted_dir);
  if (!d)
    return false;
  bool found = false;
  struct dirent* entry;
  while ((entry = readdir(d)) != NULL)
  {
    size_t len = strlen(entry->d_name);
    if (len > 4 && strcmp(entry->d_name + len - 4, ".app") == 0)
    {
      char full[1200];
      snprintf(full, sizeof(full), "%s/%s", extracted_dir, entry->d_name);
      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
      {
        snprintf(out_path, out_size, "%s", full);
        found = true;
        break;
      }
    }
  }
  closedir(d);
  return found;
}

/* Case-insensitive recursive search for a file named "Update.img" inside
 * the extracted updater .app bundle. */
bool nam_mac_find_update_img(const char* dir, char* out_path, size_t out_size)
{
  DIR* d = opendir(dir);
  if (!d)
    return false;
  struct dirent* entry;
  bool found = false;
  while (!found && (entry = readdir(d)) != NULL)
  {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char full[1200];
    snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
    struct stat st;
    if (lstat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
    {
      found = nam_mac_find_update_img(full, out_path, out_size);
    }
    else if (strcasecmp(entry->d_name, "Update.img") == 0)
    {
      snprintf(out_path, out_size, "%s", full);
      found = true;
    }
  }
  closedir(d);
  return found;
}

bool nam_mac_package_app(const char* extracted_dir, const uint8_t* patched_img_data, size_t patched_img_len,
                          const char* output_app_path, char* err, size_t err_size)
{
  if (err && err_size > 0)
    err[0] = '\0'; /* callers check err[0] after a true return to detect the non-fatal codesign warning below */

  char app_dir[1200];
  if (!nam_mac_find_app_dir(extracted_dir, app_dir, sizeof(app_dir)))
  {
    set_err(err, err_size, "no *.app bundle found in the extracted updater zip");
    return false;
  }

  char update_img_path[1200];
  if (!nam_mac_find_update_img(app_dir, update_img_path, sizeof(update_img_path)))
  {
    set_err(err, err_size, "no Update.img found inside %s -- HeadRush may have changed the updater layout",
            app_dir);
    return false;
  }

  FILE* f = fopen(update_img_path, "wb");
  if (!f || (patched_img_len > 0 && fwrite(patched_img_data, 1, patched_img_len, f) != patched_img_len))
  {
    if (f)
      fclose(f);
    set_err(err, err_size, "writing patched Update.img into %s failed", update_img_path);
    return false;
  }
  fclose(f);

  char q_dest[1300], q_app[1300], q_output[1300];
  shell_quote(output_app_path, q_dest, sizeof(q_dest));
  shell_quote(app_dir, q_app, sizeof(q_app));
  shell_quote(output_app_path, q_output, sizeof(q_output));

  char cmd[3000];
  snprintf(cmd, sizeof(cmd), "rm -rf %s && cp -R %s %s", q_dest, q_app, q_output);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "copying %s to %s failed", app_dir, output_app_path);
    return false;
  }

  snprintf(cmd, sizeof(cmd), "codesign --force --deep --sign - %s 2>/dev/null", q_output);
  if (system(cmd) != 0)
  {
    /* A failed ad-hoc re-sign is a warning, not a hard failure -- usually
     * harmless for a non-quarantined local copy. */
    set_err(err, err_size, "ad-hoc codesign of %s failed (non-fatal, continuing)", output_app_path);
  }

  return true;
}
#endif /* __APPLE__ */
