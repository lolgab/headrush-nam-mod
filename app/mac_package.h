/* mac_package.h -- macOS-only: milestone 4. Repacks a fully-extracted
 * stock HeadRush Mac updater .app (see core/zip_reader's
 * nam_zip_extract_all) with a patched Update.img and ad-hoc codesigns it.
 * Not part of nam_gui_core (it shells out to `cp`/`codesign`, both always
 * present on macOS but not portable) -- app-layer glue only, used by
 * app/main.c.
 */
#ifndef NAM_GUI_MAC_PACKAGE_H
#define NAM_GUI_MAC_PACKAGE_H

#ifdef __APPLE__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Finds the *.app bundle's embedded Update.img (anywhere inside it,
 * case-insensitive). `app_dir` is the *.app directory itself (see
 * nam_mac_find_app_dir). Returns false if none is found. */
bool nam_mac_find_update_img(const char* app_dir, char* out_path, size_t out_size);

/* Finds the single top-level "*.app" directory inside `extracted_dir`
 * (the result of nam_zip_extract_all on the stock updater zip). */
bool nam_mac_find_app_dir(const char* extracted_dir, char* out_path, size_t out_size);

/* `extracted_dir` must contain exactly one top-level "*.app" directory
 * (the result of nam_zip_extract_all on the stock updater zip). Finds its
 * embedded Update.img via nam_mac_find_update_img, overwrites it with
 * `patched_img_data`, copies the whole bundle to `output_app_path`, and
 * ad-hoc codesigns it (`codesign --force --deep --sign -`) -- a codesign
 * failure is logged via `err` but does NOT fail the overall operation
 * (still returns true): an unsigned app is a normal, expected outcome for
 * any unofficial build here. */
bool nam_mac_package_app(const char* extracted_dir, const uint8_t* patched_img_data, size_t patched_img_len,
                          const char* output_app_path, char* err, size_t err_size);

#endif /* __APPLE__ */

#endif
