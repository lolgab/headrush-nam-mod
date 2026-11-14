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

/* The footswitch notes the page-turner defaults to (0-based switch number: FS11 = 10 sends Page Down, FS5 = 4 sends
 * Page Up). Must match the defaults in patch/pagepedal.c; the installer also writes them to /usr/Evil/pagepedal.conf. */
#define NAM_PAGE_DOWN_NOTE_DEFAULT 10
#define NAM_PAGE_UP_NOTE_DEFAULT 4

/* Patches the stock M-Audio_Pass_Through_Assignments.qml (NUL-terminated text) so the two page-turner switches get a
 * fixed LED colour (Page Down blue, Page Up white) and a "PAGE DOWN" / "PAGE UP" label on their small screens. *out is
 * malloc'd, NUL-terminated. Idempotent. Refuses (false + reason) if an anchor isn't found exactly once. */
bool nam_page_pedal_patch_assignments(const char* qml_text, int page_down_note, int page_up_note, char** out, char* err,
                                      size_t err_size);

/* Installs everything into the (read-write) rootfs: /usr/Evil/pagepedal,
 * /usr/Evil/Scripts/pagepedal-usb.sh, the four patched stock scripts, the patched Assignments QML (labels and
 * colours) and /usr/Evil/pagepedal.conf with the two notes. The launcher-script block is added separately
 * (nam_launcher_add_page_pedal) because the pipeline owns that script. */
bool nam_page_pedal_install(Ext4Image* img, const uint8_t* daemon, size_t daemon_len, int page_down_note,
                            int page_up_note, char* err, size_t err_size);

#endif
