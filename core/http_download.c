/* http_download.c -- see http_download.h. */
#include "http_download.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

typedef struct
{
  uint8_t* data;
  size_t len;
  size_t cap;
  bool aborted;
} DownloadBuf;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  DownloadBuf* buf = (DownloadBuf*)userdata;
  size_t add = size * nmemb;
  if (buf->len + add > buf->cap)
  {
    size_t new_cap = buf->cap ? buf->cap * 2 : 65536;
    while (new_cap < buf->len + add)
      new_cap *= 2;
    uint8_t* n = (uint8_t*)realloc(buf->data, new_cap);
    if (!n)
      return 0; /* signals an error to libcurl */
    buf->data = n;
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->len, ptr, add);
  buf->len += add;
  return add;
}

typedef struct
{
  NamDownloadProgressFn fn;
  void* user_data;
  DownloadBuf* buf;
} ProgressCtx;

static int xferinfo_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
  (void)ultotal;
  (void)ulnow;
  ProgressCtx* ctx = (ProgressCtx*)clientp;
  if (!ctx->fn)
    return 0;
  bool cont = ctx->fn((uint64_t)dlnow, (uint64_t)(dltotal > 0 ? dltotal : 0), ctx->user_data);
  if (!cont)
  {
    ctx->buf->aborted = true;
    return 1; /* nonzero aborts the transfer */
  }
  return 0;
}

#ifdef _WIN32
/* MSYS2's mingw-w64 curl links OpenSSL, not schannel -- it needs a real CA
 * bundle *file* to verify HTTPS certs, and has no idea where one is on a
 * plain Windows box (its compiled-in default path lives inside the MSYS2
 * tree). Without this, every download fails with curl's verbatim
 * "Problem with the SSL CA cert (path? access rights?)" -- point it at the
 * ca-bundle.crt the release zip ships next to the exe instead. Static
 * buffer: this runs once, well before any thread that could race it. */
static const char* win_ca_bundle_path(void)
{
  static char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return NULL;
  char* slash = strrchr(path, '\\');
  if (!slash)
    return NULL;
  size_t dir_len = (size_t)(slash - path) + 1;
  if (dir_len + strlen("ca-bundle.crt") >= MAX_PATH)
    return NULL;
  strcpy(path + dir_len, "ca-bundle.crt");
  return path;
}
#endif

void nam_http_global_init(void)
{
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

void nam_http_global_cleanup(void)
{
  curl_global_cleanup();
}

bool nam_http_download(const char* url, NamDownloadProgressFn progress, void* progress_user_data, uint8_t** out_data,
                        size_t* out_len, char* err, size_t err_size)
{
  *out_data = NULL;
  *out_len = 0;

  CURL* curl = curl_easy_init();
  if (!curl)
  {
    set_err(err, err_size, "curl_easy_init failed");
    return false;
  }

  DownloadBuf buf = {0};
  ProgressCtx pctx = {progress, progress_user_data, &buf};

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "headrush-nam-mod-gui/1.0");
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
#ifdef _WIN32
  const char* ca_bundle = win_ca_bundle_path();
  if (ca_bundle)
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
#endif

  CURLcode res = curl_easy_perform(curl);

  if (res != CURLE_OK)
  {
    if (buf.aborted)
      set_err(err, err_size, "download cancelled");
    else
      set_err(err, err_size, "download failed: %s", curl_easy_strerror(res));
    free(buf.data);
    curl_easy_cleanup(curl);
    return false;
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (status != 0 && (status < 200 || status >= 300))
  {
    set_err(err, err_size, "download failed: HTTP %ld", status);
    free(buf.data);
    return false;
  }

  *out_data = buf.data;
  *out_len = buf.len;
  return true;
}
