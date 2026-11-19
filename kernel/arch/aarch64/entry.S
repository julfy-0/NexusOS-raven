/* Загрузка по протоколу arm64 Image (Documentation/arm64/booting.rst):
 * MMU выключен, x0 = физ. адрес DTB. Ожидаем EL2 или EL1. */
    .section .text.boot, "ax"
    .global _start
_start:
    b       start_real
    .long   0
    .quad   0                       /* text_offset */
    .quad   __image_size            /* effective image size */
    .quad   0xA                     /* LE, 4K pages, размещение в любом месте */
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241              /* "ARM\x64" */
    .long   0

start_real:
    mov     x19, x0                 /* DTB */
    msr     daifset, #0xf

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    at_el1

    /* EL2 -> EL1 (AArch64) */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #3
    msr     cnthctl_el2, x0         /* EL1 видит физ/вирт таймер */
    msr     cntvoff_el2, xzr
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0
    mov     x0, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0           /* FP/SIMD без трапа */
    isb

    /* x20 = адрес загрузки (MMU выключен, VA == PA) */
    adr     x20, _start

    /* Самоперемещение: *(load + off) = load + addend */
    adrp    x1, __rela_start
    add     x1, x1, :lo12:__rela_start
    adrp    x2, __rela_end
    add     x2, x2, :lo12:__rela_end
1:  cmp     x1, x2
    b.hs    2f
    ldp     x3, x4, [x1], #16
    ldr     x5, [x1], #8
    and     x4, x4, #0xffffffff
    cmp     x4, #1027               /* R_AARCH64_RELATIVE */
    b.ne    1b
    add     x5, x5, x20
    str     x5, [x20, x3]
    b       1b
2:
    /* sp + обнуление bss */
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __stack_bottom
    add     x1, x1, :lo12:__stack_bottom
3:  cmp     x0, x1
    b.hs    4f
    stp     xzr, xzr, [x0], #16
    b       3b
4:
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    mov     x0, x19
    mov     x1, x20
    bl      kmain_aarch64
5:  wfe
    b       5b

/* Таблица векторов: любое исключение -> exc_report(type) */
    .macro  vec idx
    .balign 128
    mov     x0, #\idx
    b       exc_entry
    .endm

    .balign 2048
    .global vectors
vectors:
    vec 0
    vec 1
    vec 2
    vec 3
    vec 4
    vec 5
    vec 6
    vec 7
    vec 8
    vec 9
    vec 10
    vec 11
    vec 12
    vec 13
    vec 14
    vec 15

exc_entry:
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      exc_report
6:  wfe
    b       6b
