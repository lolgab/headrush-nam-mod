@ Additive "Neural Amp Modeler" ModFac_construct case, injected as a detour.
@
@ Evil's ModFac_construct (0x209bc8) dispatches on r1 (type index) via
@ `cmp r1,#0x5b; ldrls pc,[pc,r1,lsl 2]` at 0x209be4/0x209be8, table at
@ 0x209bf0, 92 packed entries (0-91), no room to extend in place.
@
@ Patch strategy: overwrite ONLY the single `cmp r1,#0x5b` instruction at
@ 0x209be4 with a `b dispatch_stub` (plain relative branch, computed by the
@ Python patcher -- fits in the 4 bytes we're overwriting, no literal pool
@ needed at the overwrite site). Everything from 0x209be8 onward (the real
@ `ldrls` dispatch, the 92-entry table, all 92 real case bodies, the default
@ path at 0x209bec, the common tail at 0x209d80) is left completely
@ untouched.
@
@ dispatch_stub replicates the overwritten comparison so cases 0-91 and the
@ out-of-range default are provably bit-for-bit unaffected; only r1==92 is
@ newly special-cased here.
@
@ case92_body clones Gonkulator's own case (construct-case 0x44, disassembled
@ at 0x20a0d8-0x20a0f8): same 0x280-byte shell size, same shared secondary
@ vtable constant (0x1610f14), same reused slotA (0x1822ecc, Gonkulator's own
@ generic_init_helper argument -- safe to reuse verbatim since we don't
@ change controller behavior at all). slotB is the ONE new value: our own
@ cloned primary (DSPModule-level) vtable's address, patched into slotB_lit
@ after assembly (its value isn't known until the Python patcher lays out
@ the rest of the injected segment).
@
@ Common tail's calling convention (confirmed by disassembling 0x209d80):
@ obj pointer must be in fp (r11), slotB value in r1.

    .arm
    .globl dispatch_stub
dispatch_stub:
    cmp     r1, #92
    beq     case92_body
    cmp     r1, #0x5b              @ replicate the overwritten instruction
    ldr     pc, back_to_209be8_lit  @ resume original dispatch, flags intact

back_to_209be8_lit:
    .word   0x00209be8

    .globl case92_body
case92_body:
    mov     r0, #0x280
    ldr     r12, =0x000175b4        @ operator_new(unsigned int) -- plain function, not PLT
    blx     r12
    mov     fp, r0                  @ obj saved in fp (callee-saved, survives next call)
    ldr     r3, =0x01610f14         @ COMMON_SECONDARY_VTABLE (shared by all 92 real cases)
    str     r3, [r0]
    ldr     r1, =0x01822ecc         @ slotA = Gonkulator's own registry[40], reused verbatim
    ldr     r12, =0x00490d58        @ generic_init_helper
    blx     r12                     @ r0 still holds obj from the operator_new call above
    ldr     r1, slotB_lit           @ slotB = our new primary (DSPModule-level) vtable address
    ldr     pc, tail_lit            @ absolute jump into the real, untouched common tail

    .align 2
slotB_lit:
    .word   0x00000000        @ patched: our new primary vtable's address

tail_lit:
    .word   0x00209d80
