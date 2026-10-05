/* exec.c -- loading and running .pico programs
 *
 * This is the part that turns PicoOS from "a shell with built-in commands"
 * into something that can run code it has never seen before.
 *
 * A .pico file is deliberately dumb: a header, flat code, and a list of the
 * words inside that code that hold an absolute address. No symbols, no
 * dynamic linking. The kernel validates the header, copies the image to a
 * block of heap, adds the load offset to every word on the list, clears the
 * bss and calls it. On a 386 that is a memcpy, a short loop and a call --
 * and because nothing is reserved in advance, a program costs exactly its
 * own size.
 *
 * The one piece of real engineering here is the crash guard: before jumping
 * into the program the kernel saves its own execution state, and the CPU
 * exception handler jumps back to it. A program that divides by zero gets
 * killed; the machine keeps running.
 */

#include "pico.h"
#include "usb.h"
#include "net.h"

typedef int (*pico_entry_t)(picoapi_t *, int, char **);

/* Everything a running program needs, kept in one heap block so that a
 * background program owns a private copy of its own arguments -- the shell
 * line it was typed on is long gone by the time it runs. */
typedef struct {
    pico_entry_t entry;
    int          argc;
    char       **argv;
} prog_arg_t;

/* ---------------------------------------------------------------- api --
 * Small shims so the api table never exposes a kernel function directly.
 * That keeps the table stable even if the kernel internals get rewritten.
 */

static void api_gotoxy(int x, int y)      { con_set_cursor(x, y); }
static int  api_width(void)               { return con_width(); }
static int  api_height(void)              { return con_height(); }
static int  api_getkey(void)              { return kbd_getchar(); }
static int  api_poll(void)                { return kbd_poll(); }
static u32  api_ticks(void)               { return timer_ticks(); }
static u32  api_ms(void)                  { return timer_ms(); }
static void api_sleep(u32 ms)             { sleep_ms(ms); }
static void *api_malloc(size_t n)         { return kmalloc(n); }
static void api_free(void *p)             { kfree(p); }

/* A program asking for a page gets the same fetch `wget` uses, redirects
 * and all, only into its own buffer instead of onto the RAM disk. */
static int  api_net_get(const char *url, char *buf, u32 max, u32 *got,
                        char *status, int status_max)
{
    return net_http_fetch(url, buf, max, got, status, status_max);
}

/* ...and when that fetch was an encrypted one that never got off the
 * ground, this is the reason it gives. */
static const char *api_tls_error(void)    { return net_tls_error(); }

static void api_rtc(int *h, int *m, int *s) { rtc_time(h, m, s); }

/* ---- v7: pixels, the pointer, the speaker ---- */

static int api_mouse(int *x, int *y, int *btn);   /* defined below */

static int api_gfx_mode(int on)            { return gfx_mode(on); }
static int api_gfx_size(int *w, int *h)    { gfx_size(w, h); return 1; }
static void api_pixel(int x, int y, int c) { gfx_pixel(x, y, c); }
static void api_fillr(int x, int y, int w, int h, int c)
                                           { gfx_fill(x, y, w, h, c); }
static void api_glyph(int code, const u8 *rows) { gfx_set_glyph(code, rows); }

/* Same switching-on and is-it-alive logic as mouse(), then the pixel
 * position instead of the cell one. */
static int api_mouse_px(int *x, int *y, int *btn)
{
    int px = 0, py = 0, b = 0;
    if (!api_mouse(0, 0, 0)) return 0;
    if (!mouse_get_px(&px, &py, &b)) return 0;
    if (x)   *x = px;
    if (y)   *y = py;
    if (btn) *btn = b;
    gfx_cursor(px, py, 1);          /* the kernel draws the pointer */
    return 1;
}
static int  api_blit_rect(const u8 *f, int fw, int fh, int x, int y, int w, int h)
{ return gfx_blit_rect(f, fw, fh, x, y, w, h); }
static const u8 *api_glyph_rows(int c) { return gfx_glyph(c); }
static int  api_mouse_wheel(void)  { return mouse_wheel_take(); }
static void api_cursor_at(int x, int y, int on) { gfx_cursor(x, y, on); }
static void api_text(int x, int y, const char *s, int fg, int bg) { gfx_text(x, y, s, fg, bg); }
static int  api_usb_info(char *buf, int max)
{
    if (!buf || max < 1) return 0;
    usb_summary(buf, max);
    return (int)strlen(buf);
}
static int  api_usb_line(int i, char *buf, int max)
{
    const char *l = usb_devlog_line(i);
    if (!l || !buf || max < 1) return 0;
    buf[0] = 0;
    strncat(buf, l, (size_t)max);
    return 1;
}

static void api_sound(int on)              { sound_set(on); }

static void api_clock(int *h, int *m, int *s, int *y, int *mo, int *d)
{
    rtc_time(h, m, s);
    rtc_date(y, mo, d);
}

/* One line about the network, put together by hand: there is no snprintf
 * in here and a status line does not need one. */
static int api_net_status(char *buf, int max)
{
    if (!buf || max < 8) return -1;
    netif_t *n = net_active();
    int at = 0, i;

    const char *src = (n && n->present) ? n->name : "no network card";
    for (i = 0; src[i] && at < max - 1; i++) buf[at++] = src[i];
    if (!n || !n->present) { buf[at] = 0; return -1; }

    if (n->is_wifi) {
        const char *w = " wireless";
        for (i = 0; w[i] && at < max - 1; i++) buf[at++] = w[i];
    }
    if (at < max - 1) buf[at++] = ' ';
    if (n->ip) {
        char ip[16];
        net_fmt_ip(n->ip, ip);
        for (i = 0; ip[i] && at < max - 1; i++) buf[at++] = ip[i];
    } else {
        const char *s = "no address";
        for (i = 0; s[i] && at < max - 1; i++) buf[at++] = s[i];
    }
    buf[at] = 0;
    return 0;
}

/* `net_need_ip` is what `wget` calls before it does anything: dhcp the
 * first time, and a plain answer of "yes, you have one" after that. */
static int  api_net_up(void)              { return net_need_ip(); }

static int  api_wifi_join(const char *ssid, const char *pass)
{
    return wifi_ser_join(ssid, pass);
}
static int  api_wifi_joined(void)         { return wifi_ser_joined(); }
static const char *api_wifi_name(void)    { return wifi_ser_ssid(); }
static void api_clear(void)               { con_clear(); }
static void api_setcolor(u8 f, u8 b)      { con_setcolor(f, b); }

/* xorshift seeded from the timer, which is good enough for a falling block */
static u32 rng_state = 0;
static u32 api_rand(void)
{
    if (!rng_state) rng_state = timer_ticks() * 2654435761u + 12345u;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void api_beep(u32 hz, u32 ms)
{
    if (!sound_is_on()) return;          /* the off switch is honest */
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;
    u32 div = 1193180u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)(div >> 8));
    outb(0x61, inb(0x61) | 3);
    sleep_ms(ms);
    outb(0x61, inb(0x61) & ~3);
}

static int api_fs_read(const char *name, char *buf, u32 max)
{
    file_t *f = fs_find(name);
    if (!f) return -1;
    u32 n = f->size < max ? f->size : max;
    memcpy(buf, f->data, n);
    return (int)n;
}

static int api_fs_write(const char *name, const char *buf, u32 len)
{
    return fs_write(name, buf, len);
}

static void api_exit(int code)
{
    if (!task_is_prog()) return;
    task_set_exit(code);
    pico_longjmp(task_jmpbuf(), 2);      /* 2 = the program asked to leave */
}

/* ---- api v2 ---------------------------------------------------------
 *
 * Enabling the mouse the first time a program asks for it, rather than at
 * boot, is the whole reason a machine that never opens the menu pays
 * nothing for having one. The cost of a mouse is not the driver code, it
 * is that once the auxiliary port starts reporting, its bytes land in the
 * same buffer the keyboard reads from and everything has to be ready for
 * them. So we do not start until we are asked. */
/* The store first, then any .pico on the RAM disk that is not already in
 * it -- which is how a program you added yourself shows up next to the
 * ones that shipped. */
static int api_programs(int index, char *name, u32 *size)
{
    int n = store_count();

    if (index < n) {
        const store_ent_t *e = store_at(index);
        if (!e) return 0;
        memcpy(name, e->name, FS_NAMELEN);
        name[FS_NAMELEN - 1] = 0;
        if (size) *size = e->orig;
        return 1;
    }

    int seen = n;
    for (int i = 0;; i++) {
        file_t *f = fs_at(i);
        if (!f) break;
        int len = (int)strlen(f->name);
        if (len < 5 || strcmp(f->name + len - 5, ".pico")) continue;
        if (store_find(f->name)) continue;      /* already counted above */
        if (seen++ != index) continue;
        memcpy(name, f->name, FS_NAMELEN);
        name[FS_NAMELEN - 1] = 0;
        if (size) *size = f->size;
        return 1;
    }
    return 0;
}

static void api_power(int mode)
{
    if (mode) reboot();
    shutdown();
}

static int api_mouse(int *x, int *y, int *btn)
{
    static int tried;
    if (!tried) { tried = 1; mouse_enable(); }

    /* A controller that accepts the handshake but never sends a packet is
     * a PS/2 port with nothing plugged into it -- common on laptops, and
     * on any desktop where the mouse lives on USB. Enabled is not the same
     * as present, so a program is told there is a mouse only once one has
     * actually moved. Until then the menu stays keyboard-only and draws no
     * pointer for a mouse that cannot move it. */
    if (!mouse_alive()) {
        /* Enabled is not the same as present: until a mouse has actually
         * moved, programs are told there is none. */
        return 0;
    }
    return mouse_get(x, y, btn);
}

static int api_files(int index, char *name, u32 *size)
{
    file_t *f = fs_at(index);
    if (!f) return 0;
    if (name) { memcpy(name, f->name, FS_NAMELEN); name[FS_NAMELEN - 1] = 0; }
    if (size) *size = f->size;
    return 1;
}

static int api_tasks(int index, pico_task_t *out)
{
    return out ? task_info(index, out) : 0;
}

static void api_meminfo(pico_mem_t *out)
{
    extern char _kernel_end[];
    if (!out) return;
    out->total        = mem_total();
    out->used         = mem_used();
    out->free_bytes   = mem_free();
    out->blocks       = (u32)mem_blocks();
    out->kernel_bytes = (u32)_kernel_end - 0x00010000u;
}

/* pico_exec spawns a separate task and waits for it, then takes the screen
 * back, so a program calling this gets the console returned in the state it
 * left it. That is what makes it safe for a menu to launch things. */
static int api_run(const char *name, int argc, char **argv)
{
    if (!name || !name[0]) return PICO_ENOFILE;

    /* Unpack it first if it is still compressed. The shell's start command
     * does this, and a program calling api->run has exactly the same right
     * to expect that naming a program runs it -- the desktop listing every
     * program on the machine and then refusing to start five of them
     * because of how they happen to be stored is not a distinction the
     * person clicking should ever see. */
    if (!fs_find(name)) store_extract(name);

    return pico_exec(name, argc, argv);
}

/* No native USB driver: these slots stay 0 forever. They exist only so
 * that programs built against older kernels keep loading. */
static int api_usb_take(void)         { return 0; }
static int api_usb_give(void)         { return 0; }
static int api_usb_mouse(void)        { return usb_mouse_count(); }
static void api_shell(void)           { shell_open(); }
static int  api_usb_hid_pending(void) { return 0; }
static int  api_usb_hid_enable(void)  { return 0; }
static int  api_usb_hid_rescan(void)  { return 0; }
static int  api_usb_hid_probe_existing(void) { return 0; }

/* ---- api v13: DOOM ---- */
static int  api_gfx256(void)          { return gfx_is256(); }
static void api_setpal(const u8 *rgb) { gfx_setpal(rgb); }
static int  api_blit(const u8 *f, int w, int h) { return gfx_blit(f, w, h); }
static const char *api_fsmap(const char *n, u32 *s) { return fs_mapdata(n, s); }
static int  api_keydown(int code)     { return kbd_down(code); }

static const picoapi_t api = {
    PICOAPI_VERSION, sizeof(picoapi_t),
    putc, puts, kprintf, api_clear, api_setcolor, api_gotoxy,
    api_width, api_height,
    api_getkey, api_poll,
    api_ticks, api_ms, api_sleep,
    api_malloc, api_free,
    api_rand, api_beep,
    api_fs_read, api_fs_write, api_exit,
    api_mouse, api_files, api_tasks, api_meminfo, api_run,
    api_power, api_programs, con_cursor,
    api_net_get,
    api_tls_error,
    api_rtc,
    api_net_status,
    api_net_up,
    api_wifi_join,
    api_wifi_joined,
    api_wifi_name,
    api_gfx_mode, api_gfx_size, api_pixel, api_fillr, api_glyph,
    api_mouse_px, api_sound, api_clock,
    api_usb_take, api_usb_give, api_usb_mouse,
    api_shell,
    api_usb_hid_pending, api_usb_hid_enable, api_usb_hid_rescan,
    api_usb_hid_probe_existing,
    api_gfx256, api_setpal, api_blit, api_fsmap, api_keydown,
    api_mouse_wheel, api_cursor_at, api_text, api_usb_info, api_usb_line,
    api_blit_rect, api_glyph_rows
};

/* ------------------------------------------------------------- loader -- */

/* Which program is on the CPU right now, if any. The exception handler
 * asks this to decide between "kill the program" and "halt the machine". */
const char *pico_running(void) { return task_name(); }

const char *pico_error(int code)
{
    switch (code) {
    case PICO_OK:      return "ok";
    case PICO_ENOFILE: return "no such file";
    case PICO_EMAGIC:  return "not a .pico program";
    case PICO_EFMT:    return "built for a newer .pico format";
    case PICO_EADDR:   return "wrong load address";
    case PICO_ESIZE:   return "program too big";
    case PICO_ECKSUM:  return "corrupt image (checksum)";
    case PICO_EAPI:    return "needs a newer kernel api";
    case PICO_ETRUNC:  return "file is truncated";
    case PICO_EARCH:   return "compiled for a different processor";
    case PICO_ENOMEM:  return "not enough memory to load it";
    case PICO_ERELOC:  return "broken relocation table";
    case PICO_ETASKS:  return "too many things running at once";
    }
    return "unknown error";
}

/* Called from the exception handler when a program faults. */
void pico_fault(void)
{
    if (task_is_prog()) pico_longjmp(task_jmpbuf(), 3);   /* 3 = it crashed */
}

/* Check the header and work out how much memory the program needs.
 * Returns 0 and fills *out on success, or a PICO_E* code. */
static int validate(file_t *f, pico_hdr_t *out)
{
    if (f->size < sizeof(pico_hdr_t)) return PICO_ETRUNC;

    pico_hdr_t h;
    memcpy(&h, f->data, sizeof h);

    if (h.magic != PICO_MAGIC)       return PICO_EMAGIC;
    if (h.fmt > PICO_FMT)            return PICO_EFMT;
    if (h.arch != PICO_ARCH)         return PICO_EARCH;
    if (h.api_min > PICOAPI_VERSION) return PICO_EAPI;
    if (h.image_size + h.bss_size > PICO_MAX_IMAGE) return PICO_ESIZE;
    if (f->size < sizeof(pico_hdr_t) + h.image_size) return PICO_ETRUNC;
    if (h.entry < h.link_addr ||
        h.entry >= h.link_addr + h.image_size)       return PICO_EADDR;
    /* The relocation list sits after the image in the file, not inside it:
     * the image has to end where .bss begins, or zeroing the bss lands in
     * the wrong place. So the list is bounded by the file, not the image. */
    if (h.reloc_count &&
        sizeof(pico_hdr_t) + h.reloc_off + h.reloc_count * 4 > f->size)
        return PICO_ERELOC;

    const u8 *img = (const u8 *)(f->data + sizeof(pico_hdr_t));
    u32 sum = 0;
    for (u32 i = 0; i < h.image_size; i++) sum += img[i];
    if (sum != h.checksum) return PICO_ECKSUM;

    *out = h;
    return PICO_OK;
}

/* Copy the image to `base` and fix up every absolute address in it. */
static int load_at(file_t *f, const pico_hdr_t *h, u8 *base)
{
    const u8 *img = (const u8 *)(f->data + sizeof(pico_hdr_t));

    memcpy(base, img, h->image_size);
    memset(base + h->image_size, 0, h->bss_size);

    u32 delta = (u32)base - h->link_addr;
    if (delta) {
        /* the relocation list lives inside the image, so read it from the
         * copy we just made -- patching it is harmless, it is only data */
        const u32 *list = (const u32 *)(img + h->reloc_off);
        for (u32 i = 0; i < h->reloc_count; i++) {
            u32 off = list[i];
            if (off + 4 > h->image_size) return PICO_ERELOC;
            u32 *slot = (u32 *)(base + off);
            *slot += delta;
        }
    }
    return PICO_OK;
}

/* --------------------------------------------------------------- spawn --
 *
 * Loading and running are now two different things. `spawn` builds a task
 * around the program and hands it to the scheduler; whether the shell then
 * waits for it or carries on is the caller's business. That one split is
 * the whole difference between `start tetris` and `start clock &`.
 */

/* The body of every program task. Runs on the task's own stack. */
static void prog_body(void *arg)
{
    prog_arg_t *pa = (prog_arg_t *)arg;
    int code;

    int why = pico_setjmp(task_jmpbuf());
    if (why == 0) {
        code = pa->entry((picoapi_t *)&api, pa->argc, pa->argv);
    } else if (why == 2) {
        code = task_get_exit();          /* it called exit()  */
    } else if (why == 3) {
        code = -100;                     /* it crashed        */
    } else {
        code = -101;                     /* it was killed     */
    }

    /* Whatever state the program left the console and the speaker in,
     * put it back before the next thing runs. A graphics program that
     * exits -- or crashes -- with the drawing screen up would otherwise
     * leave the shell typing into pixels. */
    con_setcolor(LGREY, BLACK);
    outb(0x61, inb(0x61) & ~3);
    if (gfx_ready()) gfx_mode(0);

    task_exit(code);                     /* frees the image, never returns */
}

/* Copy argv into one heap block the task owns for its whole life. */
static char *pack_args(int argc, char **argv, prog_arg_t **out)
{
    u32 need = sizeof(prog_arg_t) + (u32)(argc + 1) * sizeof(char *);
    for (int i = 0; i < argc; i++) need += strlen(argv[i]) + 1;

    char *blk = (char *)kmalloc(need);
    if (!blk) return NULL;

    prog_arg_t *pa = (prog_arg_t *)blk;
    char **av = (char **)(blk + sizeof(prog_arg_t));
    char  *sp = (char *)(av + argc + 1);

    for (int i = 0; i < argc; i++) {
        strcpy(sp, argv[i]);
        av[i] = sp;
        sp += strlen(sp) + 1;
    }
    av[argc] = NULL;
    pa->argc = argc;
    pa->argv = av;
    *out = pa;
    return blk;
}

/* Load `name`, wrap it in a task, return the pid (or a PICO_E* code). */
static int spawn(const char *name, int argc, char **argv, int foreground)
{
    file_t *f = fs_find(name);
    if (!f) return PICO_ENOFILE;

    pico_hdr_t h;
    int rc = validate(f, &h);
    if (rc != PICO_OK) return rc;

    u8 *base = (u8 *)kmalloc(h.image_size + h.bss_size);
    if (!base) return PICO_ENOMEM;

    rc = load_at(f, &h, base);
    if (rc != PICO_OK) { kfree(base); return rc; }

    prog_arg_t *pa;
    char *blk = pack_args(argc, argv, &pa);
    if (!blk) { kfree(base); return PICO_ENOMEM; }

    pa->entry = (pico_entry_t)(h.entry + ((u32)base - h.link_addr));

    int pid = task_create(name, prog_body, pa, base, blk, foreground);
    if (pid < 0) {
        kfree(base);
        kfree(blk);
        return pid == -2 ? PICO_ETASKS : PICO_ENOMEM;
    }
    return pid;
}

/* Start a program in the background. */
int pico_bg(const char *name, int argc, char **argv)
{
    return spawn(name, argc, argv, 0);
}

/* Start a program and wait for it, the way `start tetris` should behave.
 * While we wait we are just another task parked on an event, so anything
 * running in the background keeps its share of the CPU. */
int pico_exec(const char *name, int argc, char **argv)
{
    kbd_flush();                       /* PICO-FIX-kbdflush: the child */
    int pid = spawn(name, argc, argv, 1); /* starts with a clean slate */
    if (pid < 0) return pid;

    while (!task_done(pid)) task_wait();

    int code = task_exit_code(pid);
    task_reap(pid);

    kbd_flush();           /* PICO-FIX-kbdflush: ... and the parent gets one back */
    task_setfg(task_self());             /* take the screen back */
    con_setcolor(LGREY, BLACK);
    con_cursor(1);                       /* a program that hid it and then
                                            crashed must not leave the shell
                                            typing into an invisible line */
    sti();
    return code;
}

/* Bring a background program forward and wait for it to finish. */
int pico_fg(int pid)
{
    if (task_setfg(pid) < 0) return -1;
    while (!task_done(pid)) task_wait();

    int code = task_exit_code(pid);
    task_reap(pid);
    task_setfg(task_self());
    con_setcolor(LGREY, BLACK);
    return code;
}
