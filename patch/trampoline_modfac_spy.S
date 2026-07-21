@ Spy trampoline for ModFac_construct's dispatch site (0x209be4, the single
@ `cmp r1, #0x5b` instruction patch_namloader.py documents as the pedal-type
@ jump-table comparison; r1 holds the type index being constructed). Pure
@ OBSERVER, never changes behavior: logs r1 via a hook_slot-resolved C
@ function (if installed), then ALWAYS re-executes the original overwritten
@ `cmp r1, #0x5b` and resumes the real, untouched dispatch at 0x209be8 --
@ cases 0-91 and the out-of-range default path are bit-for-bit unaffected,
@ exactly as if this were never patched. Built to answer, without any UART/
@ debugger, which type index the UI's pedal-add menu / a saved preset
@ actually sends for each pedal type -- the thing that was blocking the
@ additive (non-hijacking) NAM pedal design from ever becoming reachable.
@
@ r0-r3 and lr are saved/restored around the hook call so the original
@ dispatch sees r1 completely unchanged afterward; r4-r11 are never touched
@ at all (compiler-allocated locals spanning this dispatch point, if any,
@ are safe). Flags (CPSR) are freshly (re)set by the final `cmp r1, #0x5b`
@ immediately before branching back, so the original `ldrls` at 0x209be8
@ behaves identically regardless of what our own code did to flags in
@ between.

    .arm
    .globl trampoline_modfac_spy
trampoline_modfac_spy:
    push    {r0-r3, lr}
    mov     r0, r1                  @ arg0 = type index being constructed
    ldr     r1, spy_hook_slot_lit
    ldr     r1, [r1]
    cmp     r1, #0
    blxne   r1
    pop     {r0-r3, lr}
    cmp     r1, #0x5b               @ re-execute the original overwritten instruction
    ldr     pc, resume_lit          @ resume real dispatch at 0x209be8

spy_hook_slot_lit:
    .word   0x00000000        @ patched: address of the mutable hook-pointer slot

resume_lit:
    .word   0x00000000        @ patched: 0x209be8 (right after the original site)

    .globl hook_slot_modfac_spy
hook_slot_modfac_spy:
    .word   0x00000000        @ mutable slot: nam_preload writes the real hook here
