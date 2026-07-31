/* ext4_image.h -- libext2fs-backed replacement for the `debugfs`/`e2fsck`
 * CLI calls build_update_img.py shells out to: dump a file out of the
 * rootfs, inject (replace-or-create) a file into it, and a basic
 * self-consistency check.
 *
 * Validated directly against real e2fsprogs tools before being wired into
 * the rest of gui/core: a synthetic ext4 image (modern mke2fs defaults --
 * extents, 64bit, metadata_csum, all more feature-rich than the actual
 * embedded device's rootfs is expected to be) round-tripped through
 * create -> read-back -> `e2fsck -fn` (clean) -> remove+recreate ->
 * `e2fsck -fn` (still clean) -> `debugfs cat` (correct content).
 *
 * nam_ext4_verify_basic() is NOT a full e2fsck replacement (e2fsck is a
 * separate multi-pass program, out of scope to reimplement) -- it's a
 * lighter self-consistency cross-check (every block reachable from an
 * in-use inode is marked used in the block bitmap, and vice versa) that
 * catches the class of corruption this wrapper's own inject/remove code
 * could plausibly introduce.
 */
#ifndef NAM_GUI_EXT4_IMAGE_H
#define NAM_GUI_EXT4_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ext4Image Ext4Image; /* opaque; wraps an ext2_filsys */

/* Opens `path` (a raw ext4 filesystem image, e.g. the decompressed rootfs
 * partition). read_write must be true to use nam_ext4_inject(). *out is
 * heap-allocated; free with nam_ext4_close(). */
bool nam_ext4_open(const char* path, bool read_write, Ext4Image** out, char* err, size_t err_size);

/* Flushes any pending writes (if opened read_write) and closes/frees img. */
void nam_ext4_close(Ext4Image* img);

/* Reads the whole content of the regular file at `inner_path` (e.g.
 * "/usr/Evil/Evil"). *out_data is malloc'd -- caller frees it. */
bool nam_ext4_dump(Ext4Image* img, const char* inner_path, uint8_t** out_data, size_t* out_len, char* err,
                    size_t err_size);

/* Replaces (or creates) the regular file at `inner_path` with `data` (len
 * bytes), mode `unix_mode` (permission bits only, e.g. 0755 -- the regular
 * file type bit is added internally). Matches build_update_img.py's
 * inject(): remove any existing file at that path first (fine if none
 * exists), then create fresh. Parent directory must already exist. */
bool nam_ext4_inject(Ext4Image* img, const char* inner_path, const uint8_t* data, size_t len, uint32_t unix_mode,
                      char* err, size_t err_size);

/* See this header's own comment on what this does and does not check. */
bool nam_ext4_verify_basic(Ext4Image* img, char* err, size_t err_size);

#endif
