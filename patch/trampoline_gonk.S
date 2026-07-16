@ Trampoline for Gonkulator engine object's process() (vtable slot @ file_offset
@ 0x1824914, engine vtable base 0x182c878, index 39, currently 0x2e7910).
@
@ Unlike the IRLoader trampoline, this call site passes REAL arguments in
@ r0-r3 (this, param2, float** input, uint32_t numChannels) plus stack args
@ (float** output, int32_t numFrames, uint32_t* flags, void* ctx) -- confirmed
@ via PyGhidra decompile of 0x2e7910. We must NOT clobber r0-r3, so scratch
@ work uses r12 (ip), a caller-saved/scratch register, instead of r1 (used by
@ the original IRLoader trampoline, which only ever had `this` in r0).
@
@ This is a REPLACE (either/or), same as the IRLoader hook: if hook_slot is
@ set, tail-jump there with all args intact (same signature); else fall back
@ to the original 0x2e7910. Register preservation works "for free" here
@ because this is a plain tail-branch (bx/ldr pc) -- it never touches
@ r0-r3 or the stack, so whichever function we jump to receives the exact
@ same arguments the caller passed in.

    .arm
    .globl trampoline_gonk
trampoline_gonk:
    ldr     ip, hook_slot_lit
    ldr     ip, [ip]
    cmp     ip, #0
    bxne    ip
    ldr     pc, orig_fn_lit

hook_slot_lit:
    .word   0x00000000        @ patched: address of the mutable hook-pointer slot

orig_fn_lit:
    .word   0x00000000        @ patched: original process() address (0x2e7910)

    .globl hook_slot_gonk
hook_slot_gonk:
    .word   0x00000000        @ mutable slot: nam_preload writes the real hook here
