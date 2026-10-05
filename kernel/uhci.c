/* uhci.c -- USB 1.1 host controller, Intel/VIA flavour (I/O port based)
 *
 * UHCI does very little in hardware: software builds a linked list of
 * transfer descriptors (TDs), queue heads (QHs) group them, and the
 * controller walks the frame list once per millisecond executing whatever
 * is active. That makes it the simplest of the four to drive.
 *
 *   frame list --> [interrupt QHs] --> [control QH] --> end
 *
 * Every one of the 1024 frame slots points at the same chain, so interrupt
 * endpoints (the mouse) are serviced every millisecond and a control
 * transfer is picked up within one frame.
 *
 * Low- and full-speed devices only: that is all a UHCI port can talk to.
 * On a machine with EHCI the ports are handed to us by ehci.c.
 */

#include "usb.h"

#define MAXU   4
#define NIEP   4
#define NTD    48

typedef struct { u32 link, status, token, buf; u32 pad[4]; } utd_t;   /* 32 B */
typedef struct { u32 link, elem, pad[2]; } uqh_t;                      /* 16 B */

typedef struct {
    int       used, errors;
    uqh_t    *qh;
    utd_t    *td;
    u8       *buf;
    u8        addr, ep, ls, toggle;
    u16       mps;
    int       tag;
    usb_dev_t *dev;
} uiep_t;

typedef struct {
    int       ok;
    u16       io;
    int       nports;
    u32      *frames;
    uqh_t    *qh_ctrl, *qh_int[NIEP];
    utd_t    *td;                  /* NTD descriptors for control transfers */
    u8       *buf;                 /* 4 KB control buffer                   */
    uiep_t    iep[NIEP];
    usb_dev_t dev[8];
    usb_dev_t kid[12];
    u8        kused[12];
    u8        pstate[8];           /* 0 empty, 1 in use, 2 failed           */
    int       next_addr;
    volatile int rescan;
} uhci_t;

static uhci_t uh[MAXU];
static int    nu;
static usb_hc_t uhci_hc;

#define TD_ACTIVE   (1u << 23)
#define TD_ERRMASK  0x00760000u      /* stall, dbe, babble, crc/timeout, stuff */

static void ow(uhci_t *u, int off, u16 v) { outw((u16)(u->io + off), v); }
static u16  iw(uhci_t *u, int off)        { return inw((u16)(u->io + off)); }
static u16  pc(uhci_t *u, int p)          { return iw(u, 0x10 + 2 * p); }
static void pw(uhci_t *u, int p, u16 v)   { ow(u, 0x10 + 2 * p, v); }

/* ---- control transfers --------------------------------------------------- */

static int uhci_ctrl(usb_dev_t *ud, u8 rt, u8 rq, u16 val, u16 idx,
                     void *buf, u16 len)
{
    uhci_t *u = (uhci_t *)ud->priv;
    int ls = ud->speed == USB_LS;
    u32 mps = ud->mps0 ? ud->mps0 : 8;
    if (mps > 64) mps = 64;
    if (len > 512) return -1;
    int in = (rt & 0x80) != 0;
    u8 *sp = u->buf;                          /* 8 setup bytes, data at +16 */
    u8 *dp = u->buf + 16;

    sp[0] = rt; sp[1] = rq; sp[2] = (u8)val; sp[3] = (u8)(val >> 8);
    sp[4] = (u8)idx; sp[5] = (u8)(idx >> 8); sp[6] = (u8)len; sp[7] = (u8)(len >> 8);
    if (len && !in && buf) memcpy(dp, buf, len);
    if (len && in) memset(dp, 0, len);

    u32 base = ((u32)ud->addr << 8);
    u32 st = TD_ACTIVE | (3u << 27) | (ls ? (1u << 26) : 0);
    int n = 0;

    u->td[n].status = st;
    u->td[n].token = (7u << 21) | base | 0x2D;                 /* SETUP */
    u->td[n].buf = (u32)sp;
    n++;

    int toggle = 1;
    for (u32 off = 0; off < len; off += mps) {
        u32 c = len - off < mps ? len - off : mps;
        if (n >= NTD - 2) return -1;
        u->td[n].status = st | (in ? (1u << 29) : 0);
        u->td[n].token = ((c - 1) << 21) | ((u32)toggle << 19) | base | (in ? 0x69 : 0xE1);
        u->td[n].buf = (u32)(dp + off);
        toggle ^= 1;
        n++;
    }
    u->td[n].status = st | (1u << 24);                          /* IOC */
    u->td[n].token = (0x7FFu << 21) | (1u << 19) | base | ((len && in) ? 0xE1 : 0x69);
    u->td[n].buf = 0;
    n++;

    for (int i = 0; i < n; i++)
        u->td[i].link = (i + 1 < n) ? (((u32)&u->td[i + 1]) | 4) : 1;
    u->qh_ctrl->elem = (u32)&u->td[0];

    u32 t0 = timer_ms();
    int err = 0;
    for (;;) {
        int done = 1;
        for (int i = 0; i < n; i++) {
            u32 s = u->td[i].status;
            if (s & TD_ERRMASK) { err = 1; break; }
            if (s & TD_ACTIVE) done = 0;
        }
        if (err || done) break;
        if ((i32)(timer_ms() - t0) > 500) { err = 1; break; }
    }
    u->qh_ctrl->elem = 1;
    if (err) { usb_msleep(2); return -1; }

    if (len && in && buf) memcpy(buf, dp, len);
    return len;
}

/* ---- interrupt endpoints ---------------------------------------------------- */

static void iep_arm(uiep_t *e)
{
    e->td->link = 1;
    e->td->status = TD_ACTIVE | (3u << 27) | (e->ls ? (1u << 26) : 0) | (1u << 29);
    e->td->token = ((u32)(e->mps - 1) << 21) | ((u32)e->toggle << 19) |
                   ((u32)(e->ep & 15) << 15) | ((u32)e->addr << 8) | 0x69;
    e->td->buf = (u32)e->buf;
    e->qh->elem = (u32)e->td;
}

static int uhci_int_open(usb_dev_t *ud, u8 ep, u16 mps, u8 interval, int tag)
{
    (void)interval;
    uhci_t *u = (uhci_t *)ud->priv;
    uiep_t *e = NULL;
    for (int i = 0; i < NIEP; i++) if (!u->iep[i].used) { e = &u->iep[i]; e->qh = u->qh_int[i]; break; }
    if (!e) return -1;
    if (mps > 8 && ud->speed == USB_LS) mps = 8;
    if (mps > 64) mps = 64;
    if (!e->td)  e->td  = usb_dma(32, 32);
    if (!e->buf) e->buf = usb_dma(64, 64);
    if (!e->td || !e->buf) return -1;
    memset(e->buf, 0, 64);
    e->addr = ud->addr; e->ep = ep; e->ls = ud->speed == USB_LS;
    e->toggle = 0; e->mps = mps; e->tag = tag; e->errors = 0; e->dev = ud;
    iep_arm(e);
    e->used = 1;
    return 0;
}

void uhci_tick(void)
{
    for (int k = 0; k < nu; k++) {
        uhci_t *u = &uh[k];
        if (!u->ok) continue;
        for (int i = 0; i < NIEP; i++) {
            uiep_t *e = &u->iep[i];
            if (!e->used) continue;
            u32 s = e->td->status;
            if (s & TD_ACTIVE) continue;
            if (!(s & TD_ERRMASK)) {
                int got = (int)((s + 1) & 0x7FF);
                if (got > e->mps) got = e->mps;
                if (got > 0) usb_hid_report(e->tag, e->buf, got);
                e->toggle ^= 1;
                e->errors = 0;
            } else if (++e->errors > 20) {
                e->used = 0;                   /* the device is gone */
                e->qh->elem = 1;
                continue;
            }
            iep_arm(e);
        }
        if (iw(u, 0x02) & 0x10) { }            /* ignore: no interrupts used */
    }
}

/* ---- ports ----------------------------------------------------------------------- */

static usb_dev_t *uhci_attach_child(usb_dev_t *hub, int port, int speed)
{
    uhci_t *u = (uhci_t *)hub->priv;
    int k;
    for (k = 0; k < 12 && u->kused[k]; k++) ;
    if (k == 12) return NULL;
    usb_dev_t *d = &u->kid[k];
    memset(d, 0, sizeof *d);
    d->hc = &uhci_hc;
    d->priv = u;
    d->speed = (u8)(speed == USB_LS ? USB_LS : USB_FS);   /* a 1.1 bus: no high speed */
    d->port = (u8)port;
    d->mps0 = 8;
    d->parent = hub;
    int addr = u->next_addr++;
    if (u->next_addr > 120) u->next_addr = 1;
    if (uhci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return NULL;
    usb_msleep(12);
    d->addr = (u8)addr;
    u->kused[k] = 1;
    return d;
}

static void uhci_drop_child(usb_dev_t *d)
{
    uhci_t *u = (uhci_t *)d->priv;
    for (int i = 0; i < NIEP; i++)
        if (u->iep[i].used && u->iep[i].dev == d) {
            u->iep[i].used = 0;
            u->iep[i].qh->elem = 1;
        }
    for (int k = 0; k < 12; k++) if (&u->kid[k] == d) u->kused[k] = 0;
}

static void uhci_port_up(uhci_t *u, int p)
{
    u->pstate[p] = 2;
    u16 sc = pc(u, p);
    int ls = (sc & 0x100) != 0;

    pw(u, p, 0x0200);                        /* reset */
    usb_msleep(55);
    pw(u, p, 0x0000);
    usb_msleep(12);
    for (int i = 0; i < 10; i++) {
        pw(u, p, (u16)(pc(u, p) & 0x1FF5) | 0x0004 | 0x000A);   /* enable, clear changes */
        usb_msleep(5);
        if (pc(u, p) & 0x0004) break;
    }
    sc = pc(u, p);
    if (!(sc & 0x0004)) return;

    usb_dev_t *d = &u->dev[p];
    memset(d, 0, sizeof *d);
    d->hc = &uhci_hc;
    d->priv = u;
    d->speed = ls ? USB_LS : USB_FS;
    d->port = (u8)(p + 1);
    d->mps0 = 8;
    d->addr = 0;

    int addr = u->next_addr++;
    if (u->next_addr > 120) u->next_addr = 1;
    if (uhci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return;
    usb_msleep(12);
    d->addr = (u8)addr;
    if (usb_enumerate(d) >= 0) u->pstate[p] = 1;
}

static void uhci_scan(uhci_t *u)
{
    for (int p = 0; p < u->nports; p++) {
        u16 sc = pc(u, p);
        if (sc & 0x000A) pw(u, p, (u16)(sc & 0x1FF5) | 0x000A);   /* ack changes */
        if (sc & 1) {
            if (!u->pstate[p]) uhci_port_up(u, p);
        } else if (u->pstate[p]) {
            for (int i = 0; i < NIEP; i++)
                if (u->iep[i].used && usb_is_below(u->iep[i].dev, &u->dev[p])) {
                    u->iep[i].used = 0;
                    u->iep[i].qh->elem = 1;
                }
            usb_detach(&u->dev[p]);
            u->pstate[p] = 0;
        }
    }
}

void uhci_service(void)
{
    for (int k = 0; k < nu; k++)
        if (uh[k].ok) uhci_scan(&uh[k]);
}

/* ---- bring-up --------------------------------------------------------------------- */

static int uhci_init_one(pci_dev_t *pd)
{
    uhci_t *u = &uh[nu];
    memset(u, 0, sizeof *u);

    u32 bar4 = pci_read32(pd->bus, pd->dev, pd->fn, 0x20);
    if (!(bar4 & 1)) return 0;
    u->io = (u16)(bar4 & 0xFFFC);
    if (!u->io) return 0;
    pci_enable_bus_master(pd);

    /* take it from the BIOS: no more SMIs for USB legacy keyboard emulation */
    pci_write16(pd->bus, pd->dev, pd->fn, 0xC0, 0x8F00);

    ow(u, 0x00, 0x0004);                     /* global reset */
    usb_msleep(12);
    ow(u, 0x00, 0x0000);
    usb_msleep(2);
    ow(u, 0x00, 0x0002);                     /* host controller reset */
    for (int i = 0; i < 100 && (iw(u, 0x00) & 2); i++) usb_msleep(1);
    if (iw(u, 0x00) & 2) return 0;

    u->frames  = usb_dma(4096, 4096);
    u->qh_ctrl = usb_dma(16, 16);
    u->td      = usb_dma(NTD * 32, 32);
    u->buf     = usb_dma(1024, 64);
    if (!u->frames || !u->qh_ctrl || !u->td || !u->buf) return 0;
    for (int i = 0; i < NIEP; i++) {
        u->qh_int[i] = usb_dma(16, 16);
        if (!u->qh_int[i]) return 0;
    }
    u->qh_ctrl->link = 1; u->qh_ctrl->elem = 1;
    for (int i = 0; i < NIEP; i++) {
        u->qh_int[i]->elem = 1;
        u->qh_int[i]->link = (i + 1 < NIEP) ? (((u32)u->qh_int[i + 1]) | 2)
                                            : (((u32)u->qh_ctrl) | 2);
    }
    for (int i = 0; i < 1024; i++) u->frames[i] = ((u32)u->qh_int[0]) | 2;

    ow(u, 0x04, 0);                          /* no interrupts: polled */
    ow(u, 0x06, 0);                          /* frame number */
    outl((u16)(u->io + 0x08), (u32)u->frames);
    outb((u16)(u->io + 0x0C), 0x40);         /* SOF timing */
    ow(u, 0x02, 0x003F);                     /* clear status */
    pci_write16(pd->bus, pd->dev, pd->fn, 0xC0, 0x2000);
    ow(u, 0x00, 0x00C1);                     /* run, configured, 64-byte packets */
    usb_msleep(10);

    for (int p = 0; p < 8; p++) {
        u16 v = pc(u, p);
        if (v == 0xFFFF || !(v & 0x80)) break;   /* bit 7 is always 1 on a real port */
        u->nports++;
    }
    if (!u->nports) return 0;
    u->next_addr = 1;
    u->ok = 1;
    usb_msleep(100);
    uhci_scan(u);
    return 1;
}

int uhci_probe(void)
{
    uhci_hc.name = "UHCI";
    uhci_hc.ctrl = uhci_ctrl;
    uhci_hc.int_open = uhci_int_open;
    uhci_hc.set_mps0 = NULL;
    uhci_hc.hub_config = NULL;
    uhci_hc.attach_child = uhci_attach_child;
    uhci_hc.drop_child = uhci_drop_child;
    int found = 0;
    pci_dev_t pd;
    for (int i = 0; i < MAXU; i++) {
        if (!pci_find_class(0x0C, 0x03, 0x00, i, &pd)) break;
        if (uhci_init_one(&pd)) { nu++; found++; usb_count_controller(3); }
    }
    return found;
}
