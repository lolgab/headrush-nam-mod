/* patch_pipeline.h -- the actual "apply the NAM mod" pipeline, factored out
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

/* Called with a human-readable status line at each pipeline milestone
 * (same "OK ..." messages the CLI prints) -- the GUI appends these to its
 * log view, the CLI just printf's them. Never called with an error; on
 * failure the function returns false and fills `err` instead. */
typedef void (*NamProgressFn)(const char* message, void* user_data);

/* What to install. On the Pedalboard the default is `nam_pedals` alone: two new pedals, "NAM" and "NAM TWO" (two instances each, four per rig), appear in
 * the Distortion list and nothing stock is touched. `anxiety_hijack` is the older design, kept opt-in for people whose
 * existing pedalboards use Anxiety OD as the NAM engine: Anxiety OD (v1) itself runs NAM, and with `anxiety_v2` the
 * separate Anxiety OD V2 does too (up to 4 hijacked instances in all). Both can be on together. The other models
 * (MX5, Gigboard) have no additive support yet, so for them `anxiety_hijack` is the only option and must be set.
 * `page_pedal` additionally installs the footswitch page-turner mod (Pedalboard only): FS5/FS11 become USB-keyboard
 * Page Down/Up, see docs/PAGE_PEDAL.md and core/page_pedal.h. */
typedef struct
{
  bool nam_pedals;     /* add "NAM" and "NAM TWO" (Pedalboard only), see spy_patch.h and docs/ADDITIVE_PEDAL.md */
  bool anxiety_hijack; /* legacy: Anxiety OD (v1) process() runs NAM */
  bool anxiety_v2;     /* with anxiety_hijack: also hijack Anxiety OD V2's separate process() slot */
  bool page_pedal;
  bool page_notes_set; /* page-turner: use page_down_note / page_up_note instead of the defaults (FS11 / FS5) */
  int page_down_note;  /* 0-based footswitch numbers (FS number - 1) */
  int page_up_note;
} NamPatchOptions;

/* `workdir` must already exist (the caller creates and later removes it,
 * e.g. via mkdtemp) -- used only for the rootfs ext4 image's temp files.
 * libnam_hook.so/libnam_preload.so/trampoline_gonk.bin are compiled
 * directly into the binary (see core/nam_blobs_embedded.h), not read from
 * disk. `model_name` may be NULL to auto-detect from the stock image's
 * `compatible` string. `opts` selects what is installed (see NamPatchOptions); at least one of nam_pedals /
 * anxiety_hijack must be set.
 *
 * On success, *out_img_data is a malloc'd buffer (caller frees it)
 * containing the patched Update.img, and returns true. On failure, writes
 * a reason into err and returns false. */
bool nam_patch_pipeline(const uint8_t* stock_img_data, size_t stock_img_len, const char* model_name,
                         const NamPatchOptions* opts, const char* workdir, NamProgressFn progress,
                         void* progress_user_data, uint8_t** out_img_data, size_t* out_img_len, char* err,
                         size_t err_size);

#endif
