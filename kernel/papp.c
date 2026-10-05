/* papp.c -- the app archive
 *
 * Adding a program to PicoOS used to mean rebuilding the kernel, because the
 * programs are baked into the kernel image as static data. That is fine for
 * the ones that ship with it and useless for anything you write yourself.
 *
 * So there is a second place programs can live: a small archive glued onto
 * the back of the kernel on disk. The boot sector already reads a run of
 * sectors starting at LBA 1 and it does not care what is in them, so making
 * the run longer is enough to get the archive into memory for free -- no
 * second load, no disk driver, no filesystem. Under UEFI it is even simpler:
 * the archive is part of KERNEL.BIN, and the loader reads whole files.
 *
 * The kernel finds it by looking at the first sector boundary after its own
 * `_kernel_end`, which is exactly where tools/addapp.py puts it. Files are
 * mounted in place -- the ramdisk points straight at them, nothing is copied
 * to the heap -- and the heap simply starts after the archive instead of
 * after the kernel.
 *
 *     archive header      'PAPP', total size, file count, format version
 *     file count entries  16-byte name, stored size, offset, flags,
 *                         original size
 *     the file data
 *
 * Version 2 added the last two fields, because entries may now be packed
 * with tools/lzss.py. A packed entry does not go into the ramdisk at boot;
 * it goes into the store and waits to be asked for. A plain one is mounted
 * in place exactly as before, which is what text files want -- `cat` on a
 * compressed file would have to unpack it into the heap first, and a
 * readme is not worth that.
 *
 * Version 1 archives still load: the entries are 24 bytes instead of 32 and
 * everything in them is uncompressed. Old images keep working, which is
 * cheap to support and saves anyone with a stick already written from
 * having to write it again.
 *
 * There is no checksum on the archive itself. Every .pico inside it already
 * carries its own, and a text file that arrives corrupt will look corrupt.
 */

#include "pico.h"

#define PAPP_MAGIC   0x50504150u        /* 'PAPP' little endian */
#define PAPP_MAX     (12u * 1024 * 1024) /* refuse anything absurd */
#define PAPP_MAXFILE 64

typedef struct {
    u32 magic;
    u32 total_size;
    u32 count;
    u32 version;
} papp_hdr_t;

#define PAPP_PACKED 1u          /* entry flag: the data is an LZSS stream */

typedef struct {
    char name[FS_NAMELEN];
    u32  size;                  /* bytes as stored in the archive */
    u32  offset;                /* from the start of the archive  */
    u32  flags;
    u32  orig;                  /* unpacked size; == size if plain */
} papp_ent_t;

typedef struct {
    char name[FS_NAMELEN];
    u32  size;
    u32  offset;
} papp_ent_v1_t;

extern char _kernel_end[];

/* Where the archive would be if there is one: the first 512-byte boundary
 * at or after the end of the kernel image. */
static const papp_hdr_t *papp_at(void)
{
    /* Under UEFI the loader keeps the archive where it loaded it (above
     * 1 MB, intact) and passes the address along, because a copy at
     * 0x10000 would span the VGA/BIOS hole. On the BIOS path there is no
     * bootinfo and the archive sits right behind the kernel as before. */
    bootinfo_t *bi = bootinfo();
    if (bi && bi->archive_addr && bi->archive_size) {
        const papp_hdr_t *h = (const papp_hdr_t *)bi->archive_addr;
        if (h->magic == PAPP_MAGIC && h->total_size == bi->archive_size)
            return h;
    }
    u32 a = ((u32)_kernel_end + 511u) & ~511u;
    return (const papp_hdr_t *)a;
}

/* Returns the number of bytes the archive occupies, or 0 if there is none.
 * Called by mem_init before anything else, so it must not allocate. */
u32 papp_size(void)
{
    const papp_hdr_t *h = papp_at();

    if (h->magic != PAPP_MAGIC) return 0;
    if (h->total_size < sizeof *h || h->total_size > PAPP_MAX) return 0;
    if (h->count > PAPP_MAXFILE) return 0;

    u32 entsize = (h->version >= 2) ? sizeof(papp_ent_t)
                                    : sizeof(papp_ent_v1_t);
    if (sizeof *h + h->count * entsize > h->total_size) return 0;

    return h->total_size;
}

u32 papp_base(void)
{
    return papp_size() ? (u32)papp_at() : 0;
}

int papp_count(void)
{
    return papp_size() ? (int)papp_at()->count : 0;
}

/* Plain entries are mounted into the ramdisk in place; packed ones are
 * registered with the store and unpacked only if somebody asks. */
void papp_install(void)
{
    u32 total = papp_size();
    if (!total) return;

    const papp_hdr_t *h = papp_at();
    const char *base = (const char *)h;
    int v2 = (h->version >= 2);

    for (u32 i = 0; i < h->count; i++) {
        papp_ent_t e;
        if (v2) {
            e = ((const papp_ent_t *)(h + 1))[i];
        } else {
            const papp_ent_v1_t *o = &((const papp_ent_v1_t *)(h + 1))[i];
            memcpy(e.name, o->name, FS_NAMELEN);
            e.size = o->size; e.offset = o->offset;
            e.flags = 0; e.orig = o->size;
        }

        /* a bad entry costs that one file, not the boot */
        if (e.offset < sizeof *h) continue;
        if (e.offset + e.size > total) continue;
        if (!e.name[0]) continue;

        char name[FS_NAMELEN];
        memcpy(name, e.name, FS_NAMELEN);
        name[FS_NAMELEN - 1] = 0;

        if (e.flags & PAPP_PACKED) {
            if (e.orig == 0 || e.orig > PAPP_MAX) continue;
            store_add(name, (const u8 *)(base + e.offset), e.size, e.orig);
        } else {
            fs_mount(name, base + e.offset, e.size);
        }
    }
}
