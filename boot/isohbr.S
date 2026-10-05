/*  boot/isohbr.S -- the sector xorrisofs puts at the front of the hybrid ISO.
 *
 *  One file, every machine. picoos.iso already boots from a CD through El
 *  Torito; this sector makes the same file boot from a USB stick or a hard
 *  disk written with dd, which is where an MBR is expected instead.
 *
 *  It finds the kernel the same way the floppy boot sector does, by way of
 *  one number: xorrisofs writes the LBA of the El Torito boot image into
 *  offset 432 of this sector when it builds the ISO. That boot image *is*
 *  our 1.44 MB floppy image, and the kernel begins at its second sector, so
 *  the kernel is one sector past the number xorrisofs hands us.
 *
 *  Everything has to fit below 432: past that come the patched LBA, the
 *  disk signature, the partition table and the 0x55AA.
 */

    .code16
    .text
    .globl  start
start:
    jmp     real_start
    nop

real_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp              /* stack grows down, away from us */
    sti

    movb    %dl, boot_drive
    movb    %dl, 0x500                /* the kernel looks for its drive here */

    /* the boot image xorrisofs points at, plus one for its boot sector */
    movl    boot_image_lba, %eax
    incl    %eax
    movl    %eax, cur_lba

    movw    $KERNEL_SEG, cur_seg
    movw    kernel_sectors, %cx
load_loop:
    pushw   %cx
    call    read_sector
    popw    %cx
    incl    cur_lba
    addw    $0x20, cur_seg            /* 512 bytes is 0x20 paragraphs */
    loop    load_loop

    /* ---- A20, or the kernel wraps around at one megabyte ---- */
    inb     $0x92, %al
    testb   $2, %al
    jnz     a20_done
    orb     $2, %al
    andb    $0xFE, %al                /* never touch the reset bit */
    outb    %al, $0x92
a20_done:

    cli
    lgdt    gdt_desc
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmp    $0x08, $pmode

/* one sector at a time: some BIOSes refuse a longer transfer, and one
 * sector is the only length every one of them agrees on */
read_sector:
    movw    $5, retries
rs_try:
    movl    cur_lba, %eax
    movl    %eax, dap_lba
    movw    cur_seg, %ax
    movw    %ax, dap_seg
    movw    $dap, %si
    movb    $0x42, %ah                /* extended read */
    movb    boot_drive, %dl
    int     $0x13
    jnc     rs_ok
    xorw    %ax, %ax                  /* reset the controller, then again */
    movb    boot_drive, %dl
    int     $0x13
    decw    retries
    jnz     rs_try
    movw    $msg_err, %si
    call    print
hang16:
    hlt
    jmp     hang16
rs_ok:
    ret

print:
    pushw   %ax
    pushw   %bx
p_loop:
    lodsb
    testb   %al, %al
    jz      p_done
    movb    $0x0E, %ah
    movw    $0x0007, %bx
    int     $0x10
    jmp     p_loop
p_done:
    popw    %bx
    popw    %ax
    ret

    .code32
pmode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x0000F000, %esp
    xorl    %eax, %eax                /* no bootinfo block on this path */
    ljmp    $0x08, $0x00010000

/* ---------------------------------------------------------------- */

KERNEL_SEG = 0x1000                   /* the kernel is loaded at 0x10000 */

    .align 8
gdt:
    .quad   0x0000000000000000        /* null */
    .quad   0x00CF9A000000FFFF        /* 0x08 code, base 0, limit 4G, 32-bit */
    .quad   0x00CF92000000FFFF        /* 0x10 data */
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt

dap:
    .byte   0x10
    .byte   0
    .word   1                         /* one sector at a time */
dap_off:
    .word   0
dap_seg:
    .word   0
dap_lba:
    .long   0
    .long   0

boot_drive:      .byte 0
cur_lba:         .long 0
cur_seg:         .word 0
retries:         .word 0

msg_err:         .asciz "disk err\r\n"

/* The image tools patch the real size in here, found through the symbol
 * table the same way it is done for the floppy boot sector. */
    .org 428
kernel_sectors:
    .word   311

/* Where xorrisofs writes the LBA of the El Torito boot image. It has to be
 * 432 exactly: that is the address it patches, and nothing else. */
    .org 432
boot_image_lba:
    .long   0

    .org 510
    .word   0xAA55
