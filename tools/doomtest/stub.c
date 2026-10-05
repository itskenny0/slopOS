/* Host test stub: emulates just enough of the PicoOS kernel api to run the
 * real doom.pico code (Doom sources + pico_libc, unmodified) on Linux. No
 * libc is used: raw x86-64 syscalls only. Anything Doom calls that is not
 * emulated here is a NULL pointer and crashes loudly, which is the point.
 *
 *   hostfs/DOOM1.WAD  the game data, preloaded like the kernel ramdisk
 *   out/frame_NNNN.ppm  every 50th frame, to eyeball the renderer
 *   out/<name>      anything Doom writes (default.cfg at exit)
 */

#include "picoapp.h"

/* ---------------- syscalls ---------------- */

static long sc1(long n, long a)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a)
                     : "rcx", "r11", "memory", "cc");
    return r;
}

static long sc3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return r;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_nanosleep 35
#define SYS_exit_group 231
#define SYS_clock_gettime 228

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 64
#define O_TRUNC 512

static unsigned long h_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (unsigned long)(p - s);
}

static void h_write_str(const char *s)
{
    sc3(SYS_write, 1, (long)s, (long)h_strlen(s));
}

static void h_write_bytes(const void *p, unsigned long n)
{
    sc3(SYS_write, 1, (long)p, (long)n);
}

/* ---------------- memset/memcpy for Doom (global; everything else here is
 * static so it cannot collide with pico_libc's symbols) ---------------- */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

/* ---------------- allocator: first fit over a fixed pool ---------------- */

#define POOLSIZE (128u * 1024u * 1024u)

static unsigned char pool[POOLSIZE] __attribute__((aligned(16)));

struct blk {
    size_t size;                 /* usable bytes after this header */
    struct blk *next;
};

static struct blk *freelist;

static void pool_init(void)
{
    freelist = (struct blk *)pool;
    freelist->size = POOLSIZE - sizeof(struct blk);
    freelist->next = 0;
}

static void *h_malloc(size_t n)
{
    struct blk **pp;
    struct blk *b, *rest;
    size_t need;

    if (!n)
        n = 1;
    n = (n + 15) & ~(size_t)15;
    need = n + sizeof(struct blk);
    for (pp = &freelist; (b = *pp) != 0; pp = &b->next) {
        if (b->size + sizeof(struct blk) < need)
            continue;
        if (b->size >= need + 32) {
            rest = (struct blk *)((unsigned char *)(b + 1) + n);
            rest->size = b->size - n - sizeof(struct blk);
            rest->next = b->next;
            b->size = n;
            b->next = rest;
        }
        *pp = b->next;
        return (void *)(b + 1);
    }
    h_write_str("stub: OUT OF POOL MEMORY\n");
    sc1(SYS_exit_group, 99);
    return 0;
}

static void h_free(void *p)
{
    struct blk *b, **pp;

    if (!p)
        return;
    b = (struct blk *)p - 1;
    /* insert sorted by address, coalesce neighbours */
    for (pp = &freelist; *pp && *pp < b; pp = &(*pp)->next)
        ;
    b->next = *pp;
    *pp = b;
    if (b->next == (struct blk *)((unsigned char *)(b + 1) + b->size)) {
        b->size += sizeof(struct blk) + b->next->size;
        b->next = b->next->next;
    }
    /* coalesce with predecessor: rescan (frees are rare) */
    {
        struct blk *q = freelist;
        while (q && q->next) {
            if (q->next ==
                (struct blk *)((unsigned char *)(q + 1) + q->size)) {
                q->size += sizeof(struct blk) + q->next->size;
                q->next = q->next->next;
            } else {
                q = q->next;
            }
        }
    }
}

/* ---------------- preloaded files (the fake ramdisk) ---------------- */

#define MAXHOSTF 8

static struct {
    char name[16];
    unsigned char *data;
    u32 size;
    int used;
} hostf[MAXHOSTF];

static int h_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void h_strcpy(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}

static void preload(const char *name, const char *path)
{
    long fd, i, total = 0, cap = 1 << 20;
    unsigned char *buf = h_malloc(cap);
    int slot;

    fd = sc3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        h_write_str("stub: cannot open ");
        h_write_str(path);
        h_write_str("\n");
        sc1(SYS_exit_group, 98);
    }
    for (;;) {
        long r;
        if (total == cap) {
            unsigned char *nb = h_malloc(cap * 2);
            memcpy(nb, buf, cap);
            h_free(buf);
            buf = nb;
            cap *= 2;
        }
        r = sc3(SYS_read, fd, (long)(buf + total), cap - total);
        if (r <= 0)
            break;
        total += r;
    }
    sc1(SYS_close, fd);
    for (slot = 0; slot < MAXHOSTF && hostf[slot].used; slot++)
        ;
    hostf[slot].used = 1;
    h_strcpy(hostf[slot].name, name);
    hostf[slot].data = buf;
    hostf[slot].size = (u32)total;
    (void)i;
}

/* ---------------- test counters ---------------- */

static unsigned long frames;
static unsigned long palsets;
static unsigned long fswrites;
static unsigned char curpal[768];
static int havepal;
static int gfxon;

static void write_ppm(unsigned n, const u8 *frame)
{
    /* tiny integer writer, no printf here */
    char path[64], head[64];
    char *p = path;
    const char *pre = "out/frame_";
    unsigned long fd, i, v;
    char digits[16];
    int nd = 0;

    while (*pre)
        *p++ = *pre++;
    v = n;
    do {
        digits[nd++] = '0' + (v % 10);
        v /= 10;
    } while (v);
    while (nd < 4)
        digits[nd++] = '0';
    while (nd--)
        *p++ = digits[nd];
    *p++ = '.';
    *p++ = 'p';
    *p++ = 'p';
    *p++ = 'm';
    *p = 0;

    p = head;
    {
        const char *h = "P6\n320 200\n255\n";
        while (*h)
            *p++ = *h++;
    }
    fd = sc3(SYS_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if ((long)fd < 0)
        return;
    sc3(SYS_write, fd, (long)head, (long)(p - head));
    for (i = 0; i < 320u * 200u; i++) {
        unsigned char px[3];
        px[0] = curpal[frame[i] * 3];
        px[1] = curpal[frame[i] * 3 + 1];
        px[2] = curpal[frame[i] * 3 + 2];
        sc3(SYS_write, fd, (long)px, 3);
    }
    sc1(SYS_close, fd);
}

/* ---------------- the api table ---------------- */

static void h_putc(char c)
{
    h_write_bytes(&c, 1);
}

static void h_puts(const char *s)
{
    h_write_str(s);
}

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static u32 h_ms(void)
{
    struct timespec ts;
    sc3(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&ts, 0);
    return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void h_sleep(u32 ms)
{
    struct timespec ts;
    if (ms > 1000)
        ms = 1000;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    sc3(SYS_nanosleep, (long)&ts, 0, 0);
}

static unsigned h_seed = 12345;

static u32 h_rand(void)
{
    h_seed = h_seed * 1103515245u + 12345u;
    return h_seed >> 8;
}

static int h_getkey(void)
{
    return 0;                    /* timedemo needs no keys */
}

static int h_poll(void)
{
    return 0;
}

static int h_keydown(int code)
{
    (void)code;
    return 0;
}

static int h_gfx_mode(int on)
{
    gfxon = on ? 1 : 0;
    return 1;
}

static int h_gfx256(void)
{
    return 1;
}

static void h_setpal(const u8 *rgb)
{
    unsigned i;
    if (!rgb)
        return;
    for (i = 0; i < 768; i++)
        curpal[i] = rgb[i];
    havepal = 1;
    palsets++;
}

static int h_blit(const u8 *frame, int w, int h)
{
    if (!frame || w != 320 || h != 200 || !havepal) {
        h_write_str("stub: BAD BLIT\n");
        return 0;
    }
    frames++;
    if (frames % 50 == 1)
        write_ppm((unsigned)frames, frame);
    return 1;
}

static const char *h_fsmap(const char *name, u32 *size)
{
    int i;
    for (i = 0; i < MAXHOSTF; i++) {
        if (hostf[i].used && !h_strcmp(hostf[i].name, name)) {
            if (size)
                *size = hostf[i].size;
            return (const char *)hostf[i].data;
        }
    }
    return 0;
}

static int h_fs_read(const char *name, char *buf, u32 max)
{
    int i;
    for (i = 0; i < MAXHOSTF; i++) {
        if (hostf[i].used && !h_strcmp(hostf[i].name, name)) {
            u32 n = hostf[i].size < max ? hostf[i].size : max;
            memcpy(buf, hostf[i].data, n);
            return (int)n;
        }
    }
    return -1;
}

static int h_fs_write(const char *name, const char *buf, u32 len)
{
    char path[32];
    char *p = path;
    const char *pre = "out/";
    long fd, wlen = len;

    while (*pre)
        *p++ = *pre++;
    while (*name)
        *p++ = *name++;
    *p = 0;
    fd = sc3(SYS_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if ((long)fd < 0)
        return -1;
    sc3(SYS_write, fd, (long)buf, wlen);
    sc1(SYS_close, fd);
    fswrites++;
    h_write_str("stub: wrote ");
    h_write_str(path);
    h_write_str("\n");
    return 0;
}

static void h_ulong(unsigned long v)
{
    char b[24];
    int n = 0;
    if (!v)
        b[n++] = '0';
    else {
        char t[24];
        int nt = 0;
        while (v) {
            t[nt++] = '0' + (v % 10);
            v /= 10;
        }
        while (nt--)
            b[n++] = t[nt];
    }
    b[n] = 0;
    h_write_str(b);
}

static void h_exit(int code)
{
    h_write_str("\nstub: exit(");
    h_ulong((unsigned long)(code < 0 ? -code : code));
    h_write_str(code < 0 ? ") frames=" : ") frames=");
    h_ulong(frames);
    h_write_str(" palsets=");
    h_ulong(palsets);
    h_write_str(" fswrites=");
    h_ulong(fswrites);
    h_write_str(code ? " FAILED\n" : " OK\n");
    sc1(SYS_exit_group, code < 0 ? 1 : code);
    for (;;)
        ;
}

static picoapi_t the_api = {
    .version = 13,
    .putc = h_putc,
    .puts = h_puts,
    .getkey = h_getkey,
    .poll = h_poll,
    .ms = h_ms,
    .sleep = h_sleep,
    .malloc = h_malloc,
    .free = h_free,
    .rand = h_rand,
    .fs_read = h_fs_read,
    .fs_write = h_fs_write,
    .exit = h_exit,
    .gfx_mode = h_gfx_mode,
    .gfx256 = h_gfx256,
    .setpal = h_setpal,
    .blit = h_blit,
    .fsmap = h_fsmap,
    .keydown = h_keydown,
};

picoapi_t *api = &the_api;

void stub_init(void)
{
    pool_init();
    preload("DOOM1.WAD", "hostfs/DOOM1.WAD");
    h_write_str("stub: ready, WAD preloaded\n");
}
