/* nam_library_cli.c -- standalone CLI for the "Prepare NAM Library"
 * feature (core/nam_library.c): scan a device's /NAM folder, preview the
 * renumbering plan, and optionally apply it (renames + Anxiety OD .block
 * generation). Thin wrapper, same pattern as tools/gui_core_cli.c -- the
 * GUI's "Prepare NAM Library" screen calls the exact same core functions.
 *
 * usage: nam-library-cli --find-drives
 *        nam-library-cli <nam_dir> [--apply] [--blocks <blocks_dir>] [--v2-blocks]
 *                         [--overwrite-blocks] [--tone N] [--level N]
 */
#include "nam_library.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

int main(int argc, char** argv)
{
  if (argc < 2)
    die("usage: %s --find-drives | <nam_dir> [--apply] [--blocks <blocks_dir>] [--v2-blocks] [--overwrite-blocks] "
        "[--tone N] [--level N]",
        argv[0]);

  if (strcmp(argv[1], "--find-drives") == 0)
  {
    NamLibraryDriveCandidate candidates[NAM_LIBRARY_MAX_DRIVE_CANDIDATES];
    int count = nam_library_find_drives(candidates, NAM_LIBRARY_MAX_DRIVE_CANDIDATES);
    if (count == 0)
    {
      printf("no mounted drive with a NAM folder found\n");
      return 0;
    }
    for (int i = 0; i < count; ++i)
      printf("%s%s  (Blocks: %s%s)\n", candidates[i].nam_dir, candidates[i].nam_dir_exists ? "" : " (not created yet)",
             candidates[i].blocks_dir, candidates[i].blocks_dir_exists ? "" : ", missing");
    return 0;
  }

  const char* nam_dir = argv[1];
  bool apply = false;
  bool generate_blocks = false;
  bool generate_v2_blocks = false;
  bool overwrite_blocks = false;
  const char* blocks_dir = NULL;
  int tone = 50, level = 50;

  for (int i = 2; i < argc; ++i)
  {
    if (strcmp(argv[i], "--apply") == 0)
      apply = true;
    else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc)
    {
      blocks_dir = argv[++i];
      generate_blocks = true;
    }
    else if (strcmp(argv[i], "--v2-blocks") == 0)
      generate_v2_blocks = true;
    else if (strcmp(argv[i], "--overwrite-blocks") == 0)
      overwrite_blocks = true;
    else if (strcmp(argv[i], "--tone") == 0 && i + 1 < argc)
      tone = atoi(argv[++i]);
    else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc)
      level = atoi(argv[++i]);
    else
      die("unknown argument: %s", argv[i]);
  }

  NamLibraryPlan plan;
  char err[1024];
  if (!nam_library_scan(nam_dir, &plan, err, sizeof(err)))
    die("%s", err);

  printf("%d NAM model(s) found, %d need renaming", plan.count, plan.rename_count);
  if (plan.duplicate_prefix_groups > 0)
    printf(" (%d duplicate index group%s in the existing library)", plan.duplicate_prefix_groups,
           plan.duplicate_prefix_groups == 1 ? "" : "s");
  printf("\n\n");

  for (int i = 0; i < plan.count; ++i)
  {
    const NamLibraryItem* item = &plan.items[i];
    if (item->needs_rename)
      printf("  %s\n    -> %s\n", item->old_filename, item->new_filename);
    else
      printf("  %s  (unchanged)\n", item->old_filename);
  }

  if (!apply)
  {
    printf("\n(dry run -- pass --apply to rename these files%s)\n",
           generate_blocks ? " and generate .block files" : "");
    return 0;
  }

  NamLibraryApplyResult result;
  if (!nam_library_apply(nam_dir, blocks_dir, &plan, generate_blocks, generate_v2_blocks, overwrite_blocks, tone,
                          level, &result, err, sizeof(err)))
    die("%s", err);

  printf("\nrenamed %d file(s)\n", result.renamed_count);
  if (generate_blocks)
    printf("wrote %d ANXIETY OD .block file(s), skipped %d already-existing\n", result.blocks_written_count,
           result.blocks_skipped_existing_count);
  if (generate_v2_blocks)
    printf("wrote %d ANXIETY OD V2 .block file(s), skipped %d already-existing\n", result.v2_blocks_written_count,
           result.v2_blocks_skipped_existing_count);
  return 0;
}
