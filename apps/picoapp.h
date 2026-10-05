/* picoapp.h -- the only header a .pico program includes.
 *
 * There is no libc here. A program gets exactly one thing from the kernel:
 * a pointer to the api table, stored in the global `api` by the startup
 * stub in crt0.c. Everything else it has to bring itself.
 *
 * Build a program with:
 *     make apps          (see the Makefile)
 */
#ifndef PICOAPP_H
#define PICOAPP_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed char        s8;
typedef short              s16;
typedef int                s32;
typedef __SIZE_TYPE__       size_t;   /* unsigned int on i386, 64-bit on a host test */

#define NULL ((void *)0)

/* ---- the api table, must match include/pico.h exactly ---- */
/* What the task manager sees. Flat, no pointers, so a program built
 * against one kernel keeps working on the next. */
typedef struct {
    int  pid;
    int  foreground;
    int  is_program;
    u32  cpu_ticks;
    u32  age_ticks;
    u32  stack_used, stack_size;
    int  waiting;                /* parked on a key or the clock    */
    char name[16];
} pico_task_t;

typedef struct {
    u32 total, used, free_bytes;
    u32 blocks;
    u32 kernel_bytes;
} pico_mem_t;

typedef struct {
    u32 version;
    u32 size;

    void (*putc)(char c);
    void (*puts)(const char *s);
    void (*printf)(const char *fmt, ...);
    void (*clear)(void);
    void (*setcolor)(u8 fg, u8 bg);
    void (*gotoxy)(int x, int y);
    int  (*width)(void);
    int  (*height)(void);

    int  (*getkey)(void);
    int  (*poll)(void);

    u32  (*ticks)(void);
    u32  (*ms)(void);
    void (*sleep)(u32 ms);

    void *(*malloc)(size_t n);
    void  (*free)(void *p);

    u32  (*rand)(void);
    void (*beep)(u32 hz, u32 ms);

    int  (*fs_read)(const char *name, char *buf, u32 max);
    int  (*fs_write)(const char *name, const char *buf, u32 len);
    void (*exit)(int code);

    /* ---- api v2 ---- */
    int  (*mouse)(int *x, int *y, int *buttons);
    int  (*files)(int index, char *name, u32 *size);
    int  (*tasks)(int index, pico_task_t *out);
    void (*meminfo)(pico_mem_t *out);
    int  (*run)(const char *name, int argc, char **argv);

    /* v3: switch the machine off or restart it. A desktop needs a way to
     * shut down, and a program cannot reach the shell's halt command. */
    void (*power)(int off_is_zero_reboot_is_one);

    /* Every program on the machine, packed or not. api->files only sees the
     * RAM disk, and a program that has never been run is still sitting
     * compressed in the store -- so a menu built on files() would show you
     * an empty machine. Returns 0 when index runs past the last one. */
    int  (*programs)(int index, char *name16, u32 *size);

    /* Hide the text cursor. A full-screen program does not want one
     * trailing its drawing around; the kernel puts it back when the
     * program exits, so forgetting to is not fatal. */
    void (*cursor)(int on);

    /* v4: fetch a page into a buffer. 0 = ok, negative = see pico.h */
    int  (*net_get)(const char *url, char *buf, u32 max, u32 *got,
                    char *status, int status_max);
    const char *(*tls_error)(void);
    void (*rtc)(int *hour, int *min, int *sec);
    int  (*net_status)(char *buf, int max);
    int  (*net_up)(void);
    int  (*wifi_join)(const char *ssid, const char *pass);
    int  (*wifi_joined)(void);
    const char *(*wifi_name)(void);

    /* ---- api v7: pixels, the pointer, the speaker -- new in 1.0 ---- */

    /* The drawing screen: 640x480 in 16 colours on a BIOS boot, the
     * firmware framebuffer unchanged on a UEFI one. gfx_mode(1) enters,
     * gfx_mode(0) returns to text. */
    int  (*gfx_mode)(int on);
    int  (*gfx_size)(int *w, int *h);
    void (*pixel)(int x, int y, int c);          /* c = 0..15   */
    void (*fill_rect)(int x, int y, int w, int h, int c);

    /* Replace one character cell of the running font -- this is how the
     * text-mode desktop gets a hand with a finger on it for a pointer. */
    void (*glyph)(int code, const u8 *rows16);

    /* The pointer in pixels, for the drawing screen. Same convention as
     * mouse(): 0 when there is no mouse. */
    int  (*mouse_px)(int *x, int *y, int *buttons);

    /* The speaker has no volume knob -- it is a square wave -- but it
     * can be silenced, so that is the knob. */
    void (*sound)(int on_is_zero_off_is_one);

    /* The whole clock and calendar in one read. */
    void (*clock)(int *hour, int *min, int *sec,
                  int *year, int *month, int *day);

    /* v8-v12 USB slots: removed, always 0. USB input works through the
     * firmware's PS/2 emulation. Kept so old programs keep loading. */
    int  (*usb_take)(void);
    int  (*usb_give_back)(void);
    int  (*usb_mouse_present)(void);

    /* v9: open the text shell as a child view; `exit` returns to the menu. */
    void (*shell)(void);

    int  (*usb_hid_pending)(void);
    int  (*usb_hid_enable)(void);
    int  (*usb_hid_rescan)(void);
    int  (*usb_hid_probe_existing)(void);

    /* ---- api v13: DOOM ---- */
    int  (*gfx256)(void);
    void (*setpal)(const u8 *rgb);
    int  (*blit)(const u8 *frame, int w, int h);
    const char *(*fsmap)(const char *name, u32 *size);
    int  (*keydown)(int code);

    /* ---- api v14: USB + pointer + text ---- */
    int  (*mouse_wheel)(void);
    void (*cursor_at)(int x, int y, int on);
    void (*text)(int x, int y, const char *s, int fg, int bg);
    int  (*usb_info)(char *buf, int max);
    int  (*usb_line)(int i, char *buf, int max);

    /* ---- api v15: flicker-free drawing ---- */
    int  (*blit_rect)(const u8 *frame, int fw, int fh, int x, int y, int w, int h);
    const u8 *(*glyph_rows)(int c);
} picoapi_t;

#ifndef PICOOS_VERSION
#define PICOOS_VERSION  "2.1"
#endif

extern picoapi_t *api;

/* every program provides this instead of main() */
int app_main(int argc, char **argv);

/* ---- keys without an ascii value ---- */
#define KEY_UP     0x81
#define KEY_DOWN   0x82
#define KEY_LEFT   0x83
#define KEY_RIGHT  0x84
#define KEY_HOME   0x85
#define KEY_END    0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
#define KEY_DEL    0x89
#define KEY_INS    0x8A
#define KEY_F1     0x90
#define KEY_F2     0x91
#define KEY_F3     0x92
#define KEY_F4     0x93
#define KEY_F5     0x94
#define KEY_F6     0x95
#define KEY_F9     0x98
#define KEY_F10    0x99

/* ---- colours ---- */
#define BLACK 0
#define BLUE 1
#define GREEN 2
#define CYAN 3
#define RED 4
#define MAGENTA 5
#define BROWN 6
#define LGREY 7
#define DGREY 8
#define LBLUE 9
#define LGREEN 10
#define LCYAN 11
#define LRED 12
#define LMAGENTA 13
#define YELLOW 14
#define WHITE 15

/* ---- the handful of string helpers a program usually wants ---- */
static inline int api_atoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static inline int app_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline void app_itoa(int v, char *buf)
{
    char t[12];
    int i = 0, n = 0;
    unsigned uv;
    if (v < 0) { buf[n++] = '-'; uv = (unsigned)(-v); } else uv = (unsigned)v;
    if (!uv) t[i++] = '0';
    while (uv) { t[i++] = (char)('0' + uv % 10); uv /= 10; }
    while (i) buf[n++] = t[--i];
    buf[n] = 0;
}

static inline void app_at(int x, int y, const char *s)
{
    api->gotoxy(x, y);
    api->puts(s);
}

#endif
