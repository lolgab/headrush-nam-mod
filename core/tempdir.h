/* tempdir.h -- portable "create/remove a unique scratch directory",
 * replacing the POSIX-only mkdtemp()/<unistd.h> usage that used to be
 * inlined in tools/gui_core_cli.c and app/main.c.
 *
 * This closes ONE of the two real gaps blocking a Windows build (see
 * docs/BUILDING.md) -- the other, core/ext4_image.c's dependency on
 * libext2fs (no Windows port exists via vcpkg or MSYS2/MinGW as of this
 * writing), is NOT addressed here and remains open.
 */
#ifndef NAM_GUI_TEMPDIR_H
#define NAM_GUI_TEMPDIR_H

#include <stdbool.h>
#include <stddef.h>

/* Creates a fresh, unique, empty directory suitable as a scratch work
 * directory, writing its path into `out_path` (must be at least
 * NAM_TEMPDIR_MAX_PATH bytes). Returns false (with a reason in err) on
 * failure. */
#define NAM_TEMPDIR_MAX_PATH 512
bool nam_make_temp_dir(char* out_path, size_t out_path_size, char* err, size_t err_size);

/* Recursively removes `path` (and everything under it). Best-effort --
 * failures are not reported, matching the existing `system("rm -rf ...")`
 * cleanup calls this replaces (temp-dir cleanup was already fire-and-
 * forget). */
void nam_remove_dir_recursive(const char* path);

#endif
