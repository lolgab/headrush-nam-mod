/* patch_pipeline.h -- the actual "apply the selected mods" pipeline, factored out
 * of tools/gui_core_cli.c so the GUI app can call the exact same code
 * instead of re-implementing or shelling out to the CLI. Operates on
 * in-memory buffers for the stock/output image (the caller owns file I/O
 * and, for the GUI, the download); a real temp directory is still needed
 * internally for the rootfs ext4 image (libext2fs needs a real file).
 */
#ifndef NAM_GUI_PATCH_PIPELINE_H
#define NAM_GUI_PATCH_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mods.h"

/* Called with a human-readable status line at each pipeline milestone
 * (same "OK ..." messages the CLI prints) -- the GUI appends these to its
 * log view, the CLI just printf's them. Never called with an error; on
 * failure the function returns false and fills `err` instead. */
typedef void (*NamProgressFn)(const char* message, void* user_data);

/* What to install. `mods` is the selection (see mods.h): on the Pedalboard
 * the default is the NAM pedals alone (two new pedals, "NAM" and "NAM TWO",
 * two instances each, appear in the Distortion list and nothing stock is
 * touched); the Anxiety OD hijack is the older design, opt-in there and the
 * default on models without NAM pedals. Any combination is allowed, including
 * no NAM at all, as long as each mod supports the model.
 *
 * The page-turner (NAM_MOD_PAGE_PEDAL) defaults to FS11 = Page Down and
 * FS5 = Page Up; set page_notes_set to use page_down_note / page_up_note
 * instead. */
typedef struct
{
  NamModSelection mods;
  bool page_notes_set; /* page-turner: use page_down_note / page_up_note instead of the defaults (FS11 / FS5) */
  int page_down_note;  /* 0-based footswitch numbers (FS number - 1) */
  int page_up_note;
} NamPatchOptions;

/* `workdir` must already exist (the caller creates and later removes it,
 * e.g. via mkdtemp) -- used only for the rootfs ext4 image's temp files.
 * Everything a mod installs (libnam_hook.so, libnam_preload.so,
 * trampoline_gonk.bin, the page-turner daemon, mod scripts/units/binaries)
 * is compiled directly into the binary (see core/nam_blobs_embedded.h and
 * core/mod_payloads_embedded.h), not read from disk. `model_name` may be
 * NULL to auto-detect from the stock image's `compatible` string.
 *
 * `opts->mods` is validated against the chosen model before anything is
 * touched: at least one mod selected, every selected mod supports the
 * model, and every selected mod's requirement is selected too. Without any
 * NAM mod, Evil and the NAM libraries are left stock.
 *
 * On success, *out_img_data is a malloc'd buffer (caller frees it)
 * containing the patched Update.img, and returns true. On failure, writes
 * a reason into err and returns false. */
bool nam_patch_pipeline(const uint8_t* stock_img_data, size_t stock_img_len, const char* model_name,
                         const NamPatchOptions* opts, const char* workdir, NamProgressFn progress,
                         void* progress_user_data, uint8_t** out_img_data, size_t* out_img_len, char* err,
                         size_t err_size);

#endif
