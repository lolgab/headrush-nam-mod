/* sha1.c -- public domain SHA-1 (FIPS 180-1). See sha1.h. */
#include "sha1.h"

#include <string.h>

#define ROL32(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
           | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  for (int i = 16; i < 80; ++i)
    w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

  for (int i = 0; i < 80; ++i)
  {
    uint32_t f, k;
    if (i < 20)
    {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    }
    else if (i < 40)
    {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    }
    else if (i < 60)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    uint32_t temp = ROL32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = ROL32(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void sha1_init(SHA1_CTX* ctx)
{
  ctx->state[0] = 0x67452301u;
  ctx->state[1] = 0xEFCDAB89u;
  ctx->state[2] = 0x98BADCFEu;
  ctx->state[3] = 0x10325476u;
  ctx->state[4] = 0xC3D2E1F0u;
  ctx->bitlen = 0;
  ctx->buf_len = 0;
}

void sha1_update(SHA1_CTX* ctx, const void* data, size_t len)
{
  const uint8_t* p = (const uint8_t*)data;
  ctx->bitlen += (uint64_t)len * 8;

  while (len > 0)
  {
    size_t take = 64 - ctx->buf_len;
    if (take > len)
      take = len;
    memcpy(ctx->buf + ctx->buf_len, p, take);
    ctx->buf_len += take;
    p += take;
    len -= take;
    if (ctx->buf_len == 64)
    {
      sha1_transform(ctx->state, ctx->buf);
      ctx->buf_len = 0;
    }
  }
}

void sha1_final(SHA1_CTX* ctx, uint8_t digest[20])
{
  uint64_t bitlen = ctx->bitlen;

  uint8_t pad = 0x80;
  sha1_update(ctx, &pad, 1);
  uint8_t zero = 0x00;
  while (ctx->buf_len != 56)
    sha1_update(ctx, &zero, 1);

  uint8_t lenbytes[8];
  for (int i = 0; i < 8; ++i)
    lenbytes[i] = (uint8_t)(bitlen >> (56 - 8 * i));
  /* Bypass sha1_update's bitlen accounting for the length field itself. */
  memcpy(ctx->buf + ctx->buf_len, lenbytes, 8);
  sha1_transform(ctx->state, ctx->buf);

  for (int i = 0; i < 5; ++i)
  {
    digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

void sha1_buffer(const void* data, size_t len, uint8_t digest[20])
{
  SHA1_CTX ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, data, len);
  sha1_final(&ctx, digest);
}
