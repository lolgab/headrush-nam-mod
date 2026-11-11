/* launcher_script.h -- C port of the old Python pipeline's
 * build_launcher_script(), the gonk_env-present ("hijack applied") branch
 * only -- that's the only branch the old pipeline's main() ever exercised,
 * and the only one this GUI pipeline needs (it always patches a real
 * hijack, never produces a diagnostic-only/no-hook image).
 */
#ifndef NAM_GUI_LAUNCHER_SCRIPT_H
#define NAM_GUI_LAUNCHER_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

/* `stock_script_text` is the stock /usr/Evil/Scripts/evil launcher script,
 * NUL-terminated. `hook_slot_addr_hex` is the NAM_HOOK_SLOT_GONK_ADDR value
 * as a "0x..."-style string (see ElfPatchResult.hook_slot_addr).
 * `hook_slot_v2_addr_hex` is the same for the optional Anxiety OD V2 hijack
 * (NAM_HOOK_SLOT_GONK_V2_ADDR) -- pass NULL to skip it (2-instance mode).
 * `hook_slot_eng_addr_hex` is the hook slot of the additive NAM pedals' own engine (NAM_HOOK_SLOT_ENG_ADDR, see
 * spy_patch.h's nam_elf_patch_engine_class); NULL = none. `hook_slot_addr_hex` may be NULL too (additive-only
 * build: no Anxiety OD hijack).
 * `eng_init_slot_hex` / `eng_tab_hex` (NAM_HOOK_SLOT_ENG_INIT_ADDR / NAM_ENG_TAB_ADDR, from
 * nam_elf_patch_node_registration) are the display-init hook slot and the engine table; NULL = none.
 * *out is malloc'd, NUL-terminated -- caller frees it. Returns false (with a
 * reason in err) if the stock script's expected exec line isn't found
 * exactly once (refuse-on-mismatch, same as the Python original). */
bool nam_build_launcher_script(const char* stock_script_text, const char* hook_slot_addr_hex,
                                const char* hook_slot_v2_addr_hex, const char* hook_slot_eng_addr_hex,
                                const char* eng_init_slot_hex, const char* eng_tab_hex, char** out, char* err,
                                size_t err_size);

/* Adds the page-pedal block (starts /usr/Evil/Scripts/pagepedal-usb.sh boot
 * in the background) to an already-built launcher script, right before its
 * "while [ 1 ]" loop. Idempotent: an existing block is replaced. *out is
 * malloc'd, NUL-terminated -- caller frees it. */
bool nam_launcher_add_page_pedal(const char* script_text, char** out, char* err, size_t err_size);

#endif
