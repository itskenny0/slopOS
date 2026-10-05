/* Kernel entry point. The bootloader jumps here at phys 0x00010000
 * already in 32-bit protected mode with flat segments. */
    .code32
    .section .text.start
    .global _kstart
_kstart:
    cli
    /* The loader hands us EAX = 'PIC0' and EBX = the bootinfo block, the
     * same trick multiboot uses. A plain BIOS boot arrives with EAX = 0. */
    movl    $0, %edx
    cmpl    $0x30434950, %eax
    jne     1f
    movl    %ebx, %edx
1:

    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $stack_top, %esp
    movl    %esp, %ebp

    /* Zero the BSS. Under BIOS that memory happens to be zero already, but
     * UEFI hands us a machine full of firmware leftovers. */
    movl    $_bss_start, %edi
    movl    $_bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    cld
    rep stosb

    /* Paint the stack with a known pattern -- after the bss wipe, because
     * the stack lives inside the bss. Nothing depends on this; it just lets
     * the kernel report afterwards how deep the stack ever really got,
     * which is the only honest way to size it. Leave the words currently
     * in use alone: paint from the bottom up to the live stack pointer. */
    movl    $stack_bottom, %edi
    movl    %esp, %ecx
    subl    %edi, %ecx
    subl    $64, %ecx           /* stay well clear of the live frame */
    shrl    $2, %ecx
    movl    $0xB5B5B5B5, %eax
    cld
    rep stosl

    /* stash the pointer now that the BSS is clean (EDX survived rep stosb) */
    movl    %edx, boot_info_ptr
    pushl   $0                  /* null return address, ends stack traces */
    call    kmain
.hang:
    cli
    hlt
    jmp     .hang

    .section .bss
    .align 4
    .global boot_info_ptr
boot_info_ptr:
    .skip 4

    .align 16
    .global stack_bottom
stack_bottom:
    /* 8 KiB. Measured, not guessed: entry.S paints this region and `res`
     * reports the high-water mark, so if that number ever creeps up the
     * machine will say so. It was 4 KiB for a long time, which was plenty
     * while the shell only ran programs -- but the shell is also what runs
     * `wget`, and that path is the whole network stack plus a TLS
     * handshake deep, which does not fit in four kilobytes. It went 152
     * bytes past the bottom and quietly rewrote the variables below it:
     * the console froze, arp stopped answering, and the fetch hung. */
    .skip 8192
    .global stack_top
stack_top:
