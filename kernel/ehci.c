/* ehci.c -- USB 2.0 host controller (the black/grey ports on most cases)
 *
 * A PC with USB 2.0 has an EHCI controller for the black/grey sockets. Two
 * layouts exist, and they need different treatment:
 *
 *  1. EHCI + "companion" controllers (UHCI/OHCI) on the same sockets (AMD,
 *     VIA, NVIDIA, older Intel). Low/full-speed devices -- mice, keyboards,
 *     wireless receivers -- are handled by the companion. This driver then
 *     only takes the controller from the BIOS and leaves CONFIGFLAG clear,
 *     which hands every port to the companion (uhci.c / ohci.c).
 *
 *  2. EHCI with NO companion (Intel 6-series and later, many embedded
 *     chips). Every socket goes through the EHCI itself, and the mouse or
 *     keyboard sits behind a built-in "rate matching hub". Handing the
 *     ports to a non-existent companion is what used to kill the keyboard
 *     and mouse in every black port: the BIOS emulation was switched off
 *     (the handoff) and nothing took over. For this layout the driver is a
 *     real EHCI driver now:
 *       - control transfers through the asynchronous schedule,
 *       - interrupt-IN polling through the periodic schedule,
 *       - split transactions for low/full-speed devices behind a
 *         high-speed hub (hub address + port go into the queue head),
 *       - hot-plug of root ports.
 *     usb.c's generic hub code already walks the hub behind the root port.
 */
#include "usb.h"

#define R32(b, o)  (*(volatile u32 *)((u8 *)(b) + (o)))

#define MAXE    4
#define NIEP    4
#define MAXPRT  15
#define NQTD    8

/* USBCMD / USBSTS */
#define CMD_RS     (1u << 0)
#define CMD_RESET  (1u << 1)
#define CMD_PSE    (1u << 4)
#define CMD_ASE    (1u << 5)
#define STS_HALT   (1u << 12)
#define STS_PSS    (1u << 14)
#define STS_ASS    (1u << 15)

/* PORTSC */
#define PS_CCS     (1u << 0)
#define PS_CSC     (1u << 1)
#define PS_PED     (1u << 2)
#define PS_PEDC    (1u << 3)
#define PS_OCC     (1u << 5)
#define PS_RESUME  (1u << 6)
#define PS_SUSP    (1u << 7)
#define PS_RESET   (1u << 8)
#define PS_LS_MASK (3u << 10)
#define PS_LS_K    (1u << 10)
#define PS_PP      (1u << 12)
#define PS_OWNER   (1u << 13)
#define PS_W1C     (PS_CSC | PS_PEDC | PS_OCC)

/* qTD token */
#define QT_ACTIVE  (1u << 7)
#define QT_HALT    (1u << 6)
#define QT_ERRMASK 0x78u                     /* buffer, babble, xact, missed uframe */
#define QT_IOC     (1u << 15)
#define QT_PID_OUT   0u
#define QT_PID_IN    1u
#define QT_PID_SETUP 2u

typedef struct { u32 next, alt, token, buf[5]; } qtd_t;                   /* 32 B */
typedef struct { u32 link, info1, info2, cur, onext, oalt, otoken, obuf[5]; } qh_t; /* 48 B */

typedef struct {
    int     used, errors;
    qh_t   *qh;
    qtd_t  *td;
    u8     *buf;
    u8      toggle;
    u16     mps;
    int     tag;
    usb_dev_t *dev;
} eiep_t;

typedef struct {
    int       ok;
    u8       *cap, *op;
    int       nports;
    int       ncc;
    int       native;                 /* 1: we drive the devices ourselves */
    u32      *frames;
    qh_t     *qh_ctrl;                /* the only queue head of the async list */
    qh_t     *qh_int[NIEP];
    qtd_t    *td;                     /* NQTD descriptors for control transfers */
    u8       *buf;                    /* 4 KB control buffer                    */
    eiep_t    iep[NIEP];
    usb_dev_t dev[MAXPRT + 1];
    usb_dev_t kid[12];
    u8        kused[12];
    u8        pstate[MAXPRT + 1];     /* 0 empty, 1 in use, 2 failed/ignored    */
    int       next_addr;
} ehci_t;

static ehci_t   eh[MAXE];
static int      ne;
static usb_hc_t ehci_hc;
static int      released;

int ehci_released(void) { return released; }

static u32 portsc(ehci_t *e, int p) { return R32(e->op, 0x44 + 4 * p); }
static void portw(ehci_t *e, int p, u32 v) { R32(e->op, 0x44 + 4 * p) = v; }

/* spin until (reg & mask) == want, or ms milliseconds pass */
static int spin(ehci_t *e, u32 off, u32 mask, u32 want, u32 ms)
{
    u32 t0 = timer_ms();
    while ((R32(e->op, off) & mask) != want)
        if ((i32)(timer_ms() - t0) > (i32)ms) return -1;
    return 0;
}

/* ---- control transfers (asynchronous schedule) ----------------------------
 *
 * One queue head, linked to itself, is the whole asynchronous list. It is
 * rewritten for every transfer, which is only safe while the controller is
 * not walking the list -- so the schedule is switched off before the queue
 * head is touched and on again to run the transfer.
 */
static void async_off(ehci_t *e)
{
    R32(e->op, 0x00) &= ~CMD_ASE;
    spin(e, 0x04, STS_ASS, 0, 20);
}

/* The nearest high-speed hub above a low/full-speed device: its address and
 * the port of that hub the device hangs off (for the split transaction). */
static void tt_of(usb_dev_t *d, u32 *hub_addr, u32 *hub_port)
{
    usb_dev_t *c = d, *h = d->parent;
    while (h && h->speed != USB_HS) { c = h; h = h->parent; }
    *hub_addr = h ? h->addr : 0;
    *hub_port = c->port;
}

static u32 eps_of(int speed) { return speed == USB_HS ? 2u : speed == USB_LS ? 1u : 0u; }

static void qtd_set(qtd_t *t, u32 next, u32 pid, u32 toggle, u32 len, void *buf, int ioc)
{
    u32 a = (u32)buf;
    t->next = next;
    t->alt = 1;
    t->token = QT_ACTIVE | (pid << 8) | (3u << 10) | (ioc ? QT_IOC : 0) |
               (len << 16) | (toggle << 31);
    t->buf[0] = a;
    t->buf[1] = (a & ~0xFFFu) + 0x1000;
    t->buf[2] = t->buf[3] = t->buf[4] = 0;
}

static int ehci_ctrl(usb_dev_t *ud, u8 rt, u8 rq, u16 val, u16 idx,
                     void *buf, u16 len)
{
    ehci_t *e = (ehci_t *)ud->priv;
    if (len > 512) return -1;
    int in = (rt & 0x80) != 0;
    u32 mps = ud->mps0 ? ud->mps0 : 8;
    if (mps > 64) mps = 64;
    u8 *sp = e->buf;                              /* 8 setup bytes, data at +16 */
    u8 *dp = e->buf + 16;

    sp[0] = rt; sp[1] = rq; sp[2] = (u8)val; sp[3] = (u8)(val >> 8);
    sp[4] = (u8)idx; sp[5] = (u8)(idx >> 8); sp[6] = (u8)len; sp[7] = (u8)(len >> 8);
    if (len && !in && buf) memcpy(dp, buf, len);
    if (len && in) memset(dp, 0, len);

    qtd_t *t = e->td;
    int n = 0;
    qtd_set(&t[n], (u32)&t[n + 1], QT_PID_SETUP, 0, 8, sp, 0);
    n++;
    int data = -1;
    if (len) {
        data = n;
        qtd_set(&t[n], (u32)&t[n + 1], in ? QT_PID_IN : QT_PID_OUT, 1, len, dp, 0);
        n++;
    }
    int stat = n;
    qtd_set(&t[n], 1, (len && in) ? QT_PID_OUT : QT_PID_IN, 1, 0, NULL, 1);
    n++;
    /* a short IN packet ends the data stage early: carry on with the status stage */
    if (data >= 0) t[data].alt = (u32)&t[stat];

    async_off(e);
    qh_t *q = e->qh_ctrl;
    u32 ha = 0, hp = 0;
    u32 info1 = (ud->addr & 0x7Fu) | (eps_of(ud->speed) << 12) | (1u << 14) |
                (1u << 15) | (mps << 16);
    u32 info2 = 1u << 30;
    if (ud->speed == USB_HS) {
        info1 |= 4u << 28;                        /* NAK reload */
    } else {
        info1 |= 1u << 27;                        /* control endpoint flag for LS/FS */
        tt_of(ud, &ha, &hp);
        info2 |= (ha << 16) | (hp << 23);
    }
    q->link = (u32)q | 2;
    q->info1 = info1;
    q->info2 = info2;
    q->cur = 0;
    q->onext = (u32)&t[0];
    q->oalt = 1;
    q->otoken = 0;
    R32(e->op, 0x04) = 0x3F;                      /* clear stale status */
    R32(e->op, 0x18) = (u32)q;                    /* start the list at our queue head */
    R32(e->op, 0x00) |= CMD_ASE;
    spin(e, 0x04, STS_ASS, STS_ASS, 20);

    u32 t0 = timer_ms();
    int err = 0;
    for (;;) {
        u32 last = *(volatile u32 *)&t[stat].token;
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (*(volatile u32 *)&t[i].token & (QT_HALT | QT_ERRMASK)) bad = 1;
        if (bad) { err = 1; break; }
        if (!(last & QT_ACTIVE)) break;
        if ((i32)(timer_ms() - t0) > 500) { err = 1; break; }
    }
    async_off(e);
    q->otoken = QT_HALT;                          /* park the queue head */
    q->onext = 1;
    if (err) { usb_msleep(2); return -1; }

    int got = len;
    if (len && in) {
        got = (int)len - (int)((t[data].token >> 16) & 0x7FFF);
        if (got < 0) got = 0;
        if (got > len) got = len;
        if (buf) memcpy(buf, dp, (size_t)got);
    }
    return got;
}

/* ---- interrupt endpoints (periodic schedule) --------------------------------- */

static void iep_arm(ehci_t *e, eiep_t *x)
{
    qtd_t *t = x->td;
    qtd_set(t, 1, QT_PID_IN, x->toggle, x->mps, x->buf, 1);
    /* the queue head is idle (overlay inactive): point it at the new qTD */
    x->qh->cur = 0;
    x->qh->oalt = 1;
    x->qh->onext = (u32)t;
    x->qh->otoken = 0;
    (void)e;
}

static int ehci_int_open(usb_dev_t *ud, u8 ep, u16 mps, u8 interval, int tag)
{
    (void)interval;
    ehci_t *e = (ehci_t *)ud->priv;
    eiep_t *x = NULL;
    int slot = -1;
    for (int i = 0; i < NIEP; i++) if (!e->iep[i].used) { slot = i; x = &e->iep[i]; break; }
    if (!x) return -1;
    if (ud->speed != USB_HS && mps > 64) mps = 64;
    if (mps > 1024) mps = 1024;
    if (!x->td)  x->td  = usb_dma(32, 32);
    if (!x->buf) x->buf = usb_dma(64, 64);
    if (!x->td || !x->buf) return -1;
    memset(x->buf, 0, 64);
    x->qh = e->qh_int[slot];
    x->toggle = 0; x->mps = mps; x->tag = tag; x->errors = 0; x->dev = ud;

    qh_t *q = x->qh;
    u32 info1 = (ud->addr & 0x7Fu) | ((u32)(ep & 15) << 8) | (eps_of(ud->speed) << 12) |
                (1u << 14) | ((u32)mps << 16);
    u32 info2 = 1u << 30;
    if (ud->speed == USB_HS) {
        info2 |= 0x01;                            /* start in microframe 0, every frame */
    } else {
        u32 ha, hp;
        tt_of(ud, &ha, &hp);
        info2 |= 0x01u | (0x1Cu << 8) | (ha << 16) | (hp << 23);   /* split: start uF0, complete uF2-4 */
    }
    q->info1 = info1;
    q->info2 = info2;
    iep_arm(e, x);
    x->used = 1;
    return 0;
}

void ehci_tick(void)
{
    for (int k = 0; k < ne; k++) {
        ehci_t *e = &eh[k];
        if (!e->ok || !e->native) continue;
        for (int i = 0; i < NIEP; i++) {
            eiep_t *x = &e->iep[i];
            if (!x->used) continue;
            u32 s = *(volatile u32 *)&x->td->token;
            if (s & QT_ACTIVE) continue;
            if (!(s & (QT_HALT | QT_ERRMASK))) {
                int got = (int)x->mps - (int)((s >> 16) & 0x7FFF);
                if (got > x->mps) got = x->mps;
                if (got > 0) usb_hid_report(x->tag, x->buf, got);
                x->toggle ^= 1;
                x->errors = 0;
            } else if (++x->errors > 20) {
                x->used = 0;                      /* the device is gone */
                x->qh->otoken = QT_HALT;
                x->qh->onext = 1;
                continue;
            }
            iep_arm(e, x);
        }
    }
}

/* ---- devices and ports ---------------------------------------------------------- */

static int new_addr(ehci_t *e)
{
    int a = e->next_addr++;
    if (e->next_addr > 120) e->next_addr = 1;
    return a;
}

static usb_dev_t *ehci_attach_child(usb_dev_t *hub, int port, int speed)
{
    ehci_t *e = (ehci_t *)hub->priv;
    int k;
    for (k = 0; k < 12 && e->kused[k]; k++) ;
    if (k == 12) return NULL;
    usb_dev_t *d = &e->kid[k];
    memset(d, 0, sizeof *d);
    d->hc = &ehci_hc;
    d->priv = e;
    d->speed = (u8)speed;
    d->port = (u8)port;
    d->mps0 = speed == USB_HS ? 64 : 8;
    d->parent = hub;
    int addr = new_addr(e);
    if (ehci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return NULL;
    usb_msleep(12);
    d->addr = (u8)addr;
    e->kused[k] = 1;
    return d;
}

static void ehci_drop_child(usb_dev_t *d)
{
    ehci_t *e = (ehci_t *)d->priv;
    for (int i = 0; i < NIEP; i++)
        if (e->iep[i].used && e->iep[i].dev == d) {
            e->iep[i].used = 0;
            e->iep[i].qh->otoken = QT_HALT;
            e->iep[i].qh->onext = 1;
        }
    for (int k = 0; k < 12; k++) if (&e->kid[k] == d) e->kused[k] = 0;
}

static void ehci_port_up(ehci_t *e, int p)
{
    e->pstate[p] = 2;
    u32 sc = portsc(e, p);
    if (e->ncc && (sc & PS_LS_MASK) == PS_LS_K) {         /* low speed: companion's job */
        portw(e, p, (sc & ~PS_W1C) | PS_OWNER);
        return;
    }
    sc = (sc & ~(PS_W1C | PS_PED)) | PS_RESET;             /* reset the port */
    portw(e, p, sc);
    usb_msleep(55);
    sc = portsc(e, p);
    portw(e, p, sc & ~(PS_W1C | PS_RESET));
    for (int i = 0; i < 20 && (portsc(e, p) & PS_RESET); i++) usb_msleep(2);
    usb_msleep(12);                                        /* reset recovery */
    sc = portsc(e, p);
    portw(e, p, sc);                                       /* writing 1 acks the change bits */
    if (!(sc & PS_PED)) {                                  /* not high speed */
        if (e->ncc) portw(e, p, (portsc(e, p) & ~PS_W1C) | PS_OWNER);
        return;
    }

    usb_dev_t *d = &e->dev[p];
    memset(d, 0, sizeof *d);
    d->hc = &ehci_hc;
    d->priv = e;
    d->speed = USB_HS;
    d->port = (u8)(p + 1);
    d->mps0 = 64;
    d->addr = 0;
    int addr = new_addr(e);
    if (ehci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return;
    usb_msleep(12);
    d->addr = (u8)addr;
    if (usb_enumerate(d) >= 0) e->pstate[p] = 1;
}

static void ehci_scan(ehci_t *e)
{
    for (int p = 0; p < e->nports; p++) {
        u32 sc = portsc(e, p);
        if (sc & PS_W1C) portw(e, p, sc);                  /* writing 1 acks the change bits */
        if (sc & PS_OWNER) continue;                       /* handed to a companion */
        if (sc & PS_CCS) {
            if (!e->pstate[p]) ehci_port_up(e, p);
        } else if (e->pstate[p]) {
            for (int i = 0; i < NIEP; i++)
                if (e->iep[i].used && usb_is_below(e->iep[i].dev, &e->dev[p])) {
                    e->iep[i].used = 0;
                    e->iep[i].qh->otoken = QT_HALT;
                    e->iep[i].qh->onext = 1;
                }
            usb_detach(&e->dev[p]);
            e->pstate[p] = 0;
        }
    }
}

void ehci_service(void)
{
    for (int k = 0; k < ne; k++)
        if (eh[k].ok && eh[k].native) ehci_scan(&eh[k]);
}

/* ---- bring-up ---------------------------------------------------------------------- */

static int ehci_native_start(ehci_t *e)
{
    e->frames  = usb_dma(4096, 4096);
    e->qh_ctrl = usb_dma(sizeof(qh_t), 32);
    e->td      = usb_dma(NQTD * 32, 32);
    e->buf     = usb_dma(1024, 4096);
    if (!e->frames || !e->qh_ctrl || !e->td || !e->buf) return 0;
    for (int i = 0; i < NIEP; i++) {
        e->qh_int[i] = usb_dma(sizeof(qh_t), 32);
        if (!e->qh_int[i]) return 0;
    }

    /* async list: one self-linked queue head (H bit), parked */
    e->qh_ctrl->link = (u32)e->qh_ctrl | 2;
    e->qh_ctrl->info1 = 1u << 15;
    e->qh_ctrl->info2 = 1u << 30;
    e->qh_ctrl->onext = 1; e->qh_ctrl->oalt = 1; e->qh_ctrl->otoken = QT_HALT;

    /* periodic list: every frame slot walks the same chain of parked QHs */
    for (int i = 0; i < NIEP; i++) {
        qh_t *q = e->qh_int[i];
        q->link = (i + 1 < NIEP) ? (((u32)e->qh_int[i + 1]) | 2) : 1;
        q->info2 = 1u << 30;
        q->onext = 1; q->oalt = 1; q->otoken = QT_HALT;
    }
    for (int i = 0; i < 1024; i++) e->frames[i] = ((u32)e->qh_int[0]) | 2;

    R32(e->op, 0x10) = 0;                             /* 32-bit segment */
    R32(e->op, 0x14) = (u32)e->frames;
    R32(e->op, 0x18) = (u32)e->qh_ctrl;
    R32(e->op, 0x08) = 0;                             /* polled */
    R32(e->op, 0x04) = 0x3F;
    R32(e->op, 0x00) = CMD_RS | CMD_PSE | (8u << 16); /* run, periodic on */
    if (spin(e, 0x04, STS_HALT, 0, 100) < 0) return 0;
    R32(e->op, 0x40) = 1;                             /* CONFIGFLAG: ports are ours */
    usb_msleep(5);
    return 1;
}

static int ehci_one(pci_dev_t *pd)
{
    ehci_t *e = &eh[ne];
    memset(e, 0, sizeof *e);

    u32 bar0 = pci_read32(pd->bus, pd->dev, pd->fn, 0x10);
    if ((bar0 & 1) || !(bar0 & ~0xFu)) return 0;
    if (((bar0 >> 1) & 3) == 2 && pci_read32(pd->bus, pd->dev, pd->fn, 0x14)) return 0;
    u8 *cap = (u8 *)(bar0 & ~0xFu);
    pci_enable_bus_master(pd);

    u32 hcs = R32(cap, 4), hcc = R32(cap, 8);
    u8 *op = cap + (*(volatile u8 *)cap);
    e->cap = cap; e->op = op;
    e->ncc = (int)((hcs >> 12) & 15);
    e->nports = (int)(hcs & 15);
    if (e->nports > MAXPRT) e->nports = MAXPRT;
    e->native = e->ncc ? 0 : 1;

    /* BIOS handoff: the extended capability with ID 1 */
    u32 eecp = (hcc >> 8) & 0xFF;
    if (eecp >= 0x40) {
        u32 c = pci_read32(pd->bus, pd->dev, pd->fn, (u8)eecp);
        if ((c & 0xFF) == 1) {
            if (c & (1u << 16)) {
                pci_write32(pd->bus, pd->dev, pd->fn, (u8)eecp, c | (1u << 24));
                for (int i = 0; i < 100; i++) {
                    if (!(pci_read32(pd->bus, pd->dev, pd->fn, (u8)eecp) & (1u << 16))) break;
                    usb_msleep(10);
                }
            }
            /* no SMIs from this controller any more */
            pci_write32(pd->bus, pd->dev, pd->fn, (u8)eecp + 4, 0);
        }
    }

    R32(op, 0x00) &= ~1u;                                   /* stop       */
    for (int i = 0; i < 100 && !(R32(op, 0x04) & STS_HALT); i++) usb_msleep(1);
    R32(op, 0x00) |= CMD_RESET;                             /* reset      */
    for (int i = 0; i < 100 && (R32(op, 0x00) & CMD_RESET); i++) usb_msleep(1);

    if (!e->native) {
        /* companions own the ports */
        R32(op, 0x40) = 0;
        R32(op, 0x08) = 0;
        return 1;
    }

    if (!ehci_native_start(e)) return 0;
    if (R32(op, 0x04) & STS_HALT) return 0;
    for (int p = 0; p < e->nports; p++) {                   /* power the ports */
        u32 sc = portsc(e, p);
        if (!(sc & PS_PP)) portw(e, p, (sc & ~PS_W1C) | PS_PP);
    }
    usb_msleep(120);                                        /* debounce */
    e->next_addr = 1;
    e->ok = 1;
    ne++;
    ehci_scan(e);
    return 1;
}

int ehci_probe(void)
{
    ehci_hc.name = "EHCI";
    ehci_hc.ctrl = ehci_ctrl;
    ehci_hc.int_open = ehci_int_open;
    ehci_hc.set_mps0 = NULL;
    ehci_hc.hub_config = NULL;
    ehci_hc.attach_child = ehci_attach_child;
    ehci_hc.drop_child = ehci_drop_child;

    int found = 0, companion = 0;
    pci_dev_t pd;
    for (int i = 0; i < MAXE; i++) {
        if (!pci_find_class(0x0C, 0x03, 0x20, i, &pd)) break;
        usb_count_controller(1);
        int before = ne;
        if (ehci_one(&pd)) {
            found++;
            if (ne == before) companion = 1;     /* ports went to companions */
        }
    }
    if (companion) { released = 1; usb_msleep(250); }   /* let the companions see their devices */
    return found;
}
