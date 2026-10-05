/* store.c -- the program store
 *
 * Programs used to be mounted into the ramdisk at boot and sit there
 * forever, whether you ran them or not. On a machine with a megabyte that
 * is the wrong default: tetris is 5.5 KB of address space that most boots
 * never touch.
 *
 * So the shipped programs now travel compressed and stay compressed. The
 * store is a list of packed blobs -- some baked into the kernel image, some
 * glued on the back of it by tools/addapp.py -- and nothing is unpacked
 * until something asks for it. `start tetris` unpacks it into the ramdisk
 * on the way, so the store is invisible: there is no command to operate
 * it, because there is nothing for you to do.
 *
 * It briefly had one -- a `store` listing and a `get` to unpack by hand --
 * and that was a mistake. `start` already did the unpacking, so the extra
 * commands only ever told you a detail of how the file was stored. What
 * survives of them is one column in `progs`.
 *
 * Unpacking costs heap, which is exactly the trade being made: disk and
 * idle memory are free, and you pay only for the programs you actually run.
 *
 * The store itself owns nothing. Every entry points at data that is already
 * resident -- kernel rodata or the archive behind it -- so an empty store
 * and a full one cost the same eight bytes per slot.
 */

#include "pico.h"

static store_ent_t entries[STORE_MAX];
static int nent;

void store_init(void)
{
    nent = 0;
}

int store_add(const char *name, const u8 *data, u32 packed, u32 orig)
{
    if (nent >= STORE_MAX) return -1;
    if (!name || !name[0]) return -1;

    /* A later entry with the same name wins -- that is how a program you
     * add with addapp.py shadows one that shipped in the kernel. */
    store_ent_t *e = NULL;
    for (int i = 0; i < nent; i++)
        if (strcmp(entries[i].name, name) == 0) { e = &entries[i]; break; }
    if (!e) e = &entries[nent++];

    strncpy(e->name, name, FS_NAMELEN - 1);
    e->name[FS_NAMELEN - 1] = 0;
    e->data   = data;
    e->packed = packed;
    e->orig   = orig;
    return 0;
}

int store_count(void) { return nent; }

const store_ent_t *store_at(int i)
{
    return (i >= 0 && i < nent) ? &entries[i] : NULL;
}

const store_ent_t *store_find(const char *name)
{
    for (int i = 0; i < nent; i++)
        if (strcmp(entries[i].name, name) == 0) return &entries[i];
    return NULL;
}

/* How much smaller the store made this system. Reported by `res`. */
u32 store_packed_bytes(void)
{
    u32 n = 0;
    for (int i = 0; i < nent; i++) n += entries[i].packed;
    return n;
}

u32 store_orig_bytes(void)
{
    u32 n = 0;
    for (int i = 0; i < nent; i++) n += entries[i].orig;
    return n;
}

/* Unpack one entry into the ramdisk.
 *
 * Returns the size on success, or a negative STORE_E* code. Allocating the
 * exact original size and handing that buffer to the filesystem means the
 * unpacked file costs nothing beyond itself -- no rounding up to a power of
 * two the way a series of fs_write calls would. */
int store_extract(const char *name)
{
    const store_ent_t *e = store_find(name);
    if (!e) return STORE_ENOENT;

    file_t *have = fs_find(name);
    if (have && have->size) return STORE_EEXIST;

    u8 *buf = (u8 *)kmalloc(e->orig ? e->orig : 1);
    if (!buf) return STORE_ENOMEM;

    if (e->packed == e->orig) {
        /* the packer refused to make this one smaller, so it went in raw */
        memcpy(buf, e->data, e->orig);
    } else {
        int n = unpack(e->data, e->packed, buf, e->orig);
        if (n < 0 || (u32)n != e->orig) { kfree(buf); return STORE_EDATA; }
    }

    file_t *f = fs_create(name);
    if (!f) { kfree(buf); return STORE_EFULL; }

    if (f->data && f->cap) kfree(f->data);
    f->data = (char *)buf;
    f->size = e->orig;
    f->cap  = e->orig;          /* the filesystem owns it now */
    return (int)e->orig;
}
