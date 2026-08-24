/* nam_library.c -- see nam_library.h. */
#include "nam_library.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uuid/uuid.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

/* One raw *.nam file as found on disk, before final ordering/indexing. */
typedef struct
{
  char filename[NAM_LIBRARY_MAX_NAME]; /* full on-disk name, incl. extension */
  char base_name[NAM_LIBRARY_MAX_NAME]; /* filename with any leading "NNN - " stripped (extension kept) */
  bool has_prefix;
  int prefix_value;
} ScanEntry;

static bool has_nam_extension(const char* name, size_t len)
{
  return len > 4 && strcasecmp(name + len - 4, ".nam") == 0;
}

/* Detects a well-formed "NNN - " prefix (3 digits, space, hyphen, space)
 * at the start of `name_no_prefix_check`. On success, *base_name points
 * to the remainder (still owned by the caller's buffer). */
static bool parse_prefix(const char* name, int* out_value, const char** out_rest)
{
  if (!(isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) && isdigit((unsigned char)name[2])))
    return false;
  if (!(name[3] == ' ' && name[4] == '-' && name[5] == ' '))
    return false;
  *out_value = (name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0');
  *out_rest = name + 6;
  return true;
}

static int cmp_prefixed(const void* a, const void* b)
{
  const ScanEntry* ea = (const ScanEntry*)a;
  const ScanEntry* eb = (const ScanEntry*)b;
  if (ea->prefix_value != eb->prefix_value)
    return ea->prefix_value - eb->prefix_value;
  return strcasecmp(ea->base_name, eb->base_name);
}

static int cmp_unprefixed(const void* a, const void* b)
{
  const ScanEntry* ea = (const ScanEntry*)a;
  const ScanEntry* eb = (const ScanEntry*)b;
  return strcasecmp(ea->base_name, eb->base_name);
}

static void reindex_item(NamLibraryItem* item, int index)
{
  item->index = index;
  snprintf(item->new_filename, sizeof(item->new_filename), "%03d - %s", index, item->base_name);
  item->needs_rename = strcmp(item->old_filename, item->new_filename) != 0;
}

void nam_library_swap_items(NamLibraryPlan* plan, int i, int j)
{
  if (i == j || i < 0 || j < 0 || i >= plan->count || j >= plan->count)
    return;
  NamLibraryItem tmp = plan->items[i];
  plan->items[i] = plan->items[j];
  plan->items[j] = tmp;
  reindex_item(&plan->items[i], i);
  reindex_item(&plan->items[j], j);

  int rename_count = 0;
  for (int k = 0; k < plan->count; ++k)
    if (plan->items[k].needs_rename)
      rename_count++;
  plan->rename_count = rename_count;
}

static bool is_dir(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* True if `volume_root` has one of the HeadRush's other default folders
 * (present on a stock device already, per README -- unlike /NAM, which
 * the user normally has to create by hand), so it's reasonable to treat
 * it as a HeadRush drive even before /NAM exists on it. */
static bool looks_like_headrush_drive(const char* volume_root)
{
  char path[NAM_LIBRARY_MAX_PATH];

  /* "Impulse Responses" alone is distinctive enough (an unrelated drive
   * having this exact folder name is very unlikely). */
  snprintf(path, sizeof(path), "%s/Impulse Responses", volume_root);
  if (is_dir(path))
    return true;

  /* "Blocks" and "Rigs" alone are common enough words that either one by
   * itself risks a false-positive match on some unrelated drive -- only
   * trust them together. */
  snprintf(path, sizeof(path), "%s/Blocks", volume_root);
  bool has_blocks = is_dir(path);
  snprintf(path, sizeof(path), "%s/Rigs", volume_root);
  bool has_rigs = is_dir(path);
  return has_blocks && has_rigs;
}

/* If `volume_root` already has a NAM folder, or looks like a HeadRush
 * drive without one yet (see looks_like_headrush_drive), fills *out and
 * returns true. */
static bool probe_volume_root(const char* volume_root, NamLibraryDriveCandidate* out)
{
  char nam_path[NAM_LIBRARY_MAX_PATH];
  snprintf(nam_path, sizeof(nam_path), "%s/NAM", volume_root);
  bool nam_exists = is_dir(nam_path);
  if (!nam_exists && !looks_like_headrush_drive(volume_root))
    return false;

  snprintf(out->nam_dir, sizeof(out->nam_dir), "%s", nam_path);
  out->nam_dir_exists = nam_exists;
  snprintf(out->blocks_dir, sizeof(out->blocks_dir), "%s/Blocks", volume_root);
  out->blocks_dir_exists = is_dir(out->blocks_dir);
  return true;
}

/* Probes every subdirectory of `parent_dir` as a candidate volume root.
 * Silently does nothing if parent_dir itself doesn't exist/isn't
 * readable -- not every probed location exists on every OS/setup. */
static void scan_subdirs_for_nam(const char* parent_dir, NamLibraryDriveCandidate* out, int max_out, int* count)
{
  DIR* d = opendir(parent_dir);
  if (!d)
    return;
  struct dirent* de;
  while (*count < max_out && (de = readdir(d)) != NULL)
  {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    char sub[NAM_LIBRARY_MAX_PATH];
    snprintf(sub, sizeof(sub), "%s/%s", parent_dir, de->d_name);
    if (!is_dir(sub))
      continue;
    if (probe_volume_root(sub, &out[*count]))
      (*count)++;
  }
  closedir(d);
}

int nam_library_find_drives(NamLibraryDriveCandidate* out, int max_out)
{
  int count = 0;
  if (max_out <= 0)
    return 0;

#if defined(__APPLE__)
  scan_subdirs_for_nam("/Volumes", out, max_out, &count);
#elif defined(_WIN32)
  DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26 && count < max_out; ++i)
  {
    if (!(mask & (1u << i)))
      continue;
    char root[4];
    snprintf(root, sizeof(root), "%c:", 'A' + i);
    if (probe_volume_root(root, &out[count]))
      count++;
  }
#else
  scan_subdirs_for_nam("/media", out, max_out, &count);
  const char* user = getenv("USER");
  if (user)
  {
    char media_user[NAM_LIBRARY_MAX_PATH];
    snprintf(media_user, sizeof(media_user), "/media/%s", user);
    scan_subdirs_for_nam(media_user, out, max_out, &count);

    char run_media_user[NAM_LIBRARY_MAX_PATH];
    snprintf(run_media_user, sizeof(run_media_user), "/run/media/%s", user);
    scan_subdirs_for_nam(run_media_user, out, max_out, &count);
  }
  scan_subdirs_for_nam("/mnt", out, max_out, &count);
#endif

  return count;
}

bool nam_library_ensure_dir(const char* dir, char* err, size_t err_size)
{
  if (is_dir(dir))
    return true;

#ifdef _WIN32
  if (CreateDirectoryA(dir, NULL))
    return true;
  set_err(err, err_size, "creating %s failed (error %lu)", dir, (unsigned long)GetLastError());
#else
  if (mkdir(dir, 0755) == 0)
    return true;
  set_err(err, err_size, "creating %s failed: %s", dir, strerror(errno));
#endif
  return false;
}

bool nam_library_scan(const char* dir, NamLibraryPlan* out_plan, char* err, size_t err_size)
{
  memset(out_plan, 0, sizeof(*out_plan));

  DIR* d = opendir(dir);
  if (!d)
  {
    set_err(err, err_size, "opening %s failed: %s", dir, strerror(errno));
    return false;
  }

  ScanEntry* entries = NULL;
  size_t capacity = 0;
  size_t count = 0;

  struct dirent* de;
  while ((de = readdir(d)) != NULL)
  {
    const char* name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    /* Skip macOS AppleDouble sidecars ("._realname.nam") -- same filter
     * patch/nam_hook.cpp applies, so this plan matches what the firmware
     * will actually load. */
    if (name[0] == '.' && name[1] == '_')
      continue;
    size_t len = strlen(name);
    if (!has_nam_extension(name, len))
      continue;
    if (len >= NAM_LIBRARY_MAX_NAME)
      continue; /* absurdly long filename -- silently ignored, matches firmware's own JSON-parse-failure tolerance */

    char full_path[2048];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, name);
    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    if (count == capacity)
    {
      size_t new_capacity = capacity ? capacity * 2 : 32;
      ScanEntry* grown = (ScanEntry*)realloc(entries, new_capacity * sizeof(ScanEntry));
      if (!grown)
      {
        free(entries);
        closedir(d);
        set_err(err, err_size, "out of memory scanning %s", dir);
        return false;
      }
      entries = grown;
      capacity = new_capacity;
    }

    ScanEntry* e = &entries[count];
    snprintf(e->filename, sizeof(e->filename), "%s", name);
    int prefix_value;
    const char* rest;
    if (parse_prefix(name, &prefix_value, &rest))
    {
      e->has_prefix = true;
      e->prefix_value = prefix_value;
      snprintf(e->base_name, sizeof(e->base_name), "%s", rest);
    }
    else
    {
      e->has_prefix = false;
      e->prefix_value = 0;
      snprintf(e->base_name, sizeof(e->base_name), "%s", name);
    }
    count++;
  }
  closedir(d);

  if (count > NAM_LIBRARY_MAX_MODELS)
  {
    size_t excess = count - NAM_LIBRARY_MAX_MODELS;
    set_err(err, err_size,
            "too many NAM models: found %zu, but the Model knob only has %d positions -- remove %zu model%s before "
            "continuing (nothing has been renamed, no blocks generated)",
            count, NAM_LIBRARY_MAX_MODELS, excess, excess == 1 ? "" : "s");
    free(entries);
    return false;
  }

  /* Partition into prefixed/unprefixed, preserving each ScanEntry's data. */
  ScanEntry* prefixed = (ScanEntry*)malloc(count ? count * sizeof(ScanEntry) : 1);
  ScanEntry* unprefixed = (ScanEntry*)malloc(count ? count * sizeof(ScanEntry) : 1);
  size_t prefixed_count = 0, unprefixed_count = 0;
  for (size_t i = 0; i < count; ++i)
  {
    if (entries[i].has_prefix)
      prefixed[prefixed_count++] = entries[i];
    else
      unprefixed[unprefixed_count++] = entries[i];
  }
  free(entries);

  qsort(prefixed, prefixed_count, sizeof(ScanEntry), cmp_prefixed);
  qsort(unprefixed, unprefixed_count, sizeof(ScanEntry), cmp_unprefixed);

  int duplicate_groups = 0;
  for (size_t i = 0; i + 1 < prefixed_count; ++i)
  {
    if (prefixed[i].prefix_value == prefixed[i + 1].prefix_value)
    {
      duplicate_groups++;
      /* Skip the rest of this run so a group of 3+ duplicates only counts once. */
      while (i + 1 < prefixed_count && prefixed[i].prefix_value == prefixed[i + 1].prefix_value)
        i++;
    }
  }

  /* Final order: existing prefixed models first (renumbered contiguously,
   * closing any gaps -- see nam_library.h's header comment for why gaps
   * can't be preserved), then newly-discovered unprefixed models appended
   * in alphabetical order. Concatenating like this is always correct
   * regardless of how the two groups interleave alphabetically: once
   * every file gets a fresh zero-padded "NNN - " prefix matching its
   * position here, the firmware's own alphabetical sort reproduces
   * exactly this order (the zero-padded numeric prefix always dominates
   * the string comparison), so index i really will be rank i on the
   * device. */
  int index = 0;
  for (size_t i = 0; i < prefixed_count; ++i, ++index)
  {
    NamLibraryItem* item = &out_plan->items[index];
    snprintf(item->old_filename, sizeof(item->old_filename), "%s", prefixed[i].filename);
    snprintf(item->base_name, sizeof(item->base_name), "%s", prefixed[i].base_name);
    item->had_prefix = true;
    reindex_item(item, index);
  }
  for (size_t i = 0; i < unprefixed_count; ++i, ++index)
  {
    NamLibraryItem* item = &out_plan->items[index];
    snprintf(item->old_filename, sizeof(item->old_filename), "%s", unprefixed[i].filename);
    snprintf(item->base_name, sizeof(item->base_name), "%s", unprefixed[i].base_name);
    item->had_prefix = false;
    reindex_item(item, index); /* unprefixed files always end up needing a rename -- they have no prefix yet */
  }

  free(prefixed);
  free(unprefixed);

  out_plan->count = index;
  out_plan->duplicate_prefix_groups = duplicate_groups;
  int rename_count = 0;
  for (int i = 0; i < out_plan->count; ++i)
    if (out_plan->items[i].needs_rename)
      rename_count++;
  out_plan->rename_count = rename_count;
  return true;
}

bool nam_library_make_block_json(int drive, int tone, int level, bool v2, char* out_json, size_t out_json_size,
                                  char* uuid_out)
{
  /* A raw 16-byte buffer, deliberately not declared as `uuid_t`: on the
   * Windows build, <windows.h> (included below for GetLogicalDrives/
   * CreateDirectoryA) drags in the Windows SDK's own `uuid_t`/`UUID`
   * (aka GUID, a struct) after e2fsprogs' <uuid/uuid.h> already fixed
   * uuid_generate_random()/uuid_unparse_lower()'s real parameter type as
   * plain `unsigned char *` -- a variable declared `uuid_t` at this point
   * in the file would pick up the wrong (GUID) type and fail to compile
   * (confirmed by CI). An explicit byte array always decays to the
   * `unsigned char *` both functions actually expect, on every platform. */
  unsigned char uuid[16];
  char uuid_str[37];
  uuid_generate_random(uuid);
  uuid_unparse_lower(uuid, uuid_str);
  if (uuid_out)
    snprintf(uuid_out, 37, "%s", uuid_str);

  /* v1 vs v2 differ only in the pedal type ("ANXIETY OD" / "ANXIETY OD V2")
   * and the matching inner content data key ("Anxiety OD" / "Anxiety OD
   * V2") -- see nam_library.h's comment on this function for how that
   * naming was derived. Everything else (Drive/Tone/Level/Hi-Lo,
   * childorder) is identical, per docs/TECHNICAL.md's confirmation that
   * v1 and v2 share the same underlying pedal engine. */
  const char* type = v2 ? "ANXIETY OD V2" : "ANXIETY OD";
  const char* data_key = v2 ? "Anxiety OD V2" : "Anxiety OD";

  /* The inner "content" value is itself JSON, escaped as a string, per
   * the .block format an existing HeadRush transform tested successfully
   * (see the linked GitHub issue). Drive/Tone/Level are user-controlled
   * ints and type/data_key come from the fixed pair above (no arbitrary
   * user string ever reaches this template), so no further escaping is
   * needed. */
  int n = snprintf(out_json, out_json_size,
                    "{\n"
                    "  \"content\": \"{\\\"data\\\":{\\\"%s\\\":{\\\"childorder\\\":[\\\"Level\\\",\\\"Drive"
                    "\\\",\\\"Tone\\\",\\\"Hi-Lo\\\"],\\\"children\\\":{\\\"Drive\\\":{\\\"type\\\":0,\\\"value\\\":"
                    "%d},\\\"Hi-Lo\\\":{\\\"state\\\":false,\\\"type\\\":1},\\\"Level\\\":{\\\"type\\\":0,\\\"value"
                    "\\\":%d},\\\"Tone\\\":{\\\"type\\\":0,\\\"value\\\":%d}}}},\\\"info\\\":{\\\"version\\\":\\\"1."
                    "0.9\\\"}}\",\n"
                    "  \"id\": \"%s\",\n"
                    "  \"readonly\": false,\n"
                    "  \"type\": \"%s\"\n"
                    "}\n",
                    data_key, drive, level, tone, uuid_str, type);
  return n > 0 && (size_t)n < out_json_size;
}

static bool rename_file(const char* from, const char* to, char* err, size_t err_size)
{
  if (rename(from, to) != 0)
  {
    set_err(err, err_size, "renaming %s to %s failed: %s", from, to, strerror(errno));
    return false;
  }
  return true;
}

/* Writes (or skips, per overwrite_blocks) one .block file per model into
 * `blocks_dir`/"ANXIETY OD"[ V2], creating `blocks_dir` itself and that
 * subfolder first if either doesn't exist yet -- same "do it ourselves"
 * approach as nam_library_ensure_dir already takes for /NAM. */
static bool write_blocks_for_type(const char* blocks_dir, const NamLibraryPlan* plan, bool v2, bool overwrite_blocks,
                                   int tone, int level, int* written_count, int* skipped_count, char* err,
                                   size_t err_size)
{
  if (!nam_library_ensure_dir(blocks_dir, err, err_size))
    return false;

  char type_dir[2048];
  snprintf(type_dir, sizeof(type_dir), "%s/%s", blocks_dir, v2 ? "ANXIETY OD V2" : "ANXIETY OD");
  if (!nam_library_ensure_dir(type_dir, err, err_size))
    return false;

  for (int i = 0; i < plan->count; ++i)
  {
    const NamLibraryItem* item = &plan->items[i];
    char title[NAM_LIBRARY_MAX_NAME];
    snprintf(title, sizeof(title), "%s", item->base_name);
    size_t title_len = strlen(title);
    if (title_len > 4 && has_nam_extension(title, title_len))
      title[title_len - 4] = '\0'; /* drop ".nam" -- the block gets its own ".block" extension */

    char block_filename[NAM_LIBRARY_MAX_NAME];
    snprintf(block_filename, sizeof(block_filename), "%03d - %s.block", item->index, title);
    char block_path[2048];
    snprintf(block_path, sizeof(block_path), "%s/%s", type_dir, block_filename);

    struct stat st;
    if (!overwrite_blocks && stat(block_path, &st) == 0)
    {
      (*skipped_count)++;
      continue;
    }

    char json[2048];
    if (!nam_library_make_block_json(item->index, tone, level, v2, json, sizeof(json), NULL))
    {
      set_err(err, err_size, "rendering .block JSON for %s failed (buffer too small)", item->new_filename);
      return false;
    }

    FILE* f = fopen(block_path, "wb");
    if (!f)
    {
      set_err(err, err_size, "opening %s for write failed: %s", block_path, strerror(errno));
      return false;
    }
    size_t json_len = strlen(json);
    bool wrote = fwrite(json, 1, json_len, f) == json_len;
    fclose(f);
    if (!wrote)
    {
      set_err(err, err_size, "writing %s failed: %s", block_path, strerror(errno));
      return false;
    }
    (*written_count)++;
  }
  return true;
}

bool nam_library_apply(const char* nam_dir, const char* blocks_dir, const NamLibraryPlan* plan, bool generate_blocks,
                        bool generate_v2_blocks, bool overwrite_blocks, int tone, int level,
                        NamLibraryApplyResult* result, char* err, size_t err_size)
{
  memset(result, 0, sizeof(*result));

  int count = plan->count;
  /* 0 = still at old_filename, 1 = moved to its temp name, 2 = moved to new_filename. */
  int* state = (int*)calloc(count ? (size_t)count : 1, sizeof(int));
  char(*tmp_names)[NAM_LIBRARY_MAX_NAME] = malloc((count ? (size_t)count : 1) * NAM_LIBRARY_MAX_NAME);
  if (!state || !tmp_names)
  {
    free(state);
    free(tmp_names);
    set_err(err, err_size, "out of memory preparing rename plan");
    return false;
  }
  for (int i = 0; i < count; ++i)
    snprintf(tmp_names[i], NAM_LIBRARY_MAX_NAME, "%03d.namlib_tmp", i);

  char path_a[2048], path_b[2048];

  /* Phase 1: old_filename -> temp name, for every item that needs renaming.
   * Temp names are extension-less and index-unique, so they can never
   * collide with a real *.nam file already in the plan. */
  bool ok = true;
  for (int i = 0; i < count && ok; ++i)
  {
    if (!plan->items[i].needs_rename)
      continue;
    snprintf(path_a, sizeof(path_a), "%s/%s", nam_dir, plan->items[i].old_filename);
    snprintf(path_b, sizeof(path_b), "%s/%s", nam_dir, tmp_names[i]);
    if (rename_file(path_a, path_b, err, err_size))
      state[i] = 1;
    else
      ok = false;
  }

  /* Phase 2: temp name -> new_filename. */
  for (int i = 0; i < count && ok; ++i)
  {
    if (state[i] != 1)
      continue;
    snprintf(path_a, sizeof(path_a), "%s/%s", nam_dir, tmp_names[i]);
    snprintf(path_b, sizeof(path_b), "%s/%s", nam_dir, plan->items[i].new_filename);
    if (rename_file(path_a, path_b, err, err_size))
      state[i] = 2;
    else
      ok = false;
  }

  if (!ok)
  {
    /* Best-effort rollback to old_filename, regardless of which phase
     * each item made it to. Secondary failures here are not reported --
     * `err` already carries the original failure that triggered this. */
    for (int i = 0; i < count; ++i)
    {
      if (state[i] == 2)
      {
        snprintf(path_a, sizeof(path_a), "%s/%s", nam_dir, plan->items[i].new_filename);
        snprintf(path_b, sizeof(path_b), "%s/%s", nam_dir, tmp_names[i]);
        if (rename(path_a, path_b) == 0)
          state[i] = 1;
      }
      if (state[i] == 1)
      {
        snprintf(path_a, sizeof(path_a), "%s/%s", nam_dir, tmp_names[i]);
        snprintf(path_b, sizeof(path_b), "%s/%s", nam_dir, plan->items[i].old_filename);
        if (rename(path_a, path_b) == 0)
          state[i] = 0;
      }
    }
    free(state);
    free(tmp_names);
    return false;
  }

  for (int i = 0; i < count; ++i)
    if (plan->items[i].needs_rename)
      result->renamed_count++;
  free(state);
  free(tmp_names);

  if (!generate_blocks)
    return true;

  if (!write_blocks_for_type(blocks_dir, plan, false, overwrite_blocks, tone, level, &result->blocks_written_count,
                              &result->blocks_skipped_existing_count, err, err_size))
    return false;

  if (generate_v2_blocks)
  {
    if (!write_blocks_for_type(blocks_dir, plan, true, overwrite_blocks, tone, level,
                                &result->v2_blocks_written_count, &result->v2_blocks_skipped_existing_count, err,
                                err_size))
      return false;
  }

  return true;
}
