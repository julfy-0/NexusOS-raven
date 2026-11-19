/* NexusOS aarch64 / Pixel 6 Pro (gs101). Образ линкуется с 0 и самоперемещается
 * в entry.S (R_AARCH64_RELATIVE), адрес загрузки задаёт ABL. */
ENTRY(_start)

SECTIONS
{
    . = 0;
    __kernel_start = .;

    .text : ALIGN(4096) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }

    .rela.dyn : ALIGN(8) {
        __rela_start = .;
        *(.rela .rela.*)
        __rela_end = .;
    }

    .dynamic : ALIGN(8) { *(.dynamic) }

    .data : ALIGN(4096) {
        *(.data .data.*)
        *(.got .got.*)
    }

    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x10000;
        __stack_top = .;
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;
    __image_size = ABSOLUTE(__kernel_end - __kernel_start);

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
        *(.interp)
    }
}
