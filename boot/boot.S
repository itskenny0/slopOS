/* PicoOS stage-1 bootloader
 * 512 bytes, real mode, loaded by the BIOS at 0x7C00.
 * Reads the kernel off whatever device we booted from (LBA when the BIOS
 * supports int13h extensions, CHS otherwise), enables A20, switches the CPU
 * to 32-bit protected mode and jumps into the kernel.
 */
    .code16
    .section .text
    .global _start

#define KERNEL_SEG   0x1000      /* physical 0x10000 */
#define KERNEL_LBA   1           /* kernel starts at LBA 1 (2nd sector) */

_start:
    jmp     real_start
    nop

real_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp
    sti

    movb    %dl, boot_drive         /* the BIOS hands us the boot drive in DL */

    movw    $msg_boot, %si
    call    print

    /* ---- how much RAM is there? (KB above 1MB) ---- */
    movb    $0x88, %ah
    int     $0x15
    jc      1f
    movw    %ax, %es:(0x500)        /* stash at 0x500 for the kernel */
    jmp     2f
1:  movw    $0, %es:(0x500)
2:

    /* ---- do we have int13h LBA extensions? ---- */
    movb    $0x41, %ah
    movw    $0x55AA, %bx
    movb    boot_drive, %dl
    int     $0x13
    jc      use_chs
    cmpw    $0xAA55, %bx
    jne     use_chs
    movb    $1, lba_ok
    jmp     geom_done

use_chs:
    /* ---- ask the BIOS for the real drive geometry ---- */
    movb    $0x08, %ah
    movb    boot_drive, %dl
    pushw   %es
    xorw    %di, %di
    movw    %di, %es
    int     $0x13
    popw    %es
    jc      geom_done               /* keep the 18/2 floppy defaults */
    movb    %cl, %al
    andb    $0x3F, %al              /* CL bits 0-5 = sectors per track */
    jz      geom_done
    xorb    %ah, %ah
    movw    %ax, sectors_per_track
    movb    %dh, %al
    incb    %al                     /* DH = max head index */
    xorb    %ah, %ah
    movw    %ax, num_heads
geom_done:

    /* ---- load the kernel, one sector at a time ---- */
    movw    $KERNEL_LBA, cur_lba
    movw    $KERNEL_SEG, cur_seg
    movw    kernel_sectors, %cx
load_loop:
    pushw   %cx
    call    read_sector
    popw    %cx
    incw    cur_lba
    addw    $0x20, cur_seg          /* 512 bytes = 0x20 paragraphs */
    loop    load_loop

    movw    $msg_ok, %si
    call    print

    /* ---- A20 gate (fast port 0x92) ---- */
    inb     $0x92, %al
    testb   $2, %al
    jnz     a20_done
    orb     $2, %al
    andb    $0xFE, %al              /* never touch the reset bit */
    outb    %al, $0x92
a20_done:

    /* ---- into protected mode ---- */
    cli
    lgdt    gdt_desc
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmp    $0x08, $pmode

/* ------------------------------------------------------------------ */
/* read the sector in cur_lba to cur_seg:0000                          */
read_sector:
    movw    $5, retries
rs_try:
    cmpb    $0, lba_ok
    je      rs_chs

    /* --- LBA path: int13h AH=42h with a disk address packet --- */
    movw    cur_lba, %ax
    movw    %ax, dap_lba
    movw    cur_seg, %ax
    movw    %ax, dap_seg
    movw    $dap, %si
    movb    $0x42, %ah
    movb    boot_drive, %dl
    int     $0x13
    jnc     rs_ok
    jmp     rs_retry

    /* --- CHS path: lba -> cylinder/head/sector --- */
rs_chs:
    movw    cur_lba, %ax
    xorw    %dx, %dx
    divw    sectors_per_track
    movb    %dl, %cl
    incb    %cl                     /* CL = sector, 1-based */
    xorw    %dx, %dx
    divw    num_heads               /* AX = cylinder, DX = head */
    movb    %al, %ch
    movb    %ah, %al
    shlb    $6, %al
    orb     %al, %cl                /* cylinder bits 8-9 into CL 6-7 */
    movb    %dl, %dh                /* DH = head */
    movb    boot_drive, %dl

    movw    cur_seg, %bx
    movw    %bx, %es
    xorw    %bx, %bx
    movw    $0x0201, %ax            /* AH=02 read, AL=1 sector */
    int     $0x13
    pushw   %ax
    xorw    %ax, %ax
    movw    %ax, %es
    popw    %ax
    jnc     rs_ok

rs_retry:
    xorw    %ax, %ax                /* reset the controller and try again */
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

/* print a NUL-terminated string at DS:SI with BIOS teletype */
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

/* ------------------------------------------------------------------ */
    .code32
pmode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    /* A few dozen bytes of stack to reach the kernel, which switches to its
     * own straight away. Keep it low: anything higher would force every
     * machine to have that much RAM before PicoOS has even started. */
    movl    $0x0000F000, %esp
    xorl    %eax, %eax              /* no bootinfo block on the BIOS path */
    ljmp    $0x08, $0x00010000

/* ------------------------------------------------------------------ */
    .align 8
gdt:
    .quad   0x0000000000000000      /* null */
    .quad   0x00CF9A000000FFFF      /* 0x08 code: base 0, limit 4G, 32-bit */
    .quad   0x00CF92000000FFFF      /* 0x10 data: base 0, limit 4G, 32-bit */
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt

/* disk address packet for the LBA read path */
dap:
    .byte   0x10
    .byte   0
    .word   1                       /* sector count */
dap_off:
    .word   0                       /* buffer offset */
dap_seg:
    .word   0                       /* buffer segment */
dap_lba:
    .long   0                       /* LBA low  */
    .long   0                       /* LBA high */

boot_drive:        .byte 0
lba_ok:            .byte 0
sectors_per_track: .word 18
num_heads:         .word 2
cur_lba:           .word 0
cur_seg:           .word 0
retries:           .word 0

msg_boot: .asciz "PicoOS\r\n"
msg_ok:   .asciz "loading\r\n"
msg_err:  .asciz "disk err\r\n"

/* Patched with the real kernel size by the image tools, which locate it
 * through the symbol table. Pinned just below 0x1B8 because that is where
 * the MBR disk signature has to live -- UEFI firmware refuses to create a
 * boot entry for a disk whose signature is zero. */
    .org 436
kernel_sectors:
    .word 64

/* How many of those sectors are the kernel proper. Anything after them is
 * the app archive, added later by tools/addapp.py. The tool needs this to
 * know where the kernel ends; the kernel itself works it out from its own
 * _kernel_end symbol and never reads this. */
kernel_only:
    .word 64

    .org 440
disk_signature:
    .long 0                     /* filled in by the image tools */
    .word 0                     /* "copy protected" marker, always 0 */

/* MBR partition table -- many BIOSes refuse to boot a USB stick without one.
 * Filled in by tools/mkimage.py. */
    .org 446
partition_table:
    .space 64, 0

    .org 510
    .word 0xAA55
