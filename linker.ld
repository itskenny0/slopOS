/* PicoOS kernel linker script (rebuilt for 2.1: the original was missing
 * from the 2.0 source zip). Linked at 0x10000, where boot.S and the UEFI
 * loader both put the kernel. .text.start must come first (entry point),
 * vgaintr.o second (see the note at the bottom of the Makefile). */
ENTRY(_kstart)
SECTIONS
{
    . = 0x10000;
    .text : {
        *(.text.start)
        *vgaintr.o(.text)
        *(.text .text.*)
    }
    .rodata : ALIGN(4) { *(.rodata .rodata.*) }
    .data   : ALIGN(4) { *(.data .data.*) }
    .bss    : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }
    _kernel_end = .;
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
