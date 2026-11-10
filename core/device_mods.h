/* device_mods.h -- apply steps for the non-NAM mods (see mods.h). Each is
 * a C port of the standalone Python builder it replaces, keeping every
 * one of its refuse-rather-than-guess checks. Callers have already run
 * nam_mods_validate(), but each step still re-checks its own model, so a
 * mistake upstream fails loudly instead of patching the wrong firmware.
 */
#ifndef NAM_GUI_DEVICE_MODS_H
#define NAM_GUI_DEVICE_MODS_H

#include "ext4_image.h"
#include "patch_pipeline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Adds USB gadget g3 (two CDC-ACM serial functions), its systemd units,
 * and a teardown call at the top of the stock audio and mass-storage USB
 * mode scripts (the SoC has a single UDC). See mods/usb_console/. */
bool nam_mod_apply_usb_console(Ext4Image* img, const char* model_name, NamProgressFn progress, void* user_data,
                               char* err, size_t err_size);

/* Installs mx5_screen_server + an enabled unit that serves it on
 * /dev/ttyGS1. Requires the usb-console mod to have been applied to the
 * same image first. See mods/remote_screen/. */
bool nam_mod_apply_remote_screen(Ext4Image* img, const char* model_name, NamProgressFn progress, void* user_data,
                                 char* err, size_t err_size);

/* Patches the Evil binary in place (same length): replaces Tuner.qml's
 * source text and invalidates its compiled QML unit so Qt uses the edited
 * source. See mods/tuner_tempo/. */
bool nam_mod_apply_tuner_tempo(uint8_t* evil_data, size_t evil_len, const char* model_name, NamProgressFn progress,
                               void* user_data, char* err, size_t err_size);

#endif
