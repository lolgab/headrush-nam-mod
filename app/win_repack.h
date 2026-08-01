/* win_repack.h -- milestone 5: repack the stock HeadRush Windows Firmware
 * Updater .exe (a 7-Zip SFX installer) with a patched Update.img. Direct
 * port of the old Python pipeline's repack_windows_updater.py (PE stub +
 * `;!@Install@!...;!@InstallEnd@!` config block + a plain 7z archive
 * containing Background.png, Config.json, Update.img, FirmwareUpdater.exe,
 * libusb-1.0.dll).
 *
 * The plain-C LZMA SDK only has a 7z READER, not a writer (7z archive
 * CREATION is only implemented in 7-Zip's much larger C++ codebase) -- so
 * this shells out to a `7z`-compatible CLI for the actual archive
 * extract/create steps, same as the old Python pipeline did.
 * `sevenzip_path` should point at a bundled `7za`/`7zz` binary in the
 * shipped app (or "7z"/"7zz"/"7za" to search PATH) -- this is the one
 * deliberate exception to "no shelled-out tools" on this platform,
 * mirroring the codesign/cp exception already made for macOS packaging.
 */
#ifndef NAM_GUI_WIN_REPACK_H
#define NAM_GUI_WIN_REPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Zeroes the PE Security Directory entry (Authenticode signature pointer)
 * in a PE image's header, operating in place on `pe_data` (`pe_data_len`
 * bytes -- must cover at least the optional header's data directories).
 * See the old Python pipeline's strip_authenticode_directory() for why:
 * any edit to the file changes its length, invalidating the absolute
 * offset the stock signature is referenced by. */
bool nam_pe_strip_authenticode_directory(uint8_t* pe_data, size_t pe_data_len, char* err, size_t err_size);

/* Extracts the stock Update.img out of `stock_exe_path`'s embedded 7z
 * archive (needed as input to core/patch_pipeline before a repack can
 * happen at all). `sevenzip_path`/`workdir` as above. *out_data is
 * malloc'd -- caller frees it. */
bool nam_win_extract_stock_img(const char* stock_exe_path, const char* sevenzip_path, const char* workdir,
                                uint8_t** out_data, size_t* out_len, char* err, size_t err_size);

/* Full repack: reads `stock_exe_path`, swaps in `patched_img_data` for the
 * embedded Update.img, ad-hoc-strips the (now-invalid) Authenticode
 * signature pointer, and writes `output_exe_path` -- including the same
 * round-trip verification (7z integrity test + byte-compare every
 * unchanged entry) the old Python pipeline did. `sevenzip_path` is the
 * 7z-compatible CLI to shell out to (see above). `workdir` must already
 * exist (caller creates/removes it, e.g. via mkdtemp). */
bool nam_win_repack_updater(const char* stock_exe_path, const uint8_t* patched_img_data, size_t patched_img_len,
                            const char* output_exe_path, const char* sevenzip_path, const char* workdir, char* err,
                            size_t err_size);

#endif
