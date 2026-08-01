/* nam_blobs_embedded.h -- gui/blobs/{libnam_hook.so,libnam_preload.so,
 * trampoline_gonk.bin} compiled directly into the binary as byte arrays
 * (see gui/tools/bin2c.c and the generated blob_*.c sources in
 * gui/CMakeLists.txt). No external files are read at runtime. */
#ifndef NAM_BLOBS_EMBEDDED_H
#define NAM_BLOBS_EMBEDDED_H

extern const unsigned char g_nam_hook_so[];
extern const unsigned long g_nam_hook_so_len;

extern const unsigned char g_nam_preload_so[];
extern const unsigned long g_nam_preload_so_len;

extern const unsigned char g_nam_trampoline_gonk_bin[];
extern const unsigned long g_nam_trampoline_gonk_bin_len;

#endif
