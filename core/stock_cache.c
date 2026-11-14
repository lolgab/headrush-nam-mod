/* stock_cache.c -- see stock_cache.h. */
#include "stock_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define NAM_PATH_SEP '\\'
#else
#include <unistd.h>
#define NAM_PATH_SEP '/'
#endif

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t* p)
{
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i)
  {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i)
  {
    uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
    uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

void nam_sha256_hex(const uint8_t* data, size_t len, char out_hex[65])
{
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= len; i += 64)
    sha256_block(h, data + i);
  uint8_t tail[128] = {0};
  size_t rem = len - i;
  if (rem)
    memcpy(tail, data + i, rem);
  tail[rem] = 0x80;
  size_t tail_len = rem + 1 + 8 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)len * 8u;
  for (int k = 0; k < 8; ++k)
    tail[tail_len - 1 - k] = (uint8_t)(bits >> (8 * k));
  sha256_block(h, tail);
  if (tail_len == 128)
    sha256_block(h, tail + 64);
  for (int k = 0; k < 8; ++k)
    snprintf(out_hex + 8 * k, 9, "%08x", h[k]);
}

/* ---- cache files ---- */

static bool cache_dir(char* out, size_t out_size)
{
  if (getenv("NAM_NO_CACHE"))
    return false;
  const char* base = NULL;
  char tmp[700];
#ifdef _WIN32
  base = getenv("LOCALAPPDATA");
  if (!base || !*base)
    return false;
  snprintf(tmp, sizeof(tmp), "%s\\headrush-nam-mod\\cache", base);
#elif defined(__APPLE__)
  base = getenv("HOME");
  if (!base || !*base)
    return false;
  snprintf(tmp, sizeof(tmp), "%s/Library/Caches/headrush-nam-mod", base);
#else
  const char* xdg = getenv("XDG_CACHE_HOME");
  if (xdg && *xdg)
    snprintf(tmp, sizeof(tmp), "%s/headrush-nam-mod", xdg);
  else
  {
    base = getenv("HOME");
    if (!base || !*base)
      return false;
    snprintf(tmp, sizeof(tmp), "%s/.cache/headrush-nam-mod", base);
  }
#endif
  if (strlen(tmp) >= out_size)
    return false;
  strcpy(out, tmp);
  return true;
}

static int make_one_dir(const char* path)
{
#ifdef _WIN32
  return _mkdir(path);
#else
  return mkdir(path, 0755);
#endif
}

/* mkdir -p */
static bool ensure_dir(const char* path)
{
  char buf[800];
  if (strlen(path) >= sizeof(buf))
    return false;
  strcpy(buf, path);
  for (char* p = buf + 1; *p; ++p)
  {
    if (*p == '/' || *p == '\\')
    {
      char c = *p;
      *p = '\0';
      if (make_one_dir(buf) != 0 && errno != EEXIST)
        return false;
      *p = c;
    }
  }
  if (make_one_dir(buf) != 0 && errno != EEXIST)
    return false;
  return true;
}

static bool entry_paths(const char* key, char* zip, size_t zip_size, char* meta, size_t meta_size)
{
  char dir[700];
  if (!cache_dir(dir, sizeof(dir)))
    return false;
  /* the key is built by us from the model name; keep it a plain file name anyway */
  for (const char* c = key; *c; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
      return false;
  int a = snprintf(zip, zip_size, "%s%c%s.zip", dir, NAM_PATH_SEP, key);
  int b = snprintf(meta, meta_size, "%s%c%s.sha256", dir, NAM_PATH_SEP, key);
  return a > 0 && b > 0 && (size_t)a < zip_size && (size_t)b < meta_size;
}

static uint8_t* read_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0)
  {
    fclose(f);
    return NULL;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
  {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static bool write_file(const char* path, const void* data, size_t len)
{
  /* temp name + rename, so an interrupted write never leaves a half file under the real name */
  char tmp[900];
  if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp))
    return false;
  FILE* f = fopen(tmp, "wb");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if (ok)
  {
#ifdef _WIN32
    remove(path); /* rename() does not replace on Windows */
#endif
    ok = rename(tmp, path) == 0;
  }
  if (!ok)
    remove(tmp);
  return ok;
}

void nam_stock_cache_drop(const char* key)
{
  char zip[900], meta[900];
  if (!entry_paths(key, zip, sizeof(zip), meta, sizeof(meta)))
    return;
  remove(zip);
  remove(meta);
}

bool nam_stock_cache_load(const char* key, const char* url, uint8_t** out_data, size_t* out_len, char out_hex[65])
{
  char zip[900], meta[900];
  if (!entry_paths(key, zip, sizeof(zip), meta, sizeof(meta)))
    return false;

  size_t meta_len = 0;
  uint8_t* meta_data = read_file(meta, &meta_len);
  if (!meta_data)
    return false; /* no entry (or no sidecar: the zip alone is not trusted) */
  char* text = (char*)realloc(meta_data, meta_len + 1);
  if (!text)
  {
    free(meta_data);
    return false;
  }
  text[meta_len] = '\0';

  char want_hex[65] = {0};
  char want_url[600] = {0};
  bool have_hash = false, have_url = false;
  for (char* line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n"))
  {
    if (strncmp(line, "sha256 ", 7) == 0 && strlen(line + 7) == 64)
    {
      memcpy(want_hex, line + 7, 64);
      have_hash = true;
    }
    else if (strncmp(line, "url ", 4) == 0 && strlen(line + 4) < sizeof(want_url))
    {
      strcpy(want_url, line + 4);
      have_url = true;
    }
  }
  free(text);

  /* a different URL means a different file: not damaged, just not ours to reuse (it gets overwritten on store) */
  if (!have_hash || !have_url || strcmp(want_url, url) != 0)
    return false;

  size_t len = 0;
  uint8_t* data = read_file(zip, &len);
  if (!data)
  {
    nam_stock_cache_drop(key);
    return false;
  }
  char got_hex[65];
  nam_sha256_hex(data, len, got_hex);
  if (strcmp(got_hex, want_hex) != 0)
  {
    free(data);
    nam_stock_cache_drop(key); /* truncated, corrupted or edited */
    return false;
  }
  *out_data = data;
  *out_len = len;
  memcpy(out_hex, got_hex, 65);
  return true;
}

bool nam_stock_cache_store(const char* key, const char* url, const uint8_t* data, size_t len, char out_hex[65],
                           char* err, size_t err_size)
{
  char dir[700], zip[900], meta[900];
  if (!entry_paths(key, zip, sizeof(zip), meta, sizeof(meta)) || !cache_dir(dir, sizeof(dir)))
  {
    if (err && err_size)
      snprintf(err, err_size, "no cache directory (disabled or HOME/LOCALAPPDATA not set)");
    return false;
  }
  nam_sha256_hex(data, len, out_hex);
  char text[800];
  snprintf(text, sizeof(text), "sha256 %s\nurl %s\n", out_hex, url);
  /* sidecar removed first: until the new pair is complete, the old zip must not be trusted */
  remove(meta);
  if (!ensure_dir(dir) || !write_file(zip, data, len) || !write_file(meta, text, strlen(text)))
  {
    if (err && err_size)
      snprintf(err, err_size, "writing the cache in %s failed: %s", dir, strerror(errno));
    nam_stock_cache_drop(key);
    return false;
  }
  return true;
}
