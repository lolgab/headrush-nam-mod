/* zip_reader.h -- miniz-backed zip reading. Read-only (extraction) is all
 * this GUI's Mac-updater-zip download path needs -- no repack support.
 */
#ifndef NAM_GUI_ZIP_READER_H
#define NAM_GUI_ZIP_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Finds the first entry in the zip (`zip_data`, `zip_len` bytes) whose
 * path ends with `name_suffix` (e.g. "Update.img") and extracts it fully
 * into memory. *out_data is malloc'd -- caller frees it. Returns false
 * (with a reason in err) if the buffer isn't a valid zip or no matching
 * entry is found. */
bool nam_zip_extract_by_suffix(const uint8_t* zip_data, size_t zip_len, const char* name_suffix, uint8_t** out_data,
                                size_t* out_len, char* err, size_t err_size);

/* Extracts every entry in the zip into `dest_dir` (created if missing,
 * along with any intermediate directories each entry needs -- an `mkdir
 * -p` equivalent), preserving directory structure, Unix permission bits
 * (needed for a .app bundle's embedded executables to stay runnable), and
 * symlinks (macOS .app bundles routinely use them, e.g. Framework.
 * framework/Versions/Current) when the archive records Unix metadata
 * (i.e. it was built on a Unix host -- true for zips from `ditto`/`zip`,
 * matching the stock HeadRush Mac updater's own download). Matches
 * `ditto -x -k`'s behavior for this project's purposes. */
bool nam_zip_extract_all(const uint8_t* zip_data, size_t zip_len, const char* dest_dir, char* err, size_t err_size);

#endif
