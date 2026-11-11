/* page_pedal.h -- optional "footswitch page-turner" mod: installs an on-device
 * daemon + USB-gadget helper script that turn two footswitches into USB
 * keyboard Page Down / Page Up for the connected computer, and patches the
 * stock USB-mode scripts so the keyboard gadget coexists with audio-interface
 * and USB-drive modes. See docs/PAGE_PEDAL.md. */
#ifndef NAM_GUI_PAGE_PEDAL_H
#define NAM_GUI_PAGE_PEDAL_H

#include "ext4_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Patches `script_text` (a stock USB-mode script's full text, NUL-terminated)
 * by file name (basename: "usb-otg-audio-start.sh", "usb-otg-audio-stop.sh",
 * "setup-mass-storage.sh" or "remove-mass-storage.sh"). *out is malloc'd,
 * NUL-terminated. Idempotent (already-patched text is returned unchanged).
 * Refuses (false + reason) if an expected anchor isn't found exactly once --
 * stock script layout changed. Exposed separately so it can be unit-tested
 * against real scripts without an ext4 image. */
bool nam_page_pedal_patch_script(const char* basename, const char* script_text, char** out, char* err,
                                  size_t err_size);

/* Installs everything into the (read-write) rootfs: /usr/Evil/pagepedal,
 * /usr/Evil/Scripts/pagepedal-usb.sh and the four patched stock scripts. The
 * launcher-script block is added separately (nam_launcher_add_page_pedal)
 * because the pipeline owns that script. */
bool nam_page_pedal_install(Ext4Image* img, const uint8_t* daemon, size_t daemon_len, char* err, size_t err_size);

#endif
