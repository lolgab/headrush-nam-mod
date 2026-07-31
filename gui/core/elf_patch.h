/* elf_patch.h -- C port of patch/patch_gonkulator.py: repoint Anxiety OD
 * engine object's process() vtable slot at an injected trampoline.
 *
 * See patch_gonkulator.py's own docstring for the full reverse-engineering
 * rationale -- this port carries none of that narrative, just the same
 * struct-level phdr edits and the same refuse-on-mismatch sanity guards
 * (wrong vtable slot value, wrong segment layout, non-zero "dead space").
 */
#ifndef NAM_GUI_ELF_PATCH_H
#define NAM_GUI_ELF_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint32_t hook_slot_addr; /* pass to the launcher script as NAM_HOOK_SLOT_GONK_ADDR */
  uint32_t trampoline_vaddr;
} ElfPatchResult;

/* Patches `data` (data_len bytes, a stock Evil binary loaded into memory --
 * modified IN PLACE, so pass a copy if the original must survive) to
 * repoint the AnxietyOD engine vtable's process() slot (engine_vtable +
 * PROCESS_SLOT*4) at a trampoline built from `tramp` (must be exactly 32
 * bytes -- see trampoline_gonk.S). `orig_fn` is both the value the slot
 * must already hold (sanity guard) and the trampoline's fallback target.
 *
 * On success, fills `result` and returns true. On any sanity-check failure
 * (wrong binary / wrong --model / already patched / unexpected layout),
 * writes a human-readable reason into err (up to err_size bytes) and
 * returns false -- same refuse-rather-than-guess behavior as the Python
 * original, never a partial/corrupt patch. */
bool nam_elf_patch_gonkulator(uint8_t* data, size_t data_len, const uint8_t tramp[32], uint32_t engine_vtable,
                               uint32_t orig_fn, ElfPatchResult* result, char* err, size_t err_size);

#endif
