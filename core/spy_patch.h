/* spy_patch.h -- debug-only observer on ModFac_construct (Pedalboard 2.7).
 *
 * Repoints slot 7 of the ModFac vtable (a data pointer, so no branch-range
 * limit) at a small trampoline that appends a 64-byte record
 * {r0, r1 (pedal type index), lr, sp, 12 stack words} to /tmp/modfac.bin via
 * raw syscalls, then tail-jumps to the real ModFac_construct. Behavior is
 * otherwise unchanged. Used to find who calls the factory and with which
 * type index, see docs/ADDITIVE_PEDAL.md.
 */
#ifndef NAM_GUI_SPY_PATCH_H
#define NAM_GUI_SPY_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Patches `data` (a stock Pedalboard 2.7 Evil binary) IN PLACE. Refuses,
 * with a reason in err, if the vtable slot or the code cave aren't exactly
 * what the analysis expects. Returns the trampoline vaddr via out_tramp. */
bool nam_elf_patch_modfac_spy(uint8_t* data, size_t data_len, uint32_t* out_tramp, char* err, size_t err_size);

#define NAM_MAX_PAIRS 3

typedef struct
{
  const char* name;       /* "<name>" and "<name> 2" become pedals; at most 12 characters */
  const char* short_name; /* short names, likewise */
  uint32_t type_idx;      /* existing ModFac type the pair is built as (91 = Glorious Drive) */
  uint32_t instances;     /* ids owned: 2 (default, "<n>", "<n> 2") or 4 ("<n>" .. "<n> 4", then it must be the only pedal) */
} NamPair;

/* Registers `npairs` extra "<name>" / "<name> 2" pedal pairs after the last normal pair (ids 167..166+2n; FX-Loop,
 * Input, Output and Mix move up by 2n) by relocating the boot-time pedal registry (type-index array, name and
 * short-name arrays, descriptor tables) and bumping every hard-coded id constant. Pedalboard 2.7 only; refuses on
 * any mismatch. Does not make the pedals usable on its own: call nam_elf_patch_node_registration afterwards. */
bool nam_elf_patch_registry(uint8_t* data, size_t data_len, const NamPair* pairs, uint32_t npairs, char* err,
                            size_t err_size);

/* One-pair convenience wrapper. */
bool nam_elf_patch_registry_test(uint8_t* data, size_t data_len, const char* name, const char* short_name,
                                 uint32_t type_idx, char* err, size_t err_size);

/* Registers the StateMap property nodes, links and value sources for the pairs added by nam_elf_patch_registry
 * (call it after that, with the same pairs). Without it the pedals crash on save, or show transparent with dead
 * knobs. */
bool nam_elf_patch_node_registration(uint8_t* data, size_t data_len, const NamPair* pairs, uint32_t npairs,
                                     uint32_t* out_init_slot, uint32_t* out_eng_tab, char* err, size_t err_size);
/* *out_init_slot: writable word hook C calls through once the nodes are active, as fn(triples, count, builder) (0 =
 * skipped); libnam_preload.so fills it. *out_eng_tab: {count, 8 x {engine, &name QString}} the engine stub fills
 * (pass it to nam_elf_patch_engine_class). Both may be NULL. */

/* Experimental: give the extra pedals their own copy of Glorious Drive's parameter-name table with Drive / Level / Tone
 * renamed to Model / OutVol / InVol (the editor's knob captions may follow). Call before
 * nam_elf_patch_node_registration; the property paths become /Engine/MixerCtrl/<pedal>/Model etc. */
void nam_spy_set_rename_params(bool on);

/* ModFac type the extra pedals must be built as to get their own audio engine (pass it as NamPair.type_idx). */
#define NAM_ENGINE_TYPE 80u

/* Gives type NAM_ENGINE_TYPE its own module+engine vtables (clones of Glorious Drive's) whose engine process() jumps
 * through a writable hook slot, returned in *out_hook_slot (libnam_preload.so fills it; it is 0 = original
 * process() until then). ids = number of extra ids the registry patch added. Call after nam_elf_patch_registry. */
bool nam_elf_patch_engine_class(uint8_t* data, size_t data_len, uint32_t ids, uint32_t eng_tab, uint32_t* out_hook_slot,
                                char* err, size_t err_size);

/* Debug: logs every call through the qt_static_metacall-style dispatcher at 0x86798 (method index, arguments) to
 * /tmp/meta.bin, to see what the block-type picker asks the UI model. Pedalboard 2.7 only.
 * `method_mask` bit i selects dispatcher method index i (all 49: 0x1ffffffffffff). */
bool nam_elf_patch_metacall_spy(uint8_t* data, size_t data_len, uint64_t method_mask, char* err, size_t err_size);

#endif
