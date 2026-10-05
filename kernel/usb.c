/* usb.c -- the part of USB that is the same on every host controller
 *
 * PicoOS Pro 2.1. The controller drivers (xhci.c, ehci.c, ohci.c, uhci.c)
 * only know how to move bytes: give a device an address, run a control
 * transfer, poll an interrupt endpoint. This file does everything else:
 *
 *   - read the descriptors and find a HID boot mouse or keyboard,
 *   - put it in boot protocol (a fixed 3-8 byte report, no descriptor
 *     parsing needed),
 *   - turn mouse reports into pointer movement (mouse.c), and keyboard
 *     reports into the same scancode stream a PS/2 keyboard produces
 *     (kbd.c), so shift, caps lock, ctrl-c and DOOM's held-key table
 *     all behave identically.
 *
 * A 2.4 GHz wireless mouse needs no special treatment: its receiver is an
 * ordinary USB device that presents itself as a boot mouse (often next to
 * a boot keyboard on the same receiver).
 *
 * Why the keyboard too, when only the mouse was asked for: the firmware
 * makes a USB keyboard look like a PS/2 one only until the OS takes the
 * USB controller away from it, and taking the controller is exactly what a
 * USB mouse driver has to do. Without this the keyboard would die the
 * moment the mouse started working.
 */

#include "usb.h"

/* ---- small shared helpers -------------------------------------------- */

static volatile int busy;
static int ctrl_count[4];            /* xhci ehci ohci uhci */
static int n_mouse, n_kbd;
static char devlog[8][72];
static int  devlogn;

int usb_busy_enter(void)
{
    u32 f = irq_save();
    busy++;
    irq_restore(f);
    return busy;
}

void usb_busy_leave(void)
{
    u32 f = irq_save();
    if (busy > 0) busy--;
    irq_restore(f);
}

void usb_msleep(u32 ms)
{
    u32 t = timer_ms() + ms + 1;
    while ((i32)(timer_ms() - t) < 0) hlt();
}

/* Memory the controller reads and writes by physical address. Zeroed,
 * aligned, never freed, and owned by the kernel so that a program which
 * triggers a hot-plug does not take the controller's rings with it when it
 * exits. */
void *usb_dma(u32 size, u32 align)
{
    u8 *p = kmalloc(size + align);
    if (!p) return NULL;
    mem_disown(p);
    u32 a = ((u32)p + align - 1) & ~(align - 1);
    memset((void *)a, 0, size);
    return (void *)a;
}

int usb_mouse_count(void)      { return n_mouse; }
int usb_kbd_count(void)        { return n_kbd; }
int usb_controller_count(void) { return ctrl_count[0] + ctrl_count[1] + ctrl_count[2] + ctrl_count[3]; }

static void hex16(u16 v, char *o)
{
    static const char h[] = "0123456789abcdef";
    o[0] = h[(v >> 12) & 15]; o[1] = h[(v >> 8) & 15];
    o[2] = h[(v >> 4) & 15];  o[3] = h[v & 15];
}

static void devlog_add(const char *hc, usb_dev_t *d, u16 vid, u16 pid,
                       const char *what)
{
    if (devlogn >= 8) return;
    char *l = devlog[devlogn++];
    static const char *spd[] = { "?", "full", "low", "high", "super" };
    int n = 0;
    const char *s;
    for (s = hc; *s && n < 8; s++) l[n++] = *s;
    l[n++] = ' ';
    for (s = "port "; *s; s++) l[n++] = *s;
    l[n++] = (char)('0' + (d->port / 10) % 10);
    l[n++] = (char)('0' + d->port % 10);
    l[n++] = ' ';
    for (s = spd[d->speed < 5 ? d->speed : 0]; *s; s++) l[n++] = *s;
    l[n++] = ' ';
    hex16(vid, l + n); n += 4; l[n++] = ':'; hex16(pid, l + n); n += 4;
    l[n++] = ' ';
    for (s = what; *s && n < 70; s++) l[n++] = *s;
    l[n] = 0;
}

void usb_summary(char *out, int max)
{
    char t[12];
    out[0] = 0;
    strncat(out, "xHCI ", (size_t)max);  utoa((u32)ctrl_count[0], t, 10); strncat(out, t, (size_t)max);
    strncat(out, "  EHCI ", (size_t)max); utoa((u32)ctrl_count[1], t, 10); strncat(out, t, (size_t)max);
    strncat(out, "  OHCI ", (size_t)max); utoa((u32)ctrl_count[2], t, 10); strncat(out, t, (size_t)max);
    strncat(out, "  UHCI ", (size_t)max); utoa((u32)ctrl_count[3], t, 10); strncat(out, t, (size_t)max);
    strncat(out, "  mouse ", (size_t)max); utoa((u32)n_mouse, t, 10); strncat(out, t, (size_t)max);
    strncat(out, "  kbd ", (size_t)max);   utoa((u32)n_kbd, t, 10);   strncat(out, t, (size_t)max);
}

/* for the shell's `usb` command */
const char *usb_devlog_line(int i) { return (i >= 0 && i < devlogn) ? devlog[i] : NULL; }
void usb_count_controller(int kind) { if (kind >= 0 && kind < 4) ctrl_count[kind]++; }

/* ---- enumeration ------------------------------------------------------ */
static int hub_setup(usb_dev_t *d, u8 cfgval, u16 vid, u16 pid);


typedef struct { u8 iface, proto, ep, interval; u16 mps; } cand_t;

static int tag_seq;

int usb_enumerate(usb_dev_t *d)
{
    u8 buf[256];
    usb_hc_t *hc = d->hc;
    int r;

    memset(buf, 0, sizeof buf);
    r = hc->ctrl(d, 0x80, 6, 0x0100, 0, buf, 8);
    if (r < 8) return -1;
    {
        u16 mps = buf[7];
        if (d->speed == USB_SS) mps = (u16)(1u << (buf[7] > 15 ? 9 : buf[7]));
        if (mps == 0) mps = 8;
        if (mps != d->mps0) {
            d->mps0 = mps;
            if (hc->set_mps0) hc->set_mps0(d, mps);
        }
    }

    memset(buf, 0, sizeof buf);
    r = hc->ctrl(d, 0x80, 6, 0x0100, 0, buf, 18);
    if (r < 18) return -1;
    u8  dev_class = buf[4];
    u16 vid = (u16)(buf[8] | (buf[9] << 8));
    u16 pid = (u16)(buf[10] | (buf[11] << 8));

    memset(buf, 0, sizeof buf);
    r = hc->ctrl(d, 0x80, 6, 0x0200, 0, buf, 9);
    if (r < 9) return -1;
    u32 total = (u32)(buf[2] | (buf[3] << 8));
    if (total > sizeof buf) total = sizeof buf;
    if (total < 9) total = 9;
    memset(buf, 0, sizeof buf);
    r = hc->ctrl(d, 0x80, 6, 0x0200, 0, buf, (u16)total);
    if (r < 9) return -1;

    u8 cfgval = buf[5];
    cand_t cand[4];
    int ncand = 0, cur = -1;
    memset(cand, 0, sizeof cand);

    for (u32 i = 0; i + 2 <= total; ) {
        u8 len = buf[i], type = buf[i + 1];
        if (!len) break;
        if (type == 4 && i + 9 <= total) {
            u8 alt = buf[i + 3], cls = buf[i + 5], sub = buf[i + 6], pr = buf[i + 7];
            cur = -1;
            if (alt == 0 && cls == 3 && sub == 1 && (pr == 1 || pr == 2) && ncand < 4) {
                cand[ncand].iface = buf[i + 2];
                cand[ncand].proto = pr;
                cur = ncand++;
            }
        } else if (type == 5 && cur >= 0 && i + 7 <= total) {
            u8 ep = buf[i + 2], attr = buf[i + 3];
            if ((attr & 3) == 3 && (ep & 0x80) && !cand[cur].ep) {
                cand[cur].ep = ep;
                cand[cur].mps = (u16)((buf[i + 4] | (buf[i + 5] << 8)) & 0x7FF);
                cand[cur].interval = buf[i + 6];
            }
        }
        i += len;
    }

    if (!ncand) {
        if (dev_class == 9 && hc->attach_child)
            return hub_setup(d, cfgval, vid, pid);
        devlog_add(hc->name, d, vid, pid, dev_class == 9 ? "hub (no support)"
                                                         : "no HID boot interface");
        return 0;
    }

    if (hc->ctrl(d, 0x00, 9, cfgval, 0, NULL, 0) < 0) return -1;

    int attached = 0;
    for (int i = 0; i < ncand; i++) {
        if (!cand[i].ep) continue;
        /* boot protocol: fixed report layout, no descriptor parsing */
        hc->ctrl(d, 0x21, 0x0B, 0, cand[i].iface, NULL, 0);
        hc->ctrl(d, 0x21, 0x0A, 0, cand[i].iface, NULL, 0);   /* idle: only on change */

        int kind = cand[i].proto == 2 ? HID_MOUSE : HID_KBD;
        int tag = kind | ((tag_seq++ & 7) << 4);
        if (hc->int_open(d, cand[i].ep, cand[i].mps, cand[i].interval, tag) == 0) {
            attached++;
            if (kind == HID_MOUSE) { n_mouse++; mouse_usb_attach(1); d->tags |= 1; }
            else                   { n_kbd++;                        d->tags |= 2; }
            devlog_add(hc->name, d, vid, pid, kind == HID_MOUSE ? "mouse" : "keyboard");
        }
    }
    return attached;
}


/* ---- hubs --------------------------------------------------------------
 *
 * Many keyboards contain a hub, every front panel is a hub, and QEMU puts
 * one in as soon as there are more devices than root ports. A hub is just
 * a device with a class-specific control interface: power its ports, look
 * for connections, reset each connected port (which tells you the speed of
 * what is behind it), then address that device like any other.
 *
 * Only devices already attached when the hub appears are seen: a device
 * plugged into a hub later is picked up the next time the hub itself is
 * replugged. Hubs behind hubs work (up to five deep).
 */
static int hub_depth(usb_dev_t *d)
{
    int n = 0;
    while (d->parent) { n++; d = d->parent; }
    return n;
}

static int hub_setup(usb_dev_t *d, u8 cfgval, u16 vid, u16 pid)
{
    usb_hc_t *hc = d->hc;
    u8 b[16];

    if (hub_depth(d) >= 4) return 0;
    if (hc->ctrl(d, 0x00, 9, cfgval, 0, NULL, 0) < 0) return -1;
    memset(b, 0, sizeof b);
    if (hc->ctrl(d, 0xA0, 6, d->speed == USB_SS ? 0x2A00 : 0x2900, 0, b, 9) < 7)
        return -1;
    int np = b[2];
    if (np < 1) return -1;
    if (np > 8) np = 8;
    int ttt = (b[3] >> 5) & 3;
    u32 pwr = (u32)b[5] * 2 + 20;
    d->nports = (u8)np;
    if (hc->hub_config && hc->hub_config(d, np, ttt) < 0) return -1;

    for (int p = 1; p <= np; p++)
        hc->ctrl(d, 0x23, 3, 8, (u16)p, NULL, 0);        /* PORT_POWER */
    usb_msleep(pwr + 120);

    char what[24] = "hub, 0 ports";
    what[5] = (char)('0' + np);
    devlog_add(hc->name, d, vid, pid, what);

    for (int p = 1; p <= np; p++) {
        u8 st[4];
        memset(st, 0, 4);
        if (hc->ctrl(d, 0xA3, 0, 0, (u16)p, st, 4) < 4) continue;
        if (!(st[0] & 1)) continue;                      /* nothing plugged in */

        hc->ctrl(d, 0x23, 3, 4, (u16)p, NULL, 0);        /* PORT_RESET */
        int ok = 0;
        for (int i = 0; i < 60; i++) {
            usb_msleep(10);
            if (hc->ctrl(d, 0xA3, 0, 0, (u16)p, st, 4) < 4) break;
            if (st[2] & 0x10) { ok = 1; break; }         /* C_PORT_RESET */
        }
        if (!ok) continue;
        hc->ctrl(d, 0x23, 1, 20, (u16)p, NULL, 0);       /* clear C_PORT_RESET */
        usb_msleep(20);
        if (hc->ctrl(d, 0xA3, 0, 0, (u16)p, st, 4) < 4) continue;
        if (!(st[0] & 2)) continue;                      /* not enabled */

        int speed = d->speed == USB_SS ? USB_SS
                  : (st[1] & 4) ? USB_HS : (st[1] & 2) ? USB_LS : USB_FS;
        usb_dev_t *c = hc->attach_child(d, p, speed);
        if (!c) continue;
        c->parent = d;
        c->route_port = (u8)p;
        d->kids[p - 1] = c;
        if (usb_enumerate(c) < 0 && hc->drop_child) {
            d->kids[p - 1] = NULL;
            hc->drop_child(c);
        }
    }
    return 0;
}

int usb_is_below(usb_dev_t *d, usb_dev_t *root)
{
    for (; d; d = d->parent) if (d == root) return 1;
    return 0;
}

void usb_detach(usb_dev_t *d)
{
    for (int i = 0; i < 8; i++)
        if (d->kids[i]) {
            usb_detach(d->kids[i]);
            if (d->hc->drop_child) d->hc->drop_child(d->kids[i]);
            d->kids[i] = NULL;
        }
    if (d->tags & 1) { if (n_mouse) n_mouse--; mouse_usb_attach(-1); }
    if (d->tags & 2) { if (n_kbd) n_kbd--; }
    d->tags = 0;
}

/* ---- HID reports ------------------------------------------------------ */

/* usage ID -> scancode set 1. High bit of the second byte = E0 prefix. */
static const u8 sc_main[0x46] = {
    0,0,0,0,
    0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
    0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,
    0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,
    0x1C,0x01,0x0E,0x0F,0x39,0x0C,0x0D,0x1A,0x1B,0x2B,0x2B,0x27,0x28,0x29,
    0x33,0x34,0x35,0x3A,
    0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44,0x57,0x58
};

/* 0x49..0x52: ins home pgup del end pgdn right left down up (all E0) */
static const u8 sc_nav[10] = { 0x52,0x47,0x49,0x53,0x4F,0x51,0x4D,0x4B,0x50,0x48 };

static void send_sc(int ext, u8 sc, int make)
{
    if (ext) kbd_scancode(0xE0);
    kbd_scancode(make ? sc : (u8)(sc | 0x80));
}

static int usage_sc(u8 u, u8 *sc, int *ext)
{
    *ext = 0;
    if (u >= 4 && u <= 0x45) { *sc = sc_main[u]; return *sc != 0; }
    if (u >= 0x49 && u <= 0x52) { *sc = sc_nav[u - 0x49]; *ext = 1; return 1; }
    if (u == 0x54) { *sc = 0x35; *ext = 1; return 1; }      /* keypad / */
    if (u == 0x58) { *sc = 0x1C; *ext = 1; return 1; }      /* keypad enter */
    return 0;
}

static u8   kprev[8][8];
static u8   rep_sc, rep_ext, rep_on;
static u32  rep_next;

static void kbd_report(int slot, const u8 *r, int len)
{
    if (len < 8) return;
    if (r[2] == 1) return;                      /* rollover error */
    u8 *old = kprev[slot & 7];

    /* modifiers: ctrl shift alt, left and right */
    static const struct { u8 bit, sc, ext; } mod[] = {
        {0x01,0x1D,0},{0x02,0x2A,0},{0x04,0x38,0},
        {0x10,0x1D,1},{0x20,0x36,0},{0x40,0x38,1},
        {0x08,0x5B,1},{0x80,0x5C,1}
    };
    for (int i = 0; i < 8; i++) {
        int was = (old[0] & mod[i].bit) != 0, now = (r[0] & mod[i].bit) != 0;
        if (was != now) send_sc(mod[i].ext, mod[i].sc, now);
    }
    for (int i = 2; i < 8; i++) {                /* released */
        u8 k = old[i];
        if (!k) continue;
        int still = 0;
        for (int j = 2; j < 8; j++) if (r[j] == k) still = 1;
        if (still) continue;
        u8 sc; int ext;
        if (usage_sc(k, &sc, &ext)) {
            send_sc(ext, sc, 0);
            if (rep_on && rep_sc == sc && rep_ext == ext) rep_on = 0;
        }
    }
    for (int i = 2; i < 8; i++) {                /* pressed */
        u8 k = r[i];
        if (!k) continue;
        int had = 0;
        for (int j = 2; j < 8; j++) if (old[j] == k) had = 1;
        if (had) continue;
        u8 sc; int ext;
        if (usage_sc(k, &sc, &ext)) {
            send_sc(ext, sc, 1);
            if (k != 0x39) {                     /* caps lock does not repeat */
                rep_sc = sc; rep_ext = (u8)ext; rep_on = 1;
                rep_next = timer_ms() + 450;
            }
        }
    }
    memcpy(old, r, 8);
}

void usb_hid_report(int tag, const u8 *data, int len)
{
    if ((tag & 15) == HID_MOUSE) {
        if (len < 3) return;
        int btn = data[0] & 7;
        int dx = (i8)data[1], dy = (i8)data[2];
        int wh = len >= 4 ? (i8)data[3] : 0;
        mouse_feed(dx, dy, wh, btn);
    } else if ((tag & 15) == HID_KBD) {
        kbd_report(tag >> 4, data, len);
    }
}

/* ---- the 100 Hz hook and the hot-plug service -------------------------- */

static int ready;

void usb_tick(void)
{
    if (!ready || busy) return;
    xhci_tick();
    ehci_tick();
    ohci_tick();
    uhci_tick();
    if (rep_on && (i32)(timer_ms() - rep_next) >= 0) {
        send_sc(rep_ext, rep_sc, 1);
        rep_next += 35;
    }
}

void usb_service(void)
{
    static u32 last;
    if (!ready || busy) return;
    u32 now = timer_ms();
    if ((i32)(now - last) < 250) return;      /* four times a second is plenty */
    last = now;
    usb_busy_enter();
    xhci_service();
    ehci_service();
    ohci_service();
    uhci_service();
    usb_busy_leave();
}

void usb_init(void)
{
    usb_busy_enter();
    int found = 0;
    found += xhci_probe();
    found += ehci_probe();
    found += ohci_probe();
    found += uhci_probe();
    (void)found;
    ready = 1;
    usb_busy_leave();
}
