/* Long mode -> 32-bit protected mode.
 *
 * Called from the UEFI loader after ExitBootServices. The loader compiles
 * its firmware calls with the Microsoft ABI, but this is a plain C function
 * and therefore uses System V:
 *   RDI = kernel entry point (physical)
 *   RSI = address of the bootinfo block
 *
 * The CPU is in 64-bit long mode with paging on and an identity map. To hand
 * control to a 32-bit kernel we must, in this order: jump into a 32-bit
 * compatibility code segment, turn paging off (that clears EFER.LMA), clear
 * EFER.LME, drop CR4.PAE, then jump to the kernel.
 */

    .section .text
    .code64
    .global enter_kernel32

enter_kernel32:
    cli

    /* EBX carries the bootinfo pointer into the kernel; the block itself
     * stays where the firmware put it, which beats trusting a fixed low
     * address that the firmware page tables may not map straight through. */
    movq    %rsi, %rbx

    /* RDI still holds the entry point; its low half EDI survives the mode
     * switch, and R8-R15 will not exist once we leave long mode. */

    /* load a GDT that has a real 32-bit code segment in it */
    leaq    gdt32(%rip), %rax
    movq    %rax, gdt32_ptr + 2(%rip)
    lgdt    gdt32_ptr(%rip)

    /* far return into the 32-bit compatibility code segment */
    leaq    compat32(%rip), %rax
    pushq   $0x08
    pushq   %rax
    lretq

    .code32
compat32:
    /* reload the data segments with 32-bit descriptors */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* paging off -> the CPU leaves long mode (LMA clears) */
    movl    %cr0, %eax
    andl    $0x7FFFFFFF, %eax
    movl    %eax, %cr0

    /* clear EFER.LME so we are properly back in legacy mode */
    movl    $0xC0000080, %ecx
    rdmsr
    andl    $0xFFFFFEFF, %eax
    wrmsr

    /* PAE off */
    movl    %cr4, %eax
    andl    $0xFFFFFFDF, %eax
    movl    %eax, %cr4

    /* fresh stack below the 640K line, then into the kernel.
     * EAX = magic, EBX = bootinfo, exactly what entry.S expects. */
    /* Same reasoning as the bios path: a low scratch stack, just enough to
     * reach the kernel entry point. 0x0F000 is below the kernel and above
     * the real-mode vectors, and is conventional memory on any machine. */
    movl    $0x0000F000, %esp

    movl    $0x30434950, %eax
    jmp     *%edi

    .section .data
    .align 16
gdt32:
    .quad   0x0000000000000000      /* null */
    .quad   0x00CF9A000000FFFF      /* 0x08: 32-bit code, base 0, limit 4G */
    .quad   0x00CF92000000FFFF      /* 0x10: 32-bit data, base 0, limit 4G */
gdt32_end:

    .align 16
gdt32_ptr:
    .word   gdt32_end - gdt32 - 1
    .quad   0                       /* base, filled in at runtime */
