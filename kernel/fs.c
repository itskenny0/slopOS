#include "pico.h"

/* A RAM filesystem.
 *
 * Files normally live in kmalloc'd buffers that grow when needed. A file can
 * also be *mounted in place*: the four programs that ship with PicoOS are
 * already sitting in the kernel image as const arrays, so the filesystem
 * just points at them instead of spending 7 KB of heap on a second copy.
 * Those entries have cap == 0. The moment anything writes to one, it gets
 * copied to the heap first, so read-only storage is invisible to callers.
 */

static file_t files[FS_MAXFILES];

void fs_init(void)
{
    for (int i = 0; i < FS_MAXFILES; i++) {
        files[i].used = 0;
        files[i].data = NULL;
        files[i].size = files[i].cap = 0;
        files[i].name[0] = 0;
    }
}

/* Register a file that already exists in memory somewhere permanent.
 * Costs one directory slot and not a single byte of heap. */
int fs_mount(const char *name, const char *data, u32 len)
{
    file_t *f = fs_create(name);
    if (!f) return -1;
    f->data = (char *)data;
    f->size = len;
    f->cap  = 0;                 /* 0 means: not ours, do not free or grow */
    return (int)len;
}

file_t *fs_find(const char *name)
{
    for (int i = 0; i < FS_MAXFILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return &files[i];
    return NULL;
}

file_t *fs_create(const char *name)
{
    file_t *f = fs_find(name);
    if (f) return f;
    for (int i = 0; i < FS_MAXFILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            strncpy(files[i].name, name, FS_NAMELEN - 1);
            files[i].name[FS_NAMELEN - 1] = 0;
            files[i].data = NULL;
            files[i].size = files[i].cap = 0;
            return &files[i];
        }
    }
    return NULL;    /* directory full */
}

static int ensure_cap(file_t *f, u32 need)
{
    if (f->cap >= need) return 1;
    u32 ncap = f->cap ? f->cap : 64;
    while (ncap < need) ncap *= 2;
    char *nd = (char *)kmalloc(ncap);
    if (!nd) return 0;
    if (f->data) {
        u32 keep = f->size < ncap ? f->size : ncap;
        memcpy(nd, f->data, keep);
        if (f->cap) kfree(f->data);      /* cap 0 = mounted, not ours */
    }
    f->data = nd;
    f->cap  = ncap;
    return 1;
}

int fs_write(const char *name, const char *data, u32 len)
{
    file_t *f = fs_create(name);
    if (!f) return -1;
    if (!ensure_cap(f, len + 1)) return -2;
    memcpy(f->data, data, len);
    f->data[len] = 0;
    f->size = len;
    return (int)len;
}

int fs_append(const char *name, const char *data, u32 len)
{
    file_t *f = fs_create(name);
    if (!f) return -1;
    if (!ensure_cap(f, f->size + len + 1)) return -2;
    memcpy(f->data + f->size, data, len);
    f->size += len;
    f->data[f->size] = 0;
    return (int)f->size;
}

int fs_delete(const char *name)
{
    file_t *f = fs_find(name);
    if (!f) return -1;
    if (f->data && f->cap) kfree(f->data);
    f->data = NULL;
    f->used = 0;
    f->size = f->cap = 0;
    f->name[0] = 0;
    return 0;
}

/* A direct pointer to the file's bytes, for readers that want megabytes
 * without copying them (DOOM's WAD). The pointer stays valid until the
 * file is written, appended or deleted. */
const char *fs_mapdata(const char *name, u32 *size)
{
    file_t *f = fs_find(name);
    if (!f || !f->data) return NULL;
    if (size) *size = f->size;
    return f->data;
}

int fs_count(void)
{
    int n = 0;
    for (int i = 0; i < FS_MAXFILES; i++) if (files[i].used) n++;
    return n;
}

file_t *fs_at(int i)
{
    int n = 0;
    for (int j = 0; j < FS_MAXFILES; j++) {
        if (files[j].used) {
            if (n == i) return &files[j];
            n++;
        }
    }
    return NULL;
}
