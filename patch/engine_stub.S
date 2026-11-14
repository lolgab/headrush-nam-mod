@ engine_stub.S -- the additive NAM pedals' module/engine glue (see core/spy_patch.c, nam_elf_patch_engine_class).
@ Assembled by hand into k_eng_stub[]; kept here as the source of truth:
@   clang --target=armv7-none-eabi -c engine_stub.S -o x.o && llvm-objcopy -O binary -j .text x.o x.bin
@
@ setup_stub replaces slot 19 of the cloned module vtable: it runs the original setup (module, parent, &name, statemap,
@ [sp]=registry), then (a) repoints the engine's vtable (module+0x270) at the clone whose process() slot is `tramp`, and
@ (b) records {engine, &name QString} in a small table so the hook can tell which NAM instance an engine belongs to.
    .arm
    .globl _start
_start:
setup_stub:
    push    {r4-r6, lr}
    ldr     r12, [sp, #16]
    sub     sp, sp, #8
    str     r12, [sp]
    mov     r4, r0
    mov     r6, r2
    ldr     ip, lit_setup
    blx     ip
    add     sp, sp, #8
    mov     r5, r0
    ldr     r0, [r4, #0x270]
    cmp     r0, #0
    beq     1f
    ldr     r1, lit_evt
    str     r1, [r0]
    ldr     r1, lit_tab
    ldr     r2, [r1]
    cmp     r2, #8
    bhs     1f
    add     r3, r1, #4
    add     r3, r3, r2, lsl #3
    str     r0, [r3]
    str     r6, [r3, #4]
    add     r2, r2, #1
    str     r2, [r1]
1:  mov     r0, r5
    pop     {r4-r6, pc}
tramp:
    ldr     ip, lit_slot
    ldr     ip, [ip]
    cmp     ip, #0
    bxne    ip
    ldr     pc, lit_orig
lit_setup: .word 0x0027cc20
lit_evt:   .word 0
lit_slot:  .word 0
lit_orig:  .word 0x002c6b68
lit_tab:   .word 0
