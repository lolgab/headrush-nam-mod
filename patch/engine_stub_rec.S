@ engine_stub_rec.S -- engine stub for builds that classify engines by pedal name (the Reverb pedals), see
@ core/spy_patch.c, nam_elf_patch_engine_class (record_hook). Same as engine_stub.S, except that instead of filling a
@ table of {engine, &name QString} (the QString is a temporary of the caller: dangling by the time audio runs) it calls
@ the hook through a slot at the moment the pedal is created, while the name is still valid:
@   record(engine, &name)       slot = *lit_tab (0 until libnam_preload.so fills it: no call)
@ Assembled by hand into k_eng_stub_rec[]:
@   clang --target=armv7-none-eabi -c engine_stub_rec.S -o x.o && llvm-objcopy -O binary -j .text x.o x.bin
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
    ldr     ip, lit_tab
    ldr     ip, [ip]
    cmp     ip, #0
    movne   r1, r6
    blxne   ip
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
