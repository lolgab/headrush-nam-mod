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

/* `workdir` must already exist (the caller creates and later removes it,
 * e.g. via mkdtemp) -- used only for the rootfs ext4 image's temp files.
 * libnam_hook.so/libnam_preload.so/trampoline_gonk.bin are compiled
 * directly into the binary (see core/nam_blobs_embedded.h), not read from
 * disk. `model_name` may be NULL to auto-detect from the stock image's
 * `compatible` string. `enable_v2_hijack` selects the instance-count mode:
 * false = 2 instances (Anxiety OD v1 only, the original behavior), true =
 * up to 4 instances (also hijacks Anxiety OD V2's separate process()
 * vtable slot, see model_targets.h's v2_engine_vtable_vaddr/
 * v2_orig_process_fn).
 *
 * `additive_nam` (Pedalboard only) replaces the Anxiety OD hijack altogether: it adds two real "NAM A" / "NAM B"
 * pedals to the Distortion list (2 instances each, 4 in all), each with its own audio engine running NAM, see
 * spy_patch.h. enable_v2_hijack is ignored then.
 *
 * `enable_page_pedal` additionally installs the footswitch page-turner mod
 * (Pedalboard only): FS5/FS11 become USB-keyboard Page Down/Up, see
 * docs/PAGE_PEDAL.md and core/page_pedal.h.
 *
 * On success, *out_img_data is a malloc'd buffer (caller frees it)
 * containing the patched Update.img, and returns true. On failure, writes
 * a reason into err and returns false. */
bool nam_patch_pipeline(const uint8_t* stock_img_data, size_t stock_img_len, const char* model_name,
                         bool enable_v2_hijack, bool enable_page_pedal, bool additive_nam, const char* workdir,
                         NamProgressFn progress, void* progress_user_data, uint8_t** out_img_data,
                         size_t* out_img_len, char* err, size_t err_size);

#endif
