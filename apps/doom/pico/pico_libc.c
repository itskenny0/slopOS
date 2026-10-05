/* pico_libc.c -- the C library DOOM thinks it is running on.
 *
 * DOOM was written against a hosted libc. PicoOS gives a program no libc
 * at all, only the kernel api table (see apps/picoapp.h), so this file
 * reimplements every library function the game calls: memory, strings,
 * character classes, numbers, a tiny printf, a sscanf that knows the five
 * conversions DOOM uses, and buffered files on top of the ramdisk.
 *
 * What is deliberately missing: locales, threads, signals, 64-bit division
 * helpers (a 386 has none to call), and anything DOOM never touches. If a
 * future source file wants more, the linker will say its name.
 *
 * This file is part of the PicoOS DOOM port, GPL-2.0-or-later, like DOOM.
 */

#include "picoapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* errno                                                              */
/* ------------------------------------------------------------------ */

int errno;

/* ------------------------------------------------------------------ */
/* memory: the kernel hands out raw blocks; a size word in front of    */
/* every block is what makes realloc possible                         */
/* ------------------------------------------------------------------ */

void *malloc(size_t n)
{
    size_t *p = api->malloc(n + sizeof(size_t));
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    *p = n;
    return (void *)(p + 1);
}

void free(void *p)
{
    if (p)
        api->free((size_t *)p - 1);
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = malloc(total ? total : 1);
    if (p && total)
        memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    size_t old;
    void *q;

    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    old = *((size_t *)p - 1);
    q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

/* ------------------------------------------------------------------ */
/* strings (memset and memcpy live in crt0.c)                         */
/* ------------------------------------------------------------------ */

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = d;
    const unsigned char *ss = s;

    if (dd == ss || !n)
        return d;
    if (dd < ss) {
        while (n--)
            *dd++ = *ss++;
    } else {
        dd += n;
        ss += n;
        while (n--)
            *--dd = *--ss;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;

    while (n--) {
        if (*p != *q)
            return *p - *q;
        p++;
        q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s)
{
    char *r = d;

    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;

    while (n && *s) {
        *d++ = *s++;
        n--;
    }
    while (n--) {
        *d++ = 0;
        /* n counts down below, keeps compilers honest */
        if (!n)
            break;
    }
    return r;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);

    while (n-- && *s)
        *p++ = *s++;
    *p = 0;
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    if (!n)
        return 0;
    while (--n && *a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c) {
        if (!*s)
            return NULL;
        s++;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    do {
        if (*s == (char)c)
            last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);

    if (!nl)
        return (char *)h;
    while (*h) {
        if (*h == *n && !strncmp(h, n, nl))
            return (char *)h;
        h++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);

    if (d)
        memcpy(d, s, n);
    return d;
}

size_t strspn(const char *s, const char *acc)
{
    const char *p = s;

    while (*p && strchr(acc, *p))
        p++;
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *rej)
{
    const char *p = s;

    while (*p && !strchr(rej, *p))
        p++;
    return (size_t)(p - s);
}

char *strerror(int e)
{
    switch (e) {
    case 0:      return "Success";
    case ENOENT: return "No such file or directory";
    case ENOMEM: return "Out of memory";
    case EINVAL: return "Invalid argument";
    case EACCES: return "Permission denied";
    case EIO:    return "Input/output error";
    default:     return "Unknown error";
    }
}

static int ci_diff(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z')
        a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
        b += 'a' - 'A';
    return (int)a - (int)b;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && !ci_diff((unsigned char)*a, (unsigned char)*b)) {
        a++;
        b++;
    }
    return ci_diff((unsigned char)*a, (unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    int d = 0;

    while (n-- && *a && !(d = ci_diff((unsigned char)*a, (unsigned char)*b))) {
        a++;
        b++;
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* character classes: ASCII only, plain functions                      */
/* ------------------------------------------------------------------ */

int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum(int c) { return isalpha(c) || (c >= '0' && c <= '9'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c <= 0x7E; }
int isgraph(int c) { return c > 0x20 && c <= 0x7E; }
int iscntrl(int c) { return c < 0x20 || c == 0x7F; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    unsigned long v = 0;
    int neg = 0, any = 0;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') {
                base = 16;
                p += 2;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'z')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z')
            d = *p - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
    }
    if (end)
        *end = (char *)(any ? p : s);
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

double strtod(const char *s, char **end)
{
    const char *p = s;
    double v = 0.0, frac = 0.0, div = 1.0;
    int neg = 0, exps = 1, expn = 0;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    while (isdigit((unsigned char)*p))
        v = v * 10.0 + (*p++ - '0');
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            frac = frac * 10.0 + (*p++ - '0');
            div *= 10.0;
        }
        v += frac / div;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '-') {
            exps = -1;
            p++;
        } else if (*p == '+') {
            p++;
        }
        while (isdigit((unsigned char)*p))
            expn = expn * 10 + (*p++ - '0');
        while (expn-- > 0)
            v = exps > 0 ? v * 10.0 : v / 10.0;
    }
    if (end)
        *end = (char *)p;
    return neg ? -v : v;
}

int atoi(const char *s)        { return (int)strtol(s, NULL, 10); }
long atol(const char *s)       { return strtol(s, NULL, 10); }
double atof(const char *s)     { return strtod(s, NULL); }
int abs(int x)                 { return x < 0 ? -x : x; }
long labs(long x)              { return x < 0 ? -x : x; }

static unsigned rseed = 1;

int rand(void)
{
    rseed = rseed * 1103515245u + 12345u;
    return (int)((rseed >> 16) & 0x7FFF);
}

void srand(unsigned seed)
{
    rseed = seed ? seed : 1;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;                 /* PicoOS has no environment */
}

int system(const char *cmd)
{
    (void)cmd;
    return -1;                   /* ... and no shell to run it in */
}

int atexit(void (*fn)(void))
{
    (void)fn;
    return -1;                   /* DOOM keeps its own exit list */
}

void exit(int code)
{
    api->gfx_mode(0);            /* an error mid-frame must still print */
    api->exit(code);
    for (;;)
        ;
}

void abort(void)
{
    api->puts("abort() called\n");
    exit(3);
}

int open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return -1;
}

/* the whole math library, as far as DOOM is concerned */
double fabs(double x)
{
    return x < 0 ? -x : x;
}

/* ------------------------------------------------------------------ */
/* files: a whole file is one buffer; reads map the ramdisk in place,  */
/* writes collect in memory and land on fclose                         */
/* ------------------------------------------------------------------ */

struct PFILE {
    char    *buf;                /* bytes (mapped or owned) */
    unsigned size;               /* valid bytes */
    unsigned cap;                /* allocated (owned only) */
    unsigned pos;                /* cursor */
    int      writing;            /* opened for write */
    int      owned;               /* free buf on fclose */
    int      eof;
    int      ungot;              /* pushed-back char, or -1 */
    int      cons;               /* 1 stdout, 2 stderr, 3 stdin, 0 file */
    char     name[16];           /* ramdisk name for writeback */
};

static FILE con_out = { 0, 0, 0, 0, 0, 0, 0, -1, 1, "" };
static FILE con_err = { 0, 0, 0, 0, 0, 0, 0, -1, 2, "" };
static FILE con_in  = { 0, 0, 0, 0, 0, 0, 0, -1, 3, "" };

FILE *stdin  = &con_in;
FILE *stdout = &con_out;
FILE *stderr = &con_err;

/* names remove()d still sit in the ramdisk (files are never really
 * deleted), so remember them and refuse to open them again */
#define MAXDEL 16
static char delnames[MAXDEL][16];
static int ndel;

static int is_deleted(const char *name)
{
    int i;

    for (i = 0; i < ndel; i++)
        if (!strcmp(delnames[i], name))
            return 1;
    return 0;
}

static void undelete(const char *name)
{
    int i;

    for (i = 0; i < ndel; i++) {
        if (!strcmp(delnames[i], name)) {
            if (i < --ndel)
                strcpy(delnames[i], delnames[ndel]);
            return;
        }
    }
}

static const char *try_variants(const char *name, unsigned *size)
{
    /* DOOM asks for "doom1.wad", the disk holds "DOOM1.WAD": try the
     * name as given, then all upper, then all lower case */
    char alt[16];
    const char *d;
    unsigned i, sz = 0;

    d = api->fsmap(name, &sz);
    if (d) {
        *size = sz;
        return d;
    }
    for (i = 0; i < 15 && name[i]; i++)
        alt[i] = (char)toupper((unsigned char)name[i]);
    alt[i] = 0;
    d = api->fsmap(alt, &sz);
    if (d) {
        *size = sz;
        return d;
    }
    for (i = 0; i < 15 && name[i]; i++)
        alt[i] = (char)tolower((unsigned char)name[i]);
    alt[i] = 0;
    d = api->fsmap(alt, &sz);
    if (d)
        *size = sz;
    return d;
}

FILE *fopen(const char *path, const char *mode)
{
    const char *base = path, *p;
    FILE *f;
    const char *data;
    unsigned sz = 0;
    int writing, trunc;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;        /* the ramdisk is flat: strip directories */

    writing = strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL ||
              strchr(mode, '+') != NULL;
    trunc = strchr(mode, 'w') != NULL;

    f = malloc(sizeof(*f));
    if (!f)
        return NULL;
    memset(f, 0, sizeof(*f));
    f->ungot = -1;
    strncpy(f->name, base, 15);

    if (!writing) {
        if (is_deleted(f->name)) {
            free(f);
            return NULL;
        }
        data = try_variants(f->name, &sz);
        if (!data) {
            errno = ENOENT;
            free(f);
            return NULL;
        }
        /* zero-copy: read straight from the ramdisk */
        f->buf = (char *)data;
        f->size = sz;
        f->cap = sz;
        return f;
    }

    /* writing: start from the old content unless truncating */
    undelete(f->name);
    data = try_variants(f->name, &sz);
    if (trunc || !data)
        sz = 0;
    f->cap = sz + 4096;
    f->buf = malloc(f->cap);
    if (!f->buf) {
        free(f);
        return NULL;
    }
    if (sz)
        memcpy(f->buf, data, sz);
    f->size = sz;
    f->pos = strchr(mode, 'a') ? sz : 0;
    f->writing = 1;
    f->owned = 1;
    return f;
}

int fclose(FILE *f)
{
    if (!f)
        return EOF;
    if (f->cons)
        return 0;
    if (f->writing)
        api->fs_write(f->name, f->buf, f->size);
    if (f->owned)
        free(f->buf);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t want, avail;

    if (!f || !size || f->cons == 3 || f->cons == 1 || f->cons == 2)
        return 0;
    if (f->ungot >= 0 && size == 1 && nmemb >= 1) {
        ((unsigned char *)ptr)[0] = (unsigned char)f->ungot;
        f->ungot = -1;
        if (nmemb == 1)
            return 1;
        ptr = (unsigned char *)ptr + 1;
        nmemb--;
        /* one item down; fall through for the rest */
        return 1 + fread(ptr, size, nmemb, f);
    }
    want = size * nmemb;
    avail = f->pos < f->size ? f->size - f->pos : 0;
    if (want > avail)
        want = avail;
    if (want) {
        memcpy(ptr, f->buf + f->pos, want);
        f->pos += want;
    }
    if (f->pos >= f->size)
        f->eof = 1;
    return want / size;
}

static int fgrow(FILE *f, unsigned need)
{
    unsigned cap = f->cap ? f->cap : 64;
    char *nb;

    if (need <= f->cap)
        return 1;
    while (cap < need)
        cap *= 2;
    nb = realloc(f->buf, cap);
    if (!nb)
        return 0;
    f->buf = nb;
    f->cap = cap;
    return 1;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t want = size * nmemb, i;

    if (!f || !size)
        return 0;
    if (f->cons == 1 || f->cons == 2) {
        const unsigned char *p = ptr;
        for (i = 0; i < want; i++)
            api->putc(p[i]);
        return nmemb;
    }
    if (f->cons == 3)
        return 0;
    if (!fgrow(f, f->pos + want))
        return 0;
    if (f->pos > f->size)        /* seeking past the end leaves zeroes */
        memset(f->buf + f->size, 0, f->pos - f->size);
    memcpy(f->buf + f->pos, ptr, want);
    f->pos += want;
    if (f->pos > f->size)
        f->size = f->pos;
    return nmemb;
}

int fseek(FILE *f, long offset, int whence)
{
    long np;

    if (!f || f->cons)
        return -1;
    if (whence == SEEK_SET)
        np = offset;
    else if (whence == SEEK_CUR)
        np = (long)f->pos + offset;
    else if (whence == SEEK_END)
        np = (long)f->size + offset;
    else
        return -1;
    if (np < 0)
        return -1;
    f->pos = (unsigned)np;
    f->ungot = -1;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f)
{
    if (!f || f->cons)
        return -1L;
    return (long)f->pos;
}

void rewind(FILE *f)
{
    if (f && !f->cons) {
        f->pos = 0;
        f->ungot = -1;
        f->eof = 0;
    }
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

int feof(FILE *f)
{
    return f ? f->eof : 1;
}

int ferror(FILE *f)
{
    (void)f;
    return 0;
}

void clearerr(FILE *f)
{
    if (f)
        f->eof = 0;
}

int fgetc(FILE *f)
{
    unsigned char c;

    if (!f)
        return EOF;
    if (f->ungot >= 0) {
        c = (unsigned char)f->ungot;
        f->ungot = -1;
        return c;
    }
    if (f->cons == 3)
        return api->getkey() & 0xFF;
    if (f->cons)
        return EOF;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

int getc(FILE *f)
{
    return fgetc(f);
}

int getchar(void)
{
    return fgetc(stdin);
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0, c;

    if (n <= 1 || !f)
        return NULL;
    while (i < n - 1 && (c = fgetc(f)) != EOF) {
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!i)
        return NULL;
    s[i] = 0;
    return s;
}

int ungetc(int c, FILE *f)
{
    if (!f || c == EOF)
        return EOF;
    f->ungot = c & 0xFF;
    f->eof = 0;
    return c & 0xFF;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;

    if (!f)
        return EOF;
    if (f->cons == 1 || f->cons == 2) {
        api->putc(ch);
        return ch;
    }
    if (f->cons == 3)
        return EOF;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *f)
{
    size_t n;

    if (!f)
        return EOF;
    n = strlen(s);
    if (f->cons == 1 || f->cons == 2) {
        api->puts(s);
        return 0;
    }
    if (f->cons == 3)
        return EOF;
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
    api->puts(s);
    api->putc('\n');
    return 0;
}

int remove(const char *path)
{
    const char *base = path, *p;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    if (ndel < MAXDEL) {
        strncpy(delnames[ndel], base, 15);
        delnames[ndel][15] = 0;
        ndel++;
    }
    return 0;
}

int rename(const char *oldp, const char *newp)
{
    FILE *f;
    char tmp[16], *all;
    long sz;
    int rc = -1;

    /* ramdisk names are 15 chars; anything longer cannot exist */
    strncpy(tmp, newp, 15);
    tmp[15] = 0;

    f = fopen(oldp, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    all = malloc(sz > 0 ? (size_t)sz : 1);
    if (all) {
        if (sz > 0)
            fread(all, 1, (size_t)sz, f);
        fclose(f);
        f = fopen(tmp, "wb");
        if (f) {
            if (sz > 0)
                fwrite(all, 1, (size_t)sz, f);
            fclose(f);
            remove(oldp);
            rc = 0;
        }
        free(all);
    } else {
        fclose(f);
    }
    return rc;
}

int mkdir(const char *path, ...)
{
    (void)path;
    return 0;                    /* every directory already exists */
}

/* ------------------------------------------------------------------ */
/* printf: %d %i %u %o %x %X %c %s %p %f and %%, with width, precision, */
/* flags and length modifiers. No 64-bit math anywhere: on a 386 the   */
/* compiler would call helpers that do not exist.                      */
/* ------------------------------------------------------------------ */

struct emit {
    char  *buf;
    size_t cap;                  /* 0 means count only */
    size_t len;
};

static void emit_ch(struct emit *e, char c)
{
    if (!e->cap || e->len + 1 < e->cap)
        e->buf[e->len] = c;
    e->len++;
}

static void emit_pad(struct emit *e, char c, int n)
{
    while (n-- > 0)
        emit_ch(e, c);
}

/* an unsigned 32-bit value in any base, into a backwards buffer */
static int u32_to_str(unsigned v, int base, int upper, char *tmp)
{
    static const char dig_lo[] = "0123456789abcdef";
    static const char dig_up[] = "0123456789ABCDEF";
    const char *dig = upper ? dig_up : dig_lo;
    int n = 0;

    if (!v) {
        tmp[n++] = '0';
    } else {
        while (v) {
            tmp[n++] = dig[v % (unsigned)base];
            v /= (unsigned)base;
        }
    }
    return n;
}

static int pico_vformat(struct emit *e, const char *f, va_list ap)
{
    while (*f) {
        int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
        int width = -1, prec = -1;
        int is_signed = 0, spec;
        char tmp[32];
        int n = 0, i;

        if (*f != '%') {
            emit_ch(e, *f++);
            continue;
        }
        f++;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            f++;
        } else if (isdigit((unsigned char)*f)) {
            width = 0;
            while (isdigit((unsigned char)*f))
                width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            f++;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                prec = 0;
                while (isdigit((unsigned char)*f))
                    prec = prec * 10 + (*f++ - '0');
            }
        }
        /* length modifiers: l is 32 bits here like int, so swallow all */
        while (*f == 'h' || *f == 'l' || *f == 'L' || *f == 'j' ||
               *f == 'z' || *f == 't')
            f++;

        spec = *f ? *f++ : 0;
        if (spec == 'd' || spec == 'i') {
            int v = va_arg(ap, int);
            unsigned u;
            int neg = v < 0;
            u = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
            n = u32_to_str(u, 10, 0, tmp);
            is_signed = 1;
            /* sign first, then zero padding (precision beats '0') */
            {
                int digits = prec > n ? prec : n;
                int signch = neg ? '-' : (plus ? '+' : (space ? ' ' : 0));
                int total = digits + (signch ? 1 : 0);
                int pad = width > total ? width - total : 0;
                if (!left && !(zero && prec < 0))
                    emit_pad(e, ' ', pad);
                if (signch)
                    emit_ch(e, (char)signch);
                if (!left && zero && prec < 0)
                    emit_pad(e, '0', pad);
                emit_pad(e, '0', digits - n);
                for (i = n - 1; i >= 0; i--)
                    emit_ch(e, tmp[i]);
                if (left)
                    emit_pad(e, ' ', pad);
            }
            continue;
        }
        if (spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X') {
            unsigned u = va_arg(ap, unsigned);
            int base = spec == 'o' ? 8 : (spec == 'u' ? 10 : 16);
            const char *pre = "";
            n = u32_to_str(u, base, spec == 'X', tmp);
            if (alt && u) {
                if (spec == 'o')
                    pre = "0";
                else if (spec == 'x')
                    pre = "0x";
                else if (spec == 'X')
                    pre = "0X";
            }
            {
                int digits = prec > n ? prec : n;
                int total = digits + (int)strlen(pre);
                int pad = width > total ? width - total : 0;
                if (!left && !(zero && prec < 0))
                    emit_pad(e, ' ', pad);
                while (*pre)
                    emit_ch(e, *pre++);
                if (!left && zero && prec < 0)
                    emit_pad(e, '0', pad);
                emit_pad(e, '0', digits - n);
                for (i = n - 1; i >= 0; i--)
                    emit_ch(e, tmp[i]);
                if (left)
                    emit_pad(e, ' ', pad);
            }
            continue;
        }
        if (spec == 'c') {
            char ch = (char)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left)
                emit_pad(e, ' ', pad);
            emit_ch(e, ch);
            if (left)
                emit_pad(e, ' ', pad);
            continue;
        }
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            size_t sl;
            int pad;
            if (!s)
                s = "(null)";
            sl = strlen(s);
            if (prec >= 0 && (size_t)prec < sl)
                sl = (size_t)prec;
            pad = width > (int)sl ? width - (int)sl : 0;
            if (!left)
                emit_pad(e, ' ', pad);
            for (i = 0; (size_t)i < sl; i++)
                emit_ch(e, s[i]);
            if (left)
                emit_pad(e, ' ', pad);
            continue;
        }
        if (spec == 'p') {
            unsigned u = (unsigned)va_arg(ap, void *);
            int pad;
            n = u32_to_str(u, 16, 0, tmp);
            pad = width > n + 2 ? width - n - 2 : 0;
            if (!left)
                emit_pad(e, ' ', pad);
            emit_ch(e, '0');
            emit_ch(e, 'x');
            for (i = n - 1; i >= 0; i--)
                emit_ch(e, tmp[i]);
            if (left)
                emit_pad(e, ' ', pad);
            continue;
        }
        if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
            spec == 'g' || spec == 'G') {
            /* plain %f only; the game never asks for e/g */
            double v = va_arg(ap, double);
            long ip;
            double frac;
            int d, neg = 0;
            if (prec < 0)
                prec = 6;
            if (v < 0) {
                neg = 1;
                v = -v;
            }
            ip = (long)v;
            frac = v - (double)ip;
            {
                int signch = neg ? '-' : (plus ? '+' : (space ? ' ' : 0));
                n = u32_to_str((unsigned)(ip < 0 ? -ip : ip), 10, 0, tmp);
                {
                    int total = n + 1 + prec + (signch ? 1 : 0);
                    int pad = width > total ? width - total : 0;
                    if (!left)
                        emit_pad(e, zero ? '0' : ' ', pad);
                    if (signch)
                        emit_ch(e, (char)signch);
                    for (i = n - 1; i >= 0; i--)
                        emit_ch(e, tmp[i]);
                    emit_ch(e, '.');
                    for (d = 0; d < prec; d++) {
                        frac *= 10.0;
                        i = (int)frac;
                        frac -= i;
                        emit_ch(e, (char)('0' + i));
                    }
                    if (left)
                        emit_pad(e, ' ', pad);
                }
            }
            continue;
        }
        if (spec == 'n') {
            int *p = va_arg(ap, int *);
            if (p)
                *p = (int)e->len;
            continue;
        }
        if (spec == '%') {
            emit_ch(e, '%');
            continue;
        }
        if (!spec)
            break;
        /* unknown conversion: print it literally rather than gabble */
        emit_ch(e, '%');
        emit_ch(e, (char)spec);
        (void)is_signed;
    }
    return (int)e->len;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    struct emit e;
    int len;

    e.buf = s;
    e.cap = n;
    e.len = 0;
    len = pico_vformat(&e, fmt, ap);
    if (n) {
        if (e.len >= n)
            s[n - 1] = 0;
        else
            s[e.len] = 0;
    }
    return len;
}

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *s, const char *fmt, va_list ap)
{
    struct emit e;
    int len;

    e.buf = s;
    e.cap = 0;                   /* unbounded, like the real one */
    e.len = 0;
    len = pico_vformat(&e, fmt, ap);
    s[len] = 0;
    return len;
}

int sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsprintf(s, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char tmp[4096];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);

    if (!f)
        return -1;
    if (f->cons == 1 || f->cons == 2) {
        api->puts(tmp);
        return len;
    }
    if (f->cons == 3)
        return -1;
    fwrite(tmp, 1, strlen(tmp), f);
    return len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* sscanf: %d %i %u %o %x %s %c %n, exactly what DOOM feeds it          */
/* ------------------------------------------------------------------ */

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    const char *p = s;
    int count = 0;

    va_start(ap, fmt);
    while (*fmt) {
        int nosave = 0, width = -1;

        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt))
                fmt++;
            while (isspace((unsigned char)*p))
                p++;
            continue;
        }
        if (*fmt != '%') {
            if (*fmt++ != *p++)
                break;
            continue;
        }
        fmt++;
        if (*fmt == '*') {
            nosave = 1;
            fmt++;
        }
        if (isdigit((unsigned char)*fmt)) {
            width = 0;
            while (isdigit((unsigned char)*fmt))
                width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'L')
            fmt++;
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'o' ||
            *fmt == 'x' || *fmt == 'X') {
            char spec = *fmt++;
            int base = spec == 'o' ? 8 : (spec == 'x' || spec == 'X' ? 16 : 10);
            char *e;
            long v;
            if (spec == 'i')
                base = 0;
            while (isspace((unsigned char)*p))
                p++;
            v = strtol(p, &e, base);
            if (e == p)
                break;
            p = e;
            if (!nosave) {
                *va_arg(ap, int *) = (int)v;
                count++;
            }
        } else if (*fmt == 's') {
            char *d;
            int n = 0;
            fmt++;
            while (isspace((unsigned char)*p))
                p++;
            d = nosave ? NULL : va_arg(ap, char *);
            while (*p && !isspace((unsigned char)*p) &&
                   (width < 0 || n < width)) {
                if (d)
                    d[n] = *p;
                n++;
                p++;
            }
            if (!n)
                break;
            if (d)
                d[n] = 0;
            if (!nosave)
                count++;
        } else if (*fmt == 'c') {
            char *d;
            int n = width < 0 ? 1 : width, i;
            fmt++;
            d = nosave ? NULL : va_arg(ap, char *);
            for (i = 0; i < n; i++) {
                if (!*p)
                    break;
                if (d)
                    d[i] = *p;
                p++;
            }
            if (i < n)
                break;
            if (!nosave)
                count++;
        } else if (*fmt == 'n') {
            fmt++;
            if (!nosave)
                *va_arg(ap, int *) = (int)(p - s);
        } else if (*fmt == '%') {
            fmt++;
            if (*p++ != '%')
                break;
        } else {
            break;
        }
    }
    va_end(ap);
    return count;
}
