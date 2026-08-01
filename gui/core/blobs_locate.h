/* blobs_locate.h -- find gui/blobs/{libnam_hook.so,libnam_preload.so,
 * trampoline_gonk.bin} at runtime, regardless of the process's current
 * working directory (a double-clicked GUI app cannot assume it was
 * launched from the repo root). */
#ifndef NAM_BLOBS_LOCATE_H
#define NAM_BLOBS_LOCATE_H

#include <stdbool.h>
#include <stddef.h>

/* Writes a directory path containing trampoline_gonk.bin into `out`.
 * Tries, in order: the directory the running executable lives in, its
 * parent (the gui/build layout during local development, where the
 * binary sits in gui/build/ and blobs live in gui/blobs/), then falls
 * back to the literal "gui/blobs" relative to the current working
 * directory (for running gui-core-cli by hand from the repo root).
 * Returns false with a message in `err` if none of those contain the
 * blobs. */
bool nam_locate_blobs_dir(char* out, size_t out_size, char* err, size_t err_size);

#endif
