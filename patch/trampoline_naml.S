@ Trampoline for the NEW additive "Neural Amp Modeler" engine's process()
@ vtable slot (our own cloned engine vtable, NOT Gonkulator's real one).
@
@ Same 8-arg ABI as trampoline_gonk.S (this, param2, float** input,
@ numChannels, float** output, numFrames, flags, ctx) -- our cloned engine
@ vtable slot sits at the identical relative offset (+0x9c) as Gonkulator's,
@ since it's a byte-for-byte copy of the same vtable shape.
@
@ Unlike trampoline_gonk.S, there is no "original" process() to fall back to
@ -- this is a brand-new engine, not a hijack. The fallback is a trivial
@ `bx lr` no-op (safe: never touches the output buffers, caller's existing
@ contents pass through whatever the caller already had there).

    .arm
    .globl trampoline_naml
trampoline_naml:
    ldr     ip, hook_slot_lit
    ldr     ip, [ip]
    cmp     ip, #0
    bxne    ip
    ldr     pc, orig_fn_lit        @ falls back to noop_ret_naml (no original exists)

hook_slot_lit:
    .word   0x00000000        @ patched: address of the mutable hook-pointer slot

orig_fn_lit:
    .word   0x00000000        @ patched: address of noop_ret_naml stub below

    .globl hook_slot_naml
hook_slot_naml:
    .word   0x00000000        @ mutable slot: nam_preload writes the real hook here

    .globl noop_ret_naml
noop_ret_naml:
    bx      lr                 @ safe no-op fallback
