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
 * `blobs_dir` is the directory containing libnam_hook.so/libnam_preload.so/
 * trampoline_gonk.bin (see gui/blobs). `model_name` may be NULL to
 * auto-detect from the stock image's `compatible` string.
 *
 * On success, *out_img_data is a malloc'd buffer (caller frees it)
 * containing the patched Update.img, and returns true. On failure, writes
 * a reason into err and returns false. */
bool nam_patch_pipeline(const uint8_t* stock_img_data, size_t stock_img_len, const char* model_name,
                         const char* blobs_dir, const char* workdir, NamProgressFn progress, void* progress_user_data,
                         uint8_t** out_img_data, size_t* out_img_len, char* err, size_t err_size);

#endif
