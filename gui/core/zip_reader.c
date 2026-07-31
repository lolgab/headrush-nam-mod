/* zip_reader.c -- see zip_reader.h. */
#include "zip_reader.h"

#include "../third_party/miniz/miniz.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

/* `mkdir -p` equivalent. */
static bool mkdir_p(const char* path, char* err, size_t err_size)
{
  char tmp[1200];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  while (len > 0 && tmp[len - 1] == '/')
    tmp[--len] = '\0';

  for (char* p = tmp + 1; *p; ++p)
  {
    if (*p == '/')
    {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
      {
        set_err(err, err_size, "mkdir %s: %s", tmp, strerror(errno));
        return false;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
  {
    set_err(err, err_size, "mkdir %s: %s", tmp, strerror(errno));
    return false;
  }
  return true;
}

bool nam_zip_extract_by_suffix(const uint8_t* zip_data, size_t zip_len, const char* name_suffix, uint8_t** out_data,
                                size_t* out_len, char* err, size_t err_size)
{
  *out_data = NULL;
  *out_len = 0;

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_mem(&zip, zip_data, zip_len, 0))
  {
    set_err(err, err_size, "not a valid zip archive (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    return false;
  }

  int num_files = (int)mz_zip_reader_get_num_files(&zip);
  int found_index = -1;
  char found_name[512] = {0};
  size_t suffix_len = strlen(name_suffix);

  for (int i = 0; i < num_files; ++i)
  {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &stat))
      continue;
    if (stat.m_is_directory)
      continue;
    size_t name_len = strlen(stat.m_filename);
    if (name_len >= suffix_len && strcmp(stat.m_filename + (name_len - suffix_len), name_suffix) == 0)
    {
      found_index = i;
      snprintf(found_name, sizeof(found_name), "%s", stat.m_filename);
      break;
    }
  }

  if (found_index < 0)
  {
    set_err(err, err_size, "no entry ending in %s found in the zip archive", name_suffix);
    mz_zip_reader_end(&zip);
    return false;
  }

  size_t extracted_len = 0;
  void* extracted = mz_zip_reader_extract_to_heap(&zip, (mz_uint)found_index, &extracted_len, 0);
  if (!extracted)
  {
    set_err(err, err_size, "failed to extract %s (%s)", found_name,
            mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    mz_zip_reader_end(&zip);
    return false;
  }

  mz_zip_reader_end(&zip);
  *out_data = (uint8_t*)extracted;
  *out_len = extracted_len;
  return true;
}

#define S_IFLNK_MASK 0170000u
#define S_IFLNK_VAL 0120000u

bool nam_zip_extract_all(const uint8_t* zip_data, size_t zip_len, const char* dest_dir, char* err, size_t err_size)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_mem(&zip, zip_data, zip_len, 0))
  {
    set_err(err, err_size, "not a valid zip archive (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    return false;
  }

  if (!mkdir_p(dest_dir, err, err_size))
  {
    mz_zip_reader_end(&zip);
    return false;
  }

  int num_files = (int)mz_zip_reader_get_num_files(&zip);
  bool ok = true;

  for (int i = 0; i < num_files && ok; ++i)
  {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &stat))
      continue;

    char full_path[1200];
    snprintf(full_path, sizeof(full_path), "%s/%s", dest_dir, stat.m_filename);

    mz_uint host = (mz_uint)(stat.m_version_made_by >> 8);
    uint32_t unix_mode = stat.m_external_attr >> 16;
    bool has_unix_mode = (host == 3) && (unix_mode != 0); /* host 3 == Unix, per the zip spec */
    bool is_symlink = has_unix_mode && ((unix_mode & S_IFLNK_MASK) == S_IFLNK_VAL);

    if (stat.m_is_directory)
    {
      if (!mkdir_p(full_path, err, err_size))
        ok = false;
      continue;
    }

    char parent[1200];
    snprintf(parent, sizeof(parent), "%s", full_path);
    char* slash = strrchr(parent, '/');
    if (slash)
    {
      *slash = '\0';
      if (!mkdir_p(parent, err, err_size))
      {
        ok = false;
        break;
      }
    }

    if (is_symlink)
    {
      size_t link_len = 0;
      void* link_target = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, &link_len, 0);
      if (!link_target)
      {
        set_err(err, err_size, "extracting symlink target for %s (%s)", stat.m_filename,
                mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        ok = false;
        break;
      }
      char target_str[1024];
      size_t n = link_len < sizeof(target_str) - 1 ? link_len : sizeof(target_str) - 1;
      memcpy(target_str, link_target, n);
      target_str[n] = '\0';
      mz_free(link_target);

      unlink(full_path); /* fine if it doesn't exist */
      if (symlink(target_str, full_path) != 0)
      {
        set_err(err, err_size, "creating symlink %s -> %s: %s", full_path, target_str, strerror(errno));
        ok = false;
        break;
      }
      continue;
    }

    if (!mz_zip_reader_extract_to_file(&zip, (mz_uint)i, full_path, 0))
    {
      set_err(err, err_size, "extracting %s (%s)", stat.m_filename,
              mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
      ok = false;
      break;
    }
    if (has_unix_mode)
      chmod(full_path, unix_mode & 07777);
  }

  mz_zip_reader_end(&zip);
  return ok;
}
