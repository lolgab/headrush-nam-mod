@ Input/Output trim knob setters for the new "Neural Amp Modeler" engine.
@
@ Confirmed ABI for this engine class's parameter setters (SESSION_NOTES.md
@ BREAKTHROUGH #4): void setParamX(EngineObj* this, float value), this in r0,
@ value in VFP register s0. Same replace-or-noop trampoline shape as the
@ process() hook, but simpler -- only r0/s0 need to pass through untouched,
@ no other live arguments.
@
@ These repoint OUR OWN cloned engine vtable's slots 14 and 16 (originally
@ the shared generic byte/flag setters 0x1a3cf4/0x1a3d28 -- safe to repurpose
@ since this is our own vtable copy, not Gonkulator's real one; no real
@ Gonkulator instance is ever affected). Deliberately does NOT write into the
@ engine object's own memory at all (its internal layout beyond the known
@ +0x2ac model-select field isn't fully mapped) -- instead calls into
@ nam_hook.cpp's own storage, exactly like the process() hook already does.

    .arm
    .globl trampoline_trim_in
trampoline_trim_in:
    ldr     ip, hook_slot_in_lit
    ldr     ip, [ip]
    cmp     ip, #0
    bxne    ip
    bx      lr                     @ no-op fallback: leaves r0/s0 untouched

hook_slot_in_lit:
    .word   0x00000000

    .globl hook_slot_naml_trim_in
hook_slot_naml_trim_in:
    .word   0x00000000

    .globl trampoline_trim_out
trampoline_trim_out:
    ldr     ip, hook_slot_out_lit
    ldr     ip, [ip]
    cmp     ip, #0
    bxne    ip
    bx      lr

hook_slot_out_lit:
    .word   0x00000000

    .globl hook_slot_naml_trim_out
hook_slot_naml_trim_out:
    .word   0x00000000
