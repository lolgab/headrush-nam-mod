/* xz_codec.h -- liblzma-backed replacement for the `xz` CLI shell-outs in
 * build_update_img.py: `xz -d -k -T0 -c` (decompress) and
 * `xz -9 -T0 --check=crc32 -c` (compress).
 */
#ifndef NAM_GUI_XZ_CODEC_H
#define NAM_GUI_XZ_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* *out is malloc'd -- caller frees it. */
bool nam_xz_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len, char* err, size_t err_size);

/* preset 9, CRC32 check (matches `xz -9 --check=crc32`) -- single-threaded
 * (T0 in the CLI only matters for inputs large enough to block-split,
 * which a HeadRush rootfs isn't; see xz_codec.c). *out is malloc'd. */
bool nam_xz_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len, char* err, size_t err_size);

#endif
