/* PicoOS UEFI loader.
 *
 * The firmware hands us a 64-bit long-mode machine with paging on and no
 * BIOS at all. The kernel is a 32-bit protected-mode binary that expects a
 * framebuffer instead of VGA text memory. So this loader has to:
 *
 *   1. find the graphics output protocol and grab the framebuffer
 *   2. read \picoos\kernel.bin off the volume we booted from
 *   3. place it at physical 0x10000
 *   4. work out which chunk of RAM the kernel may use as a heap
 *   5. exit boot services
 *   6. leave long mode and jump to the kernel in 32-bit protected mode
 */

#include "uefi.h"   /* minimal headers, no gnu-efi needed (see uefi.h) */

#define KERNEL_PHYS    0x00010000
#define BOOTINFO_PHYS  0x00005000
#define BOOTINFO_MAGIC 0x30434950u      /* 'PIC0' */

typedef struct {
    UINT32 magic;
    UINT32 firmware;       /* 1 = UEFI */
    UINT32 fb_base;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch;
    UINT32 fb_bpp;
    UINT32 fb_bgr;
    UINT32 heap_base;
    UINT32 heap_size;
    UINT32 archive_addr;   /* PAPP archive in RAM, 0 = behind the kernel */
    UINT32 archive_size;
} bootinfo_t;

/* Implemented in trampoline.S. Explicitly System V: the rest of this file
 * is built for the Microsoft ABI that UEFI requires, and getting this wrong
 * means jumping to a garbage address. */
extern void __attribute__((sysv_abi))
enter_kernel32(UINT64 entry, UINT64 bootinfo);

static EFI_GUID gop_guid  = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID lip_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID sfsp_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

/* ---- saying something without a working text console -------------------
 *
 * On a real machine the firmware's text console is not reliable once the
 * graphics mode has been touched, and after ExitBootServices it is gone
 * altogether. A boot that fails there shows a black screen and tells you
 * nothing, which is exactly the situation this exists to end.
 *
 * Blt is the one output that always works: it is part of every graphics
 * output protocol, including the Blt-only ones that have no framebuffer
 * address at all. So each stage of the boot floods the screen with a
 * colour. A machine that stops half way is then still telling you where,
 * from across the room, with no serial cable and no second computer:
 *
 *     blue     the loader is running and found the graphics protocol
 *     green    the kernel file was read off the EFI partition
 *     white    handing over to the kernel; the screen is now the kernel's
 *     red      the video mode is one a 32-bit kernel cannot draw on
 *     magenta  \picoos\kernel.bin is missing or unreadable
 *     yellow   the firmware would not release control (ExitBootServices)
 *     cyan     the firmware left too little memory
 *
 * Black straight after the vendor logo means none of this ran at all.
 */
static UINT32 *sig_fb = NULL;         /* set once the framebuffer checks out */
static int    bs_gone  = 0;           /* 1 after ExitBootServices */
static UINT32 sig_pitch, sig_w, sig_h, sig_bgr;

static void signal_colour(UINT8 r, UINT8 g, UINT8 b, UINTN hold_us)
{
    if (!sig_fb) return;

    UINT32 px = sig_bgr ? ((UINT32)r << 16) | ((UINT32)g << 8) | b
                        : ((UINT32)b << 16) | ((UINT32)g << 8) | r;
    for (UINT32 y = 0; y < sig_h; y++) {
        UINT32 *row = (UINT32 *)((UINT8 *)sig_fb + (UINTN)y * sig_pitch);
        for (UINT32 x = 0; x < sig_w; x++) row[x] = px;
    }
    if (!hold_us) return;

    /* Stall is a boot service, and the last colour we paint is deliberately
     * after ExitBootServices -- at which point calling one is a fault, not
     * an error code. That mistake is exactly what this comment is here to
     * stop the next person making. */
    if (!bs_gone) {
        uefi_call_wrapper(BS->Stall, 1, hold_us);
    } else {
        volatile UINT64 spin = hold_us * 40ULL;
        while (spin--) __asm__ __volatile__("pause");
    }
}

/* a few bytes out of the 16550, for the machine on the other side of
 * the serial cable: a loader that can say what it is doing is a loader
 * you can debug on real hardware. These are port instructions, not
 * memory writes: 0x3F8 is an address on the IO bus, not in RAM. */
static void ser_out(UINT8 v, UINT16 port)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "d"(port));
}

static UINT8 ser_in(UINT16 port)
{
    UINT8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "d"(port));
    return v;
}

static void ser_init(void)
{
    static int on;
    if (on) return;
    on = 1;
    ser_out(0x80, 0x3FB);            /* divisor latch on            */
    ser_out(0x01, 0x3F8);            /* 115200 baud                 */
    ser_out(0x00, 0x3F9);
    ser_out(0x03, 0x3FB);            /* 8 bits, no parity           */
}

static void ser_puts(const char *s)
{
    ser_init();
    for (; *s; s++) {
        int t;
        for (t = 0; t < 100000 && !(ser_in(0x3FD) & 0x20); t++) { }
        ser_out((UINT8)*s, 0x3F8);
    }
}

static void ser_dec(UINT32 v)
{
    char b[12]; int i = 0, n = 0; unsigned u = v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    char o[12];
    while (i) o[n++] = b[--i];
    o[n] = 0;
    ser_puts(o);
}

static void die(CHAR16 *msg)
{
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                      EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
    Print(L"\r\nPicoOS loader: %s\r\n", msg);
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                      EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    Print(L"Press any key to return to the firmware.\r\n");
    UINTN i;
    uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &i);
}

/* ---- pick a graphics mode we can actually draw text on ---------------- */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *setup_gop(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS st = uefi_call_wrapper(BS->LocateProtocol, 3,
                                      &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(st) || !gop) return NULL;

    /* If the mode the firmware is already in is one we can draw on, keep
     * it. Calling SetMode is not free: on a good deal of real firmware it
     * tears down and rebuilds the text console, and everything printed
     * afterwards goes nowhere -- a black screen on a machine that is in
     * fact working perfectly. A nicer resolution is not worth that.
     *
     * But a firmware that leaves the panel in 640x480 hands us a picture
     * sitting small in the top left of a big screen, taskbar and all, and
     * that is worth one SetMode: keep what is there only when it already
     * covers a reasonable share of the glass. */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *cur = gop->Mode->Info;
    if ((cur->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
         cur->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) &&
        gop->Mode->FrameBufferBase != 0 &&
        gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize
            <= 0xFFFFFFFFULL &&
        cur->HorizontalResolution >= 1024 &&
        cur->HorizontalResolution <= 1920 &&
        cur->VerticalResolution <= 1200) {
        ser_puts("gop: firmware mode is fine: ");
        ser_dec(cur->HorizontalResolution);
        ser_puts("x");
        ser_dec(cur->VerticalResolution);
        ser_puts("\r\n");
        return gop;
    }
    ser_puts("gop: firmware mode too small, looking for better\r\n");

    /* otherwise go looking for one that works */
    UINT32 best = gop->Mode->Mode;
    UINTN  best_score = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN sz;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &sz, &info)))
            continue;
        if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
            continue;
        if (info->HorizontalResolution > 1920 || info->VerticalResolution > 1200)
            continue;
        UINTN score = info->HorizontalResolution * info->VerticalResolution;
        if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768)
            score += 10000000;
        if (score > best_score) { best_score = score; best = m; }
    }
    ser_puts("gop: keep ");
    if (best != gop->Mode->Mode) {
        ser_puts("no, switch to mode ");
        ser_dec(best);
        ser_puts(": ");
        uefi_call_wrapper(gop->SetMode, 2, gop, best);
    }
    ser_dec(gop->Mode->Info->HorizontalResolution);
    ser_puts("x");
    ser_dec(gop->Mode->Info->VerticalResolution);
    ser_puts("\r\n");
    return gop;
}

/* ---- read the kernel from the volume this loader came from ------------ */
static EFI_STATUS load_kernel(EFI_HANDLE image, void **buf, UINTN *size)
{
    EFI_LOADED_IMAGE *li;
    EFI_STATUS st = uefi_call_wrapper(BS->HandleProtocol, 3,
                                      image, &lip_guid, (void **)&li);
    if (EFI_ERROR(st)) return st;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    st = uefi_call_wrapper(BS->HandleProtocol, 3,
                           li->DeviceHandle, &sfsp_guid, (void **)&fs);
    if (EFI_ERROR(st)) return st;

    EFI_FILE_HANDLE root, file;
    st = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(st)) return st;

    st = uefi_call_wrapper(root->Open, 5, root, &file, L"\\picoos\\kernel.bin",
                           EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st))
        st = uefi_call_wrapper(root->Open, 5, root, &file, L"\\KERNEL.BIN",
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) return st;

    /* how big is it? */
    UINTN info_sz = SIZE_OF_EFI_FILE_INFO + 256;
    EFI_FILE_INFO *info;
    st = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_sz,
                           (void **)&info);
    if (EFI_ERROR(st)) return st;
    EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    st = uefi_call_wrapper(file->GetInfo, 4, file, &fi_guid, &info_sz, info);
    if (EFI_ERROR(st)) return st;
    UINTN fsize = info->FileSize;
    uefi_call_wrapper(BS->FreePool, 1, info);

    /* park it at exactly 0x10000, where the kernel is linked to run */
    EFI_PHYSICAL_ADDRESS addr = KERNEL_PHYS;
    UINTN pages = (fsize + 0xFFF) / 0x1000 + 1;
    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress,
                           EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st)) {
        /* some firmware refuses that exact page; take any and copy later */
        addr = 0;
        st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiLoaderData, pages, &addr);
        if (EFI_ERROR(st)) return st;
    }

    /* Read in small pieces, never the whole file in one call. One call for
     * several MB turns into a single huge USB transfer, and the firmware's
     * USB 2.0 (EHCI) mass-storage driver on many boards simply never
     * finishes it: the stick in a black/grey port sat on the blue screen
     * for ever while the same stick in a blue USB 3 port booted. 64 KB per
     * call is what the firmware's own bootloaders use. A failed piece is
     * retried a few times before giving up. */
    UINTN read = 0;
    while (read < fsize) {
        UINTN want = fsize - read;
        if (want > 0x10000) want = 0x10000;
        UINTN got = want;
        int tries = 0;
        for (;;) {
            got = want;
            st = uefi_call_wrapper(file->Read, 3, file, &got,
                                   (void *)((UINTN)addr + read));
            if (!EFI_ERROR(st)) break;
            if (++tries >= 4) return st;
            /* back to where this piece started, then try again */
            uefi_call_wrapper(file->SetPosition, 2, file, (UINT64)read);
            uefi_call_wrapper(BS->Stall, 1, 50000);
        }
        if (got == 0) break;                       /* end of file */
        read += got;
    }
    uefi_call_wrapper(file->Close, 1, file);

    *buf  = (void *)(UINTN)addr;
    *size = read;
    return EFI_SUCCESS;
}

/* ---- the app archive inside KERNEL.BIN ----------------------------------
 *
 * The archive rides behind the kernel in KERNEL.BIN, but it must NOT be
 * copied down to 0x10000 with it: anything past 0xA0000 lands in the
 * VGA/BIOS hole and reads back as garbage. So the copy below stops where
 * the archive starts, and the kernel is told where the intact copy lives.
 * A missing or broken archive costs the extra files, never the boot. */

#define PAPP_MAGIC 0x50504150u        /* 'PAPP' little endian */

static UINT32 rd32(UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static void find_archive(UINT8 *file, UINTN fsize,
                         UINTN *arch_off, UINT32 *arch_total)
{
    *arch_off = 0; *arch_total = 0;
    for (UINTN off = 0; off + 512 <= fsize; off += 512) {
        if (rd32(file + off) != PAPP_MAGIC) continue;
        UINT32 total = rd32(file + off + 4);
        UINT32 count = rd32(file + off + 8);
        UINT32 ver   = rd32(file + off + 12);
        if (total < 16 || total > 12u * 1024u * 1024u) continue;
        if (off + total > fsize) continue;
        if (count == 0 || count > 64) continue;
        UINT32 entsize = (ver >= 2) ? 32 : 24;
        if (16 + count * entsize > total) continue;
        *arch_off = off; *arch_total = total;
        return;
    }
}

/* ---- biggest usable block of RAM below 4GB, above 1MB ----------------- */
static void find_heap(UINT32 *base, UINT32 *size)
{
    UINTN map_size = 0, map_key, desc_size;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;

    uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &map_key,
                      &desc_size, &desc_ver);
    map_size += 8 * desc_size;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, map_size,
                      (void **)&map);
    uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &map_key,
                      &desc_size, &desc_ver);

    UINT64 best_base = 0, best_size = 0;
    for (UINTN off = 0; off < map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + off);
        if (d->Type != EfiConventionalMemory) continue;

        UINT64 start = d->PhysicalStart;
        UINT64 end   = start + d->NumberOfPages * 4096;
        if (start < 0x200000) start = 0x200000;      /* stay clear of the kernel */
        if (end > 0xF0000000ULL) end = 0xF0000000ULL; /* 32-bit kernel, keep low */
        if (end <= start) continue;

        if (end - start > best_size) {
            best_size = end - start;
            best_base = start;
        }
    }
    uefi_call_wrapper(BS->FreePool, 1, map);

    if (best_size > 512ULL * 1024 * 1024) best_size = 512ULL * 1024 * 1024;
    *base = (UINT32)best_base;
    *size = (UINT32)best_size;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    InitializeLib(image, systab);
    ser_puts("picoos loader alive\r\n");
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    Print(L"PicoOS UEFI loader\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = setup_gop();
    if (!gop) { die(L"no usable graphics output protocol"); return EFI_UNSUPPORTED; }

    /* Everything below this point draws straight into the framebuffer the
     * firmware hands us, and once boot services are gone there is no way
     * left to report a problem. So the framebuffer gets checked here, while
     * the firmware console still works, and a machine we cannot draw on
     * gets a message instead of the black screen it used to get.
     *
     * The 4 GB test is the one that matters on real hardware: PicoOS is a
     * 32-bit kernel with paging off, so a framebuffer mapped high simply
     * cannot be reached. Truncating the address to 32 bits, which is what
     * this code used to do, points the console at whatever happens to live
     * at those low bits. */
    UINT64 fb = gop->Mode->FrameBufferBase;
    UINT64 fbsize = gop->Mode->FrameBufferSize;
    EFI_GRAPHICS_PIXEL_FORMAT pf = gop->Mode->Info->PixelFormat;

    ser_puts("gop: pixel format ");
    ser_dec((UINT32)pf);
    ser_puts(" fb ");
    {
        UINT32 hi = (UINT32)(fb >> 32);
        ser_dec(hi); ser_puts(":"); ser_dec((UINT32)fb);
        ser_puts(" size "); ser_dec((UINT32)(fbsize >> 10));
        ser_puts(" KB\r\n");
    }
    if (pf != PixelRedGreenBlueReserved8BitPerColor &&
        pf != PixelBlueGreenRedReserved8BitPerColor) {
        signal_colour(200, 0, 0, 0);             /* red: unusable video */
        die(L"this firmware offers no 32-bit RGB video mode");
        return EFI_UNSUPPORTED;
    }
    if (fb == 0 || fbsize == 0) {
        signal_colour(200, 0, 0, 0);
        die(L"the firmware gave no linear framebuffer (Blt-only video)");
        return EFI_UNSUPPORTED;
    }
    if (fb + fbsize > 0xFFFFFFFFULL) {
        signal_colour(200, 0, 0, 0);
        Print(L"framebuffer at 0x%lx is above 4 GB\r\n", fb);
        die(L"a 32-bit kernel cannot reach that framebuffer");
        return EFI_UNSUPPORTED;
    }

    sig_fb    = (UINT32 *)(UINTN)fb;
    sig_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    sig_w     = gop->Mode->Info->HorizontalResolution;
    sig_h     = gop->Mode->Info->VerticalResolution;
    sig_bgr   = (pf == PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
    signal_colour(0, 0, 200, 400000);            /* blue: video is usable */

    void *kernel; UINTN ksize;
    EFI_STATUS st = load_kernel(image, &kernel, &ksize);
    if (EFI_ERROR(st)) {
        signal_colour(200, 0, 200, 0);           /* magenta: no kernel file */
        die(L"could not read \\picoos\\kernel.bin");
        return st;
    }
    signal_colour(0, 170, 0, 400000);            /* green: kernel in memory */
    Print(L"kernel: %d bytes at 0x%lx\r\n", (int)ksize, (UINT64)(UINTN)kernel);

    UINTN arch_off = 0; UINT32 arch_total = 0;
    find_archive((UINT8 *)kernel, ksize, &arch_off, &arch_total);
    UINTN copy_len = arch_total ? arch_off : ksize;
    if (arch_total &&
        (UINT64)(UINTN)kernel + arch_off + arch_total > 0xFFFFFFFFULL) {
        signal_colour(200, 0, 0, 0);
        die(L"the app archive loaded above 4 GB, out of reach");
        return EFI_UNSUPPORTED;
    }
    if (arch_total)
        Print(L"archive: %d bytes at 0x%lx\r\n", (int)arch_total,
              (UINT64)(UINTN)kernel + arch_off);

    static bootinfo_t bi;
    bi.magic     = BOOTINFO_MAGIC;
    bi.firmware  = 1;
    bi.fb_base   = (UINT32)fb;              /* range-checked just above */
    bi.fb_width  = gop->Mode->Info->HorizontalResolution;
    bi.fb_height = gop->Mode->Info->VerticalResolution;
    bi.fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    bi.fb_bpp    = 32;
    bi.fb_bgr    = (gop->Mode->Info->PixelFormat ==
                    PixelBlueGreenRedReserved8BitPerColor) ? 1 : 0;
    find_heap(&bi.heap_base, &bi.heap_size);
    bi.archive_addr = arch_total ? (UINT32)((UINTN)kernel + arch_off) : 0;
    bi.archive_size = arch_total;

    Print(L"video : %dx%d @ 0x%lx\r\n", bi.fb_width, bi.fb_height,
          (UINT64)bi.fb_base);
    Print(L"heap  : %d KB at 0x%x\r\n", bi.heap_size / 1024, bi.heap_base);

    if (bi.heap_size < 256 * 1024) {
        signal_colour(0, 200, 200, 0);           /* cyan: no memory */
        die(L"the firmware left less than 256 KB of usable memory");
        return EFI_OUT_OF_RESOURCES;
    }

    /* On a real machine all of the above scrolls past in a fraction of a
     * second. Hold it long enough to be read, or photographed, because it
     * is the last thing the firmware can show if the handover goes wrong. */
    Print(L"\r\nstarting PicoOS...\r\n");
    uefi_call_wrapper(BS->Stall, 1, 1200000);      /* 1.2 seconds */

    /* ---- exit boot services; from here on the machine is ours ---- */
    UINTN map_size = 0, map_key, desc_size;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &map_key,
                      &desc_size, &desc_ver);
    map_size += 8 * desc_size;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, map_size, (void **)&map);

    for (int tries = 0; tries < 8; tries++) {
        UINTN sz = map_size;
        st = uefi_call_wrapper(BS->GetMemoryMap, 5, &sz, map, &map_key,
                               &desc_size, &desc_ver);
        if (EFI_ERROR(st)) break;
        st = uefi_call_wrapper(BS->ExitBootServices, 2, image, map_key);
        if (!EFI_ERROR(st)) break;
    }
    if (EFI_ERROR(st)) {
        signal_colour(200, 200, 0, 0);           /* yellow: firmware said no */
        die(L"ExitBootServices failed");
        return st;
    }

    bs_gone = 1;

    /* White means: the firmware is out of the way, the framebuffer has been
     * checked, and the next thing to touch this screen is PicoOS itself.
     * If the screen stays white, the kernel never got going. */
    signal_colour(255, 255, 255, 300000);

    /* the kernel may not be at 0x10000 yet if the firmware kept that page */
    if ((UINTN)kernel != KERNEL_PHYS) {
        UINT8 *d = (UINT8 *)KERNEL_PHYS, *s = (UINT8 *)kernel;
        for (UINTN i = 0; i < copy_len; i++) d[i] = s[i];
    }

    enter_kernel32(KERNEL_PHYS, (UINT64)(UINTN)&bi);
    return EFI_LOAD_ERROR;    /* never reached */
}
