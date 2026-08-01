/* xz_codec.c -- see xz_codec.h. */
#include "xz_codec.h"

#include <lzma.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void set_err(char* err, size_t err_size, const char* fmt, ...)
{
  if (!err || err_size == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

bool nam_xz_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len, char* err, size_t err_size)
{
  *out = NULL;
  *out_len = 0;

  /* liblzma's one-shot buffer decoder needs a big-enough output buffer up
   * front (it doesn't grow one itself) -- start at 4x input (xz on this
   * project's data -- ELF binaries, ext4 images -- routinely compresses
   * better than 4:1) and double on LZMA_BUF_ERROR until it fits. */
  size_t cap = in_len * 4 + 4096;
  for (int attempt = 0; attempt < 12; ++attempt)
  {
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf)
    {
      set_err(err, err_size, "out of memory (%zu bytes)", cap);
      return false;
    }
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0;
    size_t out_pos = 0;
    lzma_ret ret = lzma_stream_buffer_decode(&memlimit, 0, NULL, in, &in_pos, in_len, buf, &out_pos, cap);
    if (ret == LZMA_OK)
    {
      *out = buf;
      *out_len = out_pos;
      return true;
    }
    free(buf);
    if (ret != LZMA_BUF_ERROR)
    {
      set_err(err, err_size, "lzma_stream_buffer_decode failed (code %d)", (int)ret);
      return false;
    }
    cap *= 2;
  }
  set_err(err, err_size, "decompressed output kept exceeding retry buffer sizes -- giving up");
  return false;
}

bool nam_xz_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len, char* err, size_t err_size)
{
  *out = NULL;
  *out_len = 0;

  size_t cap = lzma_stream_buffer_bound(in_len);
  uint8_t* buf = (uint8_t*)malloc(cap);
  if (!buf)
  {
    set_err(err, err_size, "out of memory (%zu bytes)", cap);
    return false;
  }

  size_t out_pos = 0;
  lzma_ret ret = lzma_easy_buffer_encode(9, LZMA_CHECK_CRC32, NULL, in, in_len, buf, &out_pos, cap);
  if (ret != LZMA_OK)
  {
    free(buf);
    set_err(err, err_size, "lzma_easy_buffer_encode failed (code %d)", (int)ret);
    return false;
  }

  *out = buf;
  *out_len = out_pos;
  return true;
}
