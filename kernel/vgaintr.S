/* vgaintr.S -- calling the VGA BIOS from a 32-bit kernel
 *
 * New in 1.0. The kernel needs three things only the real-mode video BIOS
 * does safely on every machine made since 1987: switch video modes
 * (640x480x16 for the drawing program, text for everything else), load a
 * different font (80x50 lines), and patch single glyphs (the hand that is
 * the mouse pointer). Programming the VGA registers by hand works too,
 * until you meet the card that wires them differently -- the BIOS is the
 * thing every card promises to get right.
 *
 * The trick that keeps this simple: this kernel is linked at 0x10000, the
 * start of the second 64 K of memory, and this object is linked first
 * (see the Makefile -- vga.o must stay under 0x20000). A 16-bit code
 * descriptor with base 0x10000 therefore reaches every label in this file
 * with a plain 16-bit offset, and no descriptor ever has to be patched at
 * run time.
 *
 * The real-mode stack is 512 bytes of .bss, 16-byte aligned, below 1 MB,
 * so a real-mode segment can address it. The 32-bit stack pointer waits
 * in a .bss word. Interrupts are masked for the whole trip; ds keeps the
 * flat 32-bit descriptor ("unreal mode"), which the BIOS does not mind.
 */

.text
.code32

/* Writable on purpose: the code16 base is patched here at run time. */
.data
.align 8
vg_gdt:
    .quad   0x0000000000000000      /* 0x00 null                        */
    .quad   0x00CF9A000000FFFF      /* 0x08 code32, flat, 4 GB          */
    .quad   0x00CF92000000FFFF      /* 0x10 data32, flat, 4 GB          */
    .quad   0x00009A010000FFFF      /* 0x18 code16, base 0x10000        */
    .quad   0x000092000000FFFF      /* 0x20 data16, base 0, 64 kb       */
vg_gdt_end:
.align 8
vg_gdt_desc:
    .word   vg_gdt_end - vg_gdt - 1
    .long   vg_gdt

.bss
.align 16
vg_rm_stack:
    .space  4096                    /* a VBE mode set eats a real-mode
                                       stack by the kilobyte; 1 KB let it
                                       crash through the bottom into the
                                       kernel's own data */
vg_saved_eax:
    .space  4
vg_saved_esp:
    .space  4

/* every far jump below goes through these so the offsets are written
 * relative to the 0x10000 base of the 16-bit segments, not absolute */
#define OFF16(label)  (label - 0x10000)

.text
.code32
.globl vga_bios_int10
.type   vga_bios_int10, @function
/* void vga_bios_int10(u32 ax, u32 bx, u32 cx, u32 dx, u32 es, u32 bp) */
vga_bios_int10:
    pushl   %ebp
    movl    %esp, %ebp
    pushfl
    pushl   %ebx
    pushl   %esi
    pushl   %edi
    cli                                 /* no interrupts in either world */

    movl    %esp, vg_saved_esp

    lgdt    vg_gdt_desc

    /* the ax argument is read first: the bp argument below is loaded
     * straight into ebp, which retires this frame pointer for good */
    movl    8(%ebp), %eax               /* ax, first   */
    movl    %eax, vg_saved_eax          /* the mode dance below uses eax */
    movl    12(%ebp), %ebx              /* bx          */
    movl    16(%ebp), %ecx              /* cx          */
    movl    20(%ebp), %edx              /* dx          */
    movl    24(%ebp), %edi              /* es (a seg)  */
    movl    28(%ebp), %ebp              /* bp (an off) — last frame read */

    ljmp    $0x18, $OFF16(1f)

1:  .code16
    /* 16-bit protected mode, still on the 32-bit stack (ss = 0x10). */
    movw    $0x20, %ax
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    /* onto the low stack before anything can touch it */
    movw    $0x20, %ax
    movw    %ax, %ss
    xorl    %esp, %esp
    movw    $0xFFE, %sp

    /* protected mode off. The far jump to real mode cannot be assembled
     * as a constant -- a 16-bit relocation cannot express "my address
     * minus 0x10000" -- so the segment and offset are built in esi at
     * run time and the jump is a far return through them */
    movl    %cr0, %eax
    andl    $0x7FFFFFFE, %eax
    movl    %eax, %cr0
    movl    $2f, %esi
    shrl    $4, %esi
    pushw   %si                          /* the segment */
    movl    $2f, %esi
    andl    $0xF, %esi
    pushw   %si                          /* the offset */
    lretw

2:  /* real mode. interrupts are masked at the PIC the whole time. */
    movl    $vg_rm_stack, %esi
    shrl    $4, %esi
    movw    %si, %ss
    movw    $0xFFE, %sp
    movw    %di, %es                    /* es:bp = the caller's buffer   */
    movw    %bp, %di                    /* ...and es:di, for the VBE     */

    /* A 386 keeps the protected-mode IDTR cached across the switch to
     * real mode, and software interrupts vector through IDTR.base even
     * there -- not through the IVT at 0:0. Our kernel lidt'd its own
     * table, so without this the BIOS call would land inside the kernel
     * IDT's bytes and derail. Build the 6-byte descriptor below the
     * saved registers, aim the IDTR at the real IVT at 0:0, and give
     * every register back untouched: the two pushes cover eax/ebx,
     * (%bx) addresses through ds so ds borrows ss, and the memory is
     * free again the moment the lidt retires. The C wrapper reloads
     * the kernel table before it unmasks the PIC again. */
    pushl   %eax
    pushl   %ebx
    movl    $(vg_rm_stack + 0xFEC), %esi
    movl    $0x03FF, %eax               /* limit: 256 entries            */
    movw    %ax, (%esi)
    movl    $0, %eax                    /* base: 0                       */
    movl    %eax, 2(%esi)
    movw    $0xFEC, %bx
    pushw   %ss
    popw    %ds
    data32  lidtw (%bx)
    pushw   $0
    popw    %ds
    popl    %ebx
    popl    %eax

    /* the caller's ax, stashed before the segment dances ate it */
    movl    $(vg_saved_eax), %esi
    movl    (%esi), %eax

    int     $0x10

    /* save what the BIOS said, then go back up */
    pushl   %eax
    pushl   %ebx

    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    /* a 32-bit far return, built by hand because the assembler would
     * write a 16-bit offset for ljmp here */
    pushl   $0x08
    pushl   $3f
    data32 lret

3:  .code32
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    vg_saved_esp, %esp

    /* the BIOS results were pushed on the low stack, 8 bytes below its
     * top -- a linear address, and ds is flat again, so an ordinary read */
    movl    $(vg_rm_stack + 0xFF6), %esi
    movl    (%esi), %ebx
    movl    4(%esi), %eax

    popl    %edi
    popl    %esi
    popl    %ebx
    popfl                               /* flags back                    */
    cli                                 /* ...but interrupts stay off:
                                           a VBE mode set takes long
                                           enough that a timer tick is
                                           always pending, and letting it
                                           fire here -- masks still down,
                                           IDT freshly reloaded -- froze
                                           the whole machine. The C wrapper
                                           restores the PIC first        */
    popl    %ebp
    ret
