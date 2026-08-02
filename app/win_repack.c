/* win_repack.c -- see win_repack.h. */
#include "win_repack.h"

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

/* This whole file only ever compiles for _WIN32 (see CMakeLists.txt) --
 * every system() call below is deliberately cmd.exe syntax (double-quote
 * paths, "mkdir" with no -p since Windows' own mkdir already creates
 * intermediate directories, ">nul" not ">/dev/null"), not a portable
 * shell abstraction. No embedded-quote escaping: every path passed
 * through here is built from our own temp directory plus fixed filenames
 * (see EXPECTED_ENTRIES below), never arbitrary/user-supplied text. */
static void shell_quote(const char* path, char* out, size_t out_size)
{
  snprintf(out, out_size, "\"%s\"", path);
}

static uint32_t rd32(const uint8_t* d, size_t off)
{
  return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16)
         | ((uint32_t)d[off + 3] << 24);
}

static uint16_t rd16(const uint8_t* d, size_t off)
{
  return (uint16_t)(d[off] | (d[off + 1] << 8));
}

static void wr32(uint8_t* d, size_t off, uint32_t v)
{
  d[off] = (uint8_t)v;
  d[off + 1] = (uint8_t)(v >> 8);
  d[off + 2] = (uint8_t)(v >> 16);
  d[off + 3] = (uint8_t)(v >> 24);
}

bool nam_pe_strip_authenticode_directory(uint8_t* pe_data, size_t pe_data_len, char* err, size_t err_size)
{
  if (pe_data_len < 0x40)
  {
    set_err(err, err_size, "file too small to contain a PE e_lfanew field");
    return false;
  }
  uint32_t e_lfanew = rd32(pe_data, 0x3C);
  if ((size_t)e_lfanew + 4 > pe_data_len || memcmp(pe_data + e_lfanew, "PE\0\0", 4) != 0)
  {
    set_err(err, err_size,
            "stock updater's PE header doesn't look like a standard PE32 image -- re-verify this tool's offset "
            "math before proceeding.");
    return false;
  }

  uint32_t coff_off = e_lfanew + 4;
  if ((size_t)coff_off + 20 > pe_data_len)
  {
    set_err(err, err_size, "PE COFF header out of bounds");
    return false;
  }
  uint32_t opt_off = coff_off + 20;
  if ((size_t)opt_off + 2 > pe_data_len)
  {
    set_err(err, err_size, "PE optional header out of bounds");
    return false;
  }
  uint16_t magic = rd16(pe_data, opt_off);
  if (magic != 0x10B)
  {
    set_err(err, err_size,
            "expected a PE32 (not PE32+) optional header (magic 0x10b), got 0x%x -- re-verify this tool's offset "
            "math before proceeding.",
            (unsigned)magic);
    return false;
  }

  uint32_t numrva_off = opt_off + 92;
  if ((size_t)numrva_off + 4 > pe_data_len)
  {
    set_err(err, err_size, "PE data directory count field out of bounds");
    return false;
  }
  uint32_t numrva = rd32(pe_data, numrva_off);
  if (numrva < 5)
  {
    set_err(err, err_size,
            "PE optional header has only %u data directories, expected at least 5 (need index 4, Security) -- "
            "re-verify this tool's offset math.",
            (unsigned)numrva);
    return false;
  }

  uint32_t sec_entry_off = numrva_off + 4 + 4 * 8; /* index 4 == IMAGE_DIRECTORY_ENTRY_SECURITY */
  if ((size_t)sec_entry_off + 8 > pe_data_len)
  {
    set_err(err, err_size, "Security data directory entry out of bounds");
    return false;
  }
  wr32(pe_data, sec_entry_off, 0);
  wr32(pe_data, sec_entry_off + 4, 0);
  return true;
}

static const char* const EXPECTED_ENTRIES[] = {
  /* sorted() order, matching repack_windows_updater.py's `7z a` invocation byte-for-byte */
  "Background.png", "Config.json", "FirmwareUpdater.exe", "Update.img", "libusb-1.0.dll",
};
#define EXPECTED_ENTRY_COUNT (sizeof(EXPECTED_ENTRIES) / sizeof(EXPECTED_ENTRIES[0]))

static uint8_t* read_whole_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0)
  {
    fclose(f);
    return NULL;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
  if (buf && sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf)
    *out_len = (size_t)sz;
  return buf;
}

static bool write_whole_file(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
  fclose(f);
  return ok;
}

static bool file_exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

#define SFX_END_MARKER ";!@InstallEnd@!"

static long find_marker(const uint8_t* data, size_t len, const char* marker)
{
  size_t marker_len = strlen(marker);
  if (len < marker_len)
    return -1;
  for (size_t i = 0; i <= len - marker_len; ++i)
    if (memcmp(data + i, marker, marker_len) == 0)
      return (long)i;
  return -1;
}

bool nam_win_extract_stock_img(const char* stock_exe_path, const char* sevenzip_path, const char* workdir,
                                uint8_t** out_data, size_t* out_len, char* err, size_t err_size)
{
  *out_data = NULL;
  *out_len = 0;

  size_t stock_len;
  uint8_t* stock_data = read_whole_file(stock_exe_path, &stock_len);
  if (!stock_data)
  {
    set_err(err, err_size, "reading %s failed", stock_exe_path);
    return false;
  }

  long marker_idx = find_marker(stock_data, stock_len, SFX_END_MARKER);
  if (marker_idx < 0)
  {
    set_err(err, err_size,
            "%s doesn't look like a 7-Zip SFX installer (no %s marker found) -- HeadRush may have changed the "
            "Windows updater's packaging.",
            stock_exe_path, SFX_END_MARKER);
    free(stock_data);
    return false;
  }
  size_t archive_start = (size_t)marker_idx + strlen(SFX_END_MARKER);

  char stock7z_path[700], payload_dir[700];
  snprintf(stock7z_path, sizeof(stock7z_path), "%s/stock.7z", workdir);
  snprintf(payload_dir, sizeof(payload_dir), "%s/payload", workdir);

  bool wrote = write_whole_file(stock7z_path, stock_data + archive_start, stock_len - archive_start);
  free(stock_data);
  if (!wrote)
  {
    set_err(err, err_size, "writing embedded 7z archive to %s failed", stock7z_path);
    return false;
  }

  char q1[1300], q2[1300], q3[1300], cmd[3200];
  shell_quote(payload_dir, q1, sizeof(q1));
  shell_quote(stock7z_path, q2, sizeof(q2));
  shell_quote(sevenzip_path, q3, sizeof(q3));
  snprintf(cmd, sizeof(cmd), "if not exist %s mkdir %s && %s x -o%s %s -y >nul", q1, q1, q3, q1, q2);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "extracting the stock updater's embedded 7z archive failed");
    return false;
  }

  char update_img_path[900];
  snprintf(update_img_path, sizeof(update_img_path), "%s/Update.img", payload_dir);
  uint8_t* img_data = read_whole_file(update_img_path, out_len);
  if (!img_data)
  {
    set_err(err, err_size, "no Update.img found inside %s -- HeadRush may have changed the updater layout",
            stock_exe_path);
    return false;
  }

  *out_data = img_data;
  return true;
}

bool nam_win_repack_updater(const char* stock_exe_path, const uint8_t* patched_img_data, size_t patched_img_len,
                            const char* output_exe_path, const char* sevenzip_path, const char* workdir, char* err,
                            size_t err_size)
{
  size_t stock_len;
  uint8_t* stock_data = read_whole_file(stock_exe_path, &stock_len);
  if (!stock_data)
  {
    set_err(err, err_size, "reading %s failed", stock_exe_path);
    return false;
  }

  long marker_idx = find_marker(stock_data, stock_len, SFX_END_MARKER);
  if (marker_idx < 0)
  {
    set_err(err, err_size,
            "%s doesn't look like a 7-Zip SFX installer (no %s marker found) -- HeadRush may have changed the "
            "Windows updater's packaging.",
            stock_exe_path, SFX_END_MARKER);
    free(stock_data);
    return false;
  }
  size_t archive_start = (size_t)marker_idx + strlen(SFX_END_MARKER);

  uint8_t* sfx_prefix = (uint8_t*)malloc(archive_start);
  memcpy(sfx_prefix, stock_data, archive_start);
  if (!nam_pe_strip_authenticode_directory(sfx_prefix, archive_start, err, err_size))
  {
    free(sfx_prefix);
    free(stock_data);
    return false;
  }

  char q1[1300], q2[1300], qexe[1300];
  char stock7z_path[700], payload_dir[700], new7z_path[700], verify_dir[700];
  snprintf(stock7z_path, sizeof(stock7z_path), "%s/stock.7z", workdir);
  snprintf(payload_dir, sizeof(payload_dir), "%s/payload", workdir);
  snprintf(new7z_path, sizeof(new7z_path), "%s/new.7z", workdir);
  snprintf(verify_dir, sizeof(verify_dir), "%s/verify", workdir);
  shell_quote(sevenzip_path, qexe, sizeof(qexe));

  if (!write_whole_file(stock7z_path, stock_data + archive_start, stock_len - archive_start))
  {
    set_err(err, err_size, "writing embedded 7z archive to %s failed", stock7z_path);
    free(sfx_prefix);
    free(stock_data);
    return false;
  }
  free(stock_data);

  char cmd[3200];
  shell_quote(payload_dir, q1, sizeof(q1));
  shell_quote(stock7z_path, q2, sizeof(q2));
  snprintf(cmd, sizeof(cmd), "if not exist %s mkdir %s && %s x -o%s %s -y >nul", q1, q1, qexe, q1, q2);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "extracting the stock updater's embedded 7z archive failed");
    free(sfx_prefix);
    return false;
  }

  for (size_t i = 0; i < EXPECTED_ENTRY_COUNT; ++i)
  {
    char p[900];
    snprintf(p, sizeof(p), "%s/%s", payload_dir, EXPECTED_ENTRIES[i]);
    if (!file_exists(p))
    {
      set_err(err, err_size,
              "stock updater's archive contents changed -- expected entry %s missing. Re-verify this tool against "
              "the new layout before proceeding.",
              EXPECTED_ENTRIES[i]);
      free(sfx_prefix);
      return false;
    }
  }

  char update_img_payload_path[900];
  snprintf(update_img_payload_path, sizeof(update_img_payload_path), "%s/Update.img", payload_dir);
  if (!write_whole_file(update_img_payload_path, patched_img_data, patched_img_len))
  {
    set_err(err, err_size, "writing patched Update.img into %s failed", update_img_payload_path);
    free(sfx_prefix);
    return false;
  }

  shell_quote(new7z_path, q1, sizeof(q1));
  shell_quote(payload_dir, q2, sizeof(q2));
  char names[600] = {0};
  for (size_t i = 0; i < EXPECTED_ENTRY_COUNT; ++i)
  {
    char qn[300];
    shell_quote(EXPECTED_ENTRIES[i], qn, sizeof(qn));
    strncat(names, qn, sizeof(names) - strlen(names) - 1);
    strncat(names, " ", sizeof(names) - strlen(names) - 1);
  }
  snprintf(cmd, sizeof(cmd), "cd /d %s && %s a %s %s >nul", q2, qexe, q1, names);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "creating the new 7z archive failed");
    free(sfx_prefix);
    return false;
  }

  size_t new7z_len;
  uint8_t* new7z_data = read_whole_file(new7z_path, &new7z_len);
  if (!new7z_data)
  {
    set_err(err, err_size, "reading %s failed", new7z_path);
    free(sfx_prefix);
    return false;
  }

  FILE* out = fopen(output_exe_path, "wb");
  bool write_ok = out && fwrite(sfx_prefix, 1, archive_start, out) == archive_start
                  && fwrite(new7z_data, 1, new7z_len, out) == new7z_len;
  if (out)
    fclose(out);
  free(sfx_prefix);
  free(new7z_data);
  if (!write_ok)
  {
    set_err(err, err_size, "writing %s failed", output_exe_path);
    return false;
  }

  /* ---- round-trip verification ---- */
  shell_quote(output_exe_path, q1, sizeof(q1));
  snprintf(cmd, sizeof(cmd), "%s t %s >nul", qexe, q1);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "7z integrity test on the repacked .exe failed -- refusing to leave a broken installer "
                            "in place");
    return false;
  }

  shell_quote(verify_dir, q2, sizeof(q2));
  snprintf(cmd, sizeof(cmd), "if not exist %s mkdir %s && %s x -o%s %s -y >nul", q2, q2, qexe, q2, q1);
  if (system(cmd) != 0)
  {
    set_err(err, err_size, "round-trip FAILED: couldn't extract the repacked .exe for verification");
    return false;
  }

  for (size_t i = 0; i < EXPECTED_ENTRY_COUNT; ++i)
  {
    char verify_path[900], payload_path[900];
    snprintf(verify_path, sizeof(verify_path), "%s/%s", verify_dir, EXPECTED_ENTRIES[i]);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", payload_dir, EXPECTED_ENTRIES[i]);

    size_t vlen;
    uint8_t* vdata = read_whole_file(verify_path, &vlen);
    if (!vdata)
    {
      set_err(err, err_size, "round-trip FAILED: %s missing from the repacked .exe", EXPECTED_ENTRIES[i]);
      return false;
    }

    if (strcmp(EXPECTED_ENTRIES[i], "Update.img") == 0)
    {
      bool match = vlen == patched_img_len && memcmp(vdata, patched_img_data, vlen) == 0;
      free(vdata);
      if (!match)
      {
        set_err(err, err_size, "round-trip FAILED: repacked .exe's Update.img doesn't match the patched image "
                                "byte-exact");
        return false;
      }
    }
    else
    {
      size_t plen;
      uint8_t* pdata = read_whole_file(payload_path, &plen);
      bool match = pdata && vlen == plen && memcmp(vdata, pdata, vlen) == 0;
      free(vdata);
      free(pdata);
      if (!match)
      {
        set_err(err, err_size, "round-trip FAILED: repacked .exe's %s doesn't match the stock one byte-exact",
                EXPECTED_ENTRIES[i]);
        return false;
      }
    }
  }

  return true;
}
