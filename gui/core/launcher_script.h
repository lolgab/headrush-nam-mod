/* launcher_script.h -- C port of build_update_img.py's
 * build_launcher_script(), the gonk_env-present ("hijack applied") branch
 * only -- that's the only branch build_update_img.py's main() ever
 * exercises, and the only one this GUI pipeline needs (it always patches a
 * real hijack, never produces a diagnostic-only/no-hook image).
 */
#ifndef NAM_GUI_LAUNCHER_SCRIPT_H
#define NAM_GUI_LAUNCHER_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

/* `stock_script_text` is the stock /usr/Evil/Scripts/evil launcher script,
 * NUL-terminated. `hook_slot_addr_hex` is the NAM_HOOK_SLOT_GONK_ADDR value
 * as a "0x..."-style string (see ElfPatchResult.hook_slot_addr). *out is
 * malloc'd, NUL-terminated -- caller frees it. Returns false (with a reason
 * in err) if the stock script's expected exec line isn't found exactly
 * once (refuse-on-mismatch, same as the Python original). */
bool nam_build_launcher_script(const char* stock_script_text, const char* hook_slot_addr_hex, char** out, char* err,
                                size_t err_size);

#endif
