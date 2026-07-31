/* fit_image.h -- minimal FDT/FIT (U-Boot Flattened Image Tree) reader AND
 * writer, C port + extension of scripts/fit_image.py.
 *
 * scripts/fit_image.py only reads (build_update_img.py hands the .its text
 * to the real `mkimage` binary to write). This port additionally WRITES a
 * spec-compliant FIT blob directly -- see fit_image.c's header comment for
 * why exact byte-for-byte parity with a given `mkimage` build isn't the
 * goal (its trailing string-table padding is an internal implementation
 * quirk, not part of the format); validity and semantic equivalence
 * (same partitions, same computed hashes, same root metadata) is.
 *
 * Only implements what a HeadRush Update.img actually uses: a fixed
 * "/images/{splash,recoverysplash,rootfs}" shape, each with an xz-compressed
 * blob and a sha1 hash. No phandles/aliases/generic FIT features.
 */
#ifndef NAM_GUI_FIT_IMAGE_H
#define NAM_GUI_FIT_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIT_MAX_PATH 128
#define FIT_MAX_PROP_NAME 32
#define FIT_MAX_DEVICES 8

typedef struct
{
  char path[FIT_MAX_PATH];
  char name[FIT_MAX_PROP_NAME];
  uint32_t data_off; /* offset into the ORIGINAL data buffer passed to nam_fit_parse */
  uint32_t data_len;
} FitProp;

typedef struct
{
  FitProp* props;
  int count;
  int capacity;
} FitProps;

typedef struct
{
  char description[256];
  char compatible[64];
  uint32_t devices[FIT_MAX_DEVICES];
  int device_count;
} FitMetadata;

/* Parses `data`'s FDT structure into `out` (caller must nam_fit_props_free
 * it). Prop entries reference offsets/lengths into `data` itself -- `data`
 * must outlive any nam_fit_get_* call using this FitProps. Returns false on
 * a malformed/unrecognized blob (bad magic, unknown token, etc). */
bool nam_fit_parse(const uint8_t* data, size_t len, FitProps* out);
void nam_fit_props_free(FitProps* props);

bool nam_fit_get_bytes(const FitProps* props, const char* path, const char* name, uint32_t* out_off,
                        uint32_t* out_len);
/* Copies a NUL-terminated string into out_buf (size out_buf_size), trailing
 * NUL(s) in the source stripped, like read_prop_str()'s rstrip(b"\x00"). */
bool nam_fit_get_str(const uint8_t* data, const FitProps* props, const char* path, const char* name, char* out_buf,
                      size_t out_buf_size);
bool nam_fit_get_cells(const uint8_t* data, const FitProps* props, const char* path, const char* name,
                        uint32_t* out_cells, int max_cells, int* out_count);

/* Port of read_root_metadata(): description/compatible/inmusic,devices from
 * the FIT's root node. Returns false if any of the three is missing. */
bool nam_fit_read_root_metadata(const uint8_t* data, const FitProps* props, FitMetadata* out);

/* Builds a fresh, valid FIT blob with the fixed splash/recoverysplash/rootfs
 * shape (each image's sha1 hash is computed here, matching what a real
 * `mkimage` would compute over the same bytes). *out_data is malloc'd --
 * caller frees it. Returns false only on allocation failure. */
bool nam_fit_build(const FitMetadata* metadata, const uint8_t* splash_xz, size_t splash_len,
                    const uint8_t* recoverysplash_xz, size_t recoverysplash_len, const uint8_t* rootfs_xz,
                    size_t rootfs_len, uint8_t** out_data, size_t* out_len);

#endif
