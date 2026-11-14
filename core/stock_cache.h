/* stock_cache.h -- keeps the official updater zip the GUI downloads, so a second run (another model, another
 * option set, another try) does not download ~60 MB again.
 *
 * A cache entry is two files in a per-user cache directory: "<key>.zip" and "<key>.sha256" (the SHA-256 of the zip and
 * the URL it came from). It is only reused if the zip hashes to the recorded value again and the URL is unchanged, so
 * a truncated, corrupted or edited file is never used: it is deleted and downloaded afresh. The hash is recorded on
 * first download (trust on first use); there is no list of known-good official hashes to check against.
 *
 * Directory: macOS ~/Library/Caches/headrush-nam-mod, Windows %LOCALAPPDATA%\headrush-nam-mod\cache, otherwise
 * $XDG_CACHE_HOME or ~/.cache/headrush-nam-mod. Set NAM_NO_CACHE=1 to bypass the cache entirely. */
#ifndef NAM_GUI_STOCK_CACHE_H
#define NAM_GUI_STOCK_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SHA-256 of data as 64 lowercase hex characters plus NUL. */
void nam_sha256_hex(const uint8_t* data, size_t len, char out_hex[65]);

/* Returns true and a malloc'd copy of the cached zip (caller frees) if an intact entry exists for `key`/`url`;
 * out_hex gets its SHA-256. A damaged entry is removed and false returned. */
bool nam_stock_cache_load(const char* key, const char* url, uint8_t** out_data, size_t* out_len, char out_hex[65]);

/* Stores `data` for `key`/`url`. Best effort: a failure (no cache directory, disk full) only costs the next run a
 * download, so it is reported in err but callers can ignore it. */
bool nam_stock_cache_store(const char* key, const char* url, const uint8_t* data, size_t len, char out_hex[65],
                           char* err, size_t err_size);

/* Removes the entry (e.g. when a cached zip turns out not to contain what the GUI expects). */
void nam_stock_cache_drop(const char* key);

#endif
