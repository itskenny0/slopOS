#include "pico.h"

/* ---- COM1 serial, handy for debugging / headless boots ----
 *
 * Every character the system prints also goes down the serial line, which
 * is how the automated tests read the screen. The transmit loop waits for
 * the holding register to empty, with a spin count as a safety net.
 *
 * On a machine with no UART at all, that safety net was the whole cost: a
 * port that is not there reads back as either 0x00 or 0xFF depending on the
 * chipset, and on the 0x00 boards the loop spun its full hundred thousand
 * iterations for every single character printed. That is a fraction of a
 * second per character -- slow enough to watch text crawl down the screen.
 *
 * So we probe for the chip first. The scratch register at offset 7 exists
 * for exactly this: it does nothing except remember what you wrote to it.
 * Two different values have to survive the round trip, because a floating
 * bus can accidentally return one of them. */
#define COM1 0x3F8
static int have_uart;

static int uart_present(void)
{
    outb(COM1 + 7, 0xAE);
    if (inb(COM1 + 7) != 0xAE) return 0;
    outb(COM1 + 7, 0x51);
    if (inb(COM1 + 7) != 0x51) return 0;
    return 1;
}

void serial_init(void)
{
    have_uart = uart_present();
    if (!have_uart) return;

    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
void serial_putc(char c)
{
    if (!have_uart) return;
    int guard = 20000;
    while (!(inb(COM1 + 5) & 0x20) && guard--) ;
    outb(COM1, (u8)c);
}

/* Receiving. Everything the kernel prints goes out of this port as well,
 * which is how the tests read the screen -- and which is exactly what you
 * do not want while something on the other end is being spoken to in
 * commands. serial_mute() stops the echo without stopping the port, so a
 * driver can use the line and still print to the screen. */
static int serial_muted = 0;

void serial_mute(int on)    { serial_muted = on; }
int  serial_is_muted(void)  { return serial_muted; }

int serial_poll(void)
{
    if (!have_uart) return -1;
    if (!(inb(COM1 + 5) & 0x01)) return -1;      /* no character waiting */
    return inb(COM1);
}

void serial_write(const char *s)
{
    while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); }
}

/* 115200 by default; an ESP module from the bottom of a drawer may still
 * be at the 9600 its first firmware came up at */
void serial_set_baud(u32 baud)
{
    if (!have_uart) return;
    u32 div = 115200 / (baud ? baud : 115200);
    if (!div) div = 1;
    outb(COM1 + 3, 0x80);                 /* divisor latch */
    outb(COM1 + 0, (u8)(div & 0xFF));
    outb(COM1 + 1, (u8)(div >> 8));
    outb(COM1 + 3, 0x03);                 /* 8N1 again */
}

void putc(char c)
{
    /* Only the foreground task reaches the outside world -- screen and
     * serial line alike. The rest is kept and replayed on `fg`. */
    if (!task_owns_console()) { task_log(c); return; }

    if (!serial_muted) {
        if (c == '\n') serial_putc('\r');
        serial_putc(c);
    }
    con_putc(c);
}
void puts(const char *s) { while (*s) putc(*s++); }

/* ---- string / memory ---- */
void *memset(void *d, int c, size_t n)
{
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    u8 *a = (u8 *)d; const u8 *b = (const u8 *)s;
    while (n--) *a++ = *b++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(u8)*a - (int)(u8)*b : 0;
}
char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
/* Join two strings, refusing to walk off the end of the first one's
 * buffer. There is no printf in this kernel, so building an address out of
 * three pieces is three calls to this. */
char *strncat(char *d, const char *s, size_t max)
{
    size_t n = strlen(d);
    size_t i = 0;
    while (i + n < max && s[i]) { d[n + i] = s[i]; i++; }
    d[n + i] = 0;
    return d;
}
int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + d; s++;
        }
        return v * sign;
    }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}
void utoa(u32 v, char *buf, int base)
{
    const char *digits = "0123456789abcdef";
    char tmp[36];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % (u32)base]; v /= (u32)base; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
}
void itoa(int v, char *buf, int base)
{
    if (v < 0 && base == 10) { *buf++ = '-'; utoa((u32)(-v), buf, base); }
    else utoa((u32)v, buf, base);
}

/* ---- tiny printf: %d %u %x %b %p %s %c %%, optional width / zero pad ---- */
void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    char buf[36];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { putc(*p); continue; }
        p++;
        int width = 0, zero = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        switch (*p) {
        case 'd': itoa(__builtin_va_arg(ap, int), buf, 10); break;
        case 'u': utoa(__builtin_va_arg(ap, u32), buf, 10);  break;
        case 'x': utoa(__builtin_va_arg(ap, u32), buf, 16);  break;
        case 'b': utoa(__builtin_va_arg(ap, u32), buf, 2);   break;
        case 'p': puts("0x"); utoa(__builtin_va_arg(ap, u32), buf, 16); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            int l = (int)strlen(s);
            for (int i = l; i < width; i++) putc(' ');
            puts(s);
            continue;
        }
        case 'c': putc((char)__builtin_va_arg(ap, int)); continue;
        case '%': putc('%'); continue;
        case 0:   __builtin_va_end(ap); return;
        default:  putc('%'); putc(*p); continue;
        }
        int l = (int)strlen(buf);
        for (int i = l; i < width; i++) putc(zero ? '0' : ' ');
        puts(buf);
    }
    __builtin_va_end(ap);
}
