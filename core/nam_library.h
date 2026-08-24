/* nam_library.h -- "Prepare NAM Library" feature: scans a device's /NAM
 * folder, assigns each *.nam file a knob position, and can generate one
 * Anxiety OD .block preset per model so each is directly browsable from
 * the HeadRush's own block/preset picker.
 *
 * Ground truth for what a knob position actually IS comes from
 * patch/nam_hook.cpp (preload_models_in_background + knob_value_to_index):
 * the firmware alphabetically sorts every *.nam file it finds and uses
 * each file's RANK in that sort (0-based) as its Model-knob position --
 * g_cached_models[N] for knob step N. The conventional "NNN - " filename
 * prefix documented in the README is a pure human mnemonic; the firmware
 * never parses it. It only matches reality when the whole folder's
 * prefixes are contiguous from 000 with no gaps or duplicates, which is
 * why this module always renumbers to close gaps rather than preserving
 * them -- a preserved gap would make a generated .block's DRIVE value
 * silently wrong. */
#ifndef NAM_GUI_NAM_LIBRARY_H
#define NAM_GUI_NAM_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

/* Mirrors patch/nam_hook.cpp's kKnobSteps: a fixed 101-position knob
 * (0%-100%), so at most 101 models can ever be reachable. */
#define NAM_LIBRARY_MAX_MODELS 101
#define NAM_LIBRARY_MAX_NAME 256
#define NAM_LIBRARY_MAX_PATH 900
#define NAM_LIBRARY_MAX_DRIVE_CANDIDATES 16

typedef struct
{
  char old_filename[NAM_LIBRARY_MAX_NAME]; /* current on-disk name */
  char new_filename[NAM_LIBRARY_MAX_NAME]; /* == old_filename if no rename needed */
  char base_name[NAM_LIBRARY_MAX_NAME];    /* old_filename with any leading "NNN - " stripped */
  int index;                               /* 0..100 -- the real Model-knob/DRIVE position this file will occupy */
  bool had_prefix;                         /* true if old_filename already had a well-formed "NNN - " prefix */
  bool needs_rename;                       /* new_filename != old_filename */
} NamLibraryItem;

typedef struct
{
  NamLibraryItem items[NAM_LIBRARY_MAX_MODELS];
  int count;
  int rename_count;          /* how many items need renaming */
  int duplicate_prefix_groups; /* how many distinct prefix values were shared by >1 file pre-scan (0 = none) */
} NamLibraryPlan;

typedef struct
{
  int renamed_count;
  int blocks_written_count;
  int blocks_skipped_existing_count;
  int v2_blocks_written_count;
  int v2_blocks_skipped_existing_count;
} NamLibraryApplyResult;

typedef struct
{
  char nam_dir[NAM_LIBRARY_MAX_PATH];    /* e.g. "/Volumes/HEADRUSH/NAM" */
  char blocks_dir[NAM_LIBRARY_MAX_PATH]; /* sibling "Blocks" folder, same parent as nam_dir */
  bool nam_dir_exists;                   /* false = the drive is a HeadRush drive but has no /NAM folder yet */
  bool blocks_dir_exists;                /* false = HeadRush hasn't created it yet (unusual -- see README) */
} NamLibraryDriveCandidate;

/* Finds mounted-volume candidates for the HeadRush's USB drive by probing
 * the OS's normal removable-media mount locations:
 *   macOS:   every subfolder of /Volumes
 *   Windows: every logical drive letter
 *   other:   every subfolder of /media, /media/$USER, /run/media/$USER, /mnt
 * A volume matches if it already has a folder literally named "NAM" at its
 * root, OR if it looks like a HeadRush drive without one yet (has one of
 * the device's other default folders -- "Blocks", "Impulse Responses", or
 * "Rigs" -- which the README documents as already present on a stock
 * device, unlike /NAM which the user normally has to create by hand).
 * That second case lets the caller create /NAM itself (see
 * nam_library_ensure_dir) instead of asking the user to -- see
 * NamLibraryDriveCandidate::nam_dir_exists. No user path entry involved.
 * An unreadable mount root (permissions, a volume that unmounts mid-scan)
 * is silently skipped -- this is a speculative probe over many paths, not
 * a single trusted lookup, so that's not an error. Returns the number of
 * candidates written to `out` (0 if no plugged-in/mounted drive looks
 * like a HeadRush drive at all), capped at max_out. */
int nam_library_find_drives(NamLibraryDriveCandidate* out, int max_out);

/* Creates `dir` if it doesn't already exist (its parent is assumed to
 * exist already -- true for every caller here, since `dir` is always a
 * single "NAM" leaf under an already-confirmed-mounted volume root). No-op
 * success if it's already there. Meant for creating a detected HeadRush
 * drive's missing /NAM folder on the caller's behalf
 * (NamLibraryDriveCandidate::nam_dir_exists == false) instead of making
 * the user do it manually via the File Manager/USB transfer view. */
bool nam_library_ensure_dir(const char* dir, char* err, size_t err_size);

/* Scans `dir` for *.nam files (case-insensitive extension), skipping
 * macOS AppleDouble "._*" sidecars -- mirrors nam_hook.cpp's own filter
 * exactly, so the plan matches what the firmware will actually see.
 * Read-only: nothing is renamed or written. Returns false with `err` set
 * if the directory can't be read, or if more than NAM_LIBRARY_MAX_MODELS
 * files are found -- `err` says exactly how many to remove, and no plan
 * is produced in that case (matches the "stop before changing anything"
 * requirement). */
bool nam_library_scan(const char* dir, NamLibraryPlan* out_plan, char* err, size_t err_size);

/* Renders the .block JSON for a model at `drive` (0-100), with TONE/LEVEL
 * set to `tone`/`level` (0-100 each) and a freshly-generated random UUID
 * as its "id". If v2, renders it as an "ANXIETY OD V2" block (the
 * separately-instantiable second hijacked pedal used by the "up to 4
 * instances" mod option -- see core/model_targets.h's v2_engine_vtable_vaddr)
 * instead of the default v1 "ANXIETY OD"; same field shape either way,
 * just a different pedal type and content data key, by direct analogy
 * with patch/patch_pedal_title.py's confirmed display-name table ("Anxiety
 * OD" / "Anxiety OD V2") -- not independently verified against a real
 * V2 .block file, since none exists anywhere to check against. Writes the
 * UUID string (36 chars + NUL) to uuid_out if non-NULL. Returns false if
 * out_json_size is too small. */
bool nam_library_make_block_json(int drive, int tone, int level, bool v2, char* out_json, size_t out_json_size,
                                  char* uuid_out /* [37] */);

/* Applies a plan already produced by nam_library_scan: renames every item
 * with needs_rename via a collision-safe two-phase rename (every renamed
 * file first moves to a private temp name, then all temp names move to
 * their final names), so a same-directory rename cycle can never clobber
 * a file that hasn't moved yet. On any failure, already-moved files are
 * best-effort restored to their original old_filename before returning
 * false -- nothing is left part-renamed on the happy path, though a
 * failure mid-rollback (e.g. disk pulled mid-operation) can still leave
 * some files at a temp name, surfaced in `err`.
 *
 * If generate_blocks, additionally writes one "NNN - base_name.block" per
 * model into `blocks_dir`/"ANXIETY OD" (creating that subfolder first if
 * it doesn't exist yet -- same "do it ourselves" approach as
 * nam_library_ensure_dir for /NAM). If generate_v2_blocks is also true,
 * does the same again into `blocks_dir`/"ANXIETY OD V2" -- pass this only
 * when the "up to 4 instances" mod option was used, since v2 blocks are
 * meaningless (Anxiety OD V2 stays a normal overdrive) otherwise. An
 * existing .block file is left untouched (counted in
 * result->blocks_skipped_existing_count / v2_blocks_skipped_existing_count)
 * unless overwrite_blocks is true. Block writes are independent per-file
 * and never trigger a rollback of the renames above. */
bool nam_library_apply(const char* nam_dir, const char* blocks_dir, const NamLibraryPlan* plan, bool generate_blocks,
                        bool generate_v2_blocks, bool overwrite_blocks, int tone, int level,
                        NamLibraryApplyResult* result, char* err, size_t err_size);

/* Manual reorder support for the GUI preview screen: swaps items at
 * positions i and j and recomputes both their index/new_filename/
 * needs_rename fields for their new positions. No-op if i == j or either
 * index is out of range. Does not touch disk -- only the in-memory plan,
 * same as nam_library_scan itself; call nam_library_apply afterward to
 * act on the reordered plan. */
void nam_library_swap_items(NamLibraryPlan* plan, int i, int j);

#endif
