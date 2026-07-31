/* http_download.c -- see http_download.h. */
#include "http_download.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
