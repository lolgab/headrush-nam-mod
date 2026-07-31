/* http_download.h -- libcurl-backed HTTP(S) download to memory, replacing
 * the `curl` CLI shell-outs in scripts/quickstart_*.sh.
 */
#ifndef NAM_GUI_HTTP_DOWNLOAD_H
#define NAM_GUI_HTTP_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called periodically during the download with (downloaded_bytes,
 * total_bytes -- 0 if the server didn't send a Content-Length). Return
 * true to continue, false to abort the download. May be NULL. */
typedef bool (*NamDownloadProgressFn)(uint64_t downloaded_bytes, uint64_t total_bytes, void* user_data);

/* Call once before any nam_http_download() call (wraps curl_global_init) --
 * not thread-safe, do it on startup before spawning any worker thread. */
void nam_http_global_init(void);
void nam_http_global_cleanup(void);

/* Downloads `url` fully into memory. *out_data is malloc'd -- caller frees
 * it. Follows redirects. Returns false (with a reason in err) on any
 * transport error, non-2xx HTTP status, or if `progress` returned false. */
bool nam_http_download(const char* url, NamDownloadProgressFn progress, void* progress_user_data, uint8_t** out_data,
                        size_t* out_len, char* err, size_t err_size);

#endif
