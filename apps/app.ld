/* Every .pico program is linked flat at this address and relocated by the
 * kernel to wherever there is room (see exec.c), so the link address is
 * only a starting point. Images up to 1 MB load, for DOOM. */
ENTRY(_start)

SECTIONS
{
    . = 0x00020000;

    .text : {
        *(.text.start)          /* _start must come first */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    _image_end = .;

    .bss (NOLOAD) : {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
