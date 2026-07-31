/* sha1.h -- public domain SHA-1 (FIPS 180-1), single 20-byte digest API.
 * Only used to reproduce the per-image hash "value" property a real
 * `mkimage` computes when packing a FIT image (see gui/core/fit_image.c) --
 * not used for anything security-sensitive.
 */
#ifndef NAM_GUI_SHA1_H
#define NAM_GUI_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint32_t state[5];
  uint64_t bitlen;
  uint8_t buf[64];
  size_t buf_len;
} SHA1_CTX;

void sha1_init(SHA1_CTX* ctx);
void sha1_update(SHA1_CTX* ctx, const void* data, size_t len);
void sha1_final(SHA1_CTX* ctx, uint8_t digest[20]);

/* Convenience one-shot. */
void sha1_buffer(const void* data, size_t len, uint8_t digest[20]);

#endif
