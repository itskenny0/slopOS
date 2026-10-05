/* xhci.c -- USB 3.x host controller (eXtensible Host Controller Interface)
 *
 * Every PC made since about 2012 has one, and on a machine with no
 * EHCI/UHCI at all (most laptops built after 2016) it is the only way to
 * reach any USB port: USB 3, USB 2 and USB 1.1 devices all hang off the
 * same xHCI root ports.
 *
 * The design is the smallest one that works for HID devices:
 *
 *   - no interrupts: the event ring is polled, 100 times a second from the
 *     timer tick and in a tight loop while a command or control transfer
 *     is waiting for its answer;
 *   - one event ring segment, 64-entry rings, a few devices per controller;
 *   - no hubs, no streams, no isochronous transfers.
 *
 * Everything the controller reads (rings, contexts, buffers) is plain
 * kernel memory: the kernel runs with paging off, so a pointer is already
 * a physical address.
 */

#include "usb.h"

#define MAXDEV   8
#define MAXPORT  64
#define NXEP     8
#define RING_N   64
#define MAXCTRL  2

/* TRB types */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define CMD_ENABLE   9
#define CMD_DISABLE  10
#define CMD_ADDRESS  11
#define CMD_CONFIG   12
#define CMD_EVAL     13
#define CMD_RESETEP  14
#define CMD_SETDEQ   16
#define EV_TRANSFER  32
#define EV_CMD       33
#define EV_PORT      34

#define R32(b, o)  (*(volatile u32 *)((u8 *)(b) + (o)))

typedef struct { volatile u32 d[4]; } trb_t;

typedef struct {
    trb_t *r;
    u32    enq;
    u32    cyc;
} ring_t;

struct xhci;

typedef struct {
    int       used;
    struct xhci *x;
    u8        port, speed;
    u8        root, hubdev, hubport;  /* root port, parent slot (0 = none), port on it */
    u32       route;
    u8        is_hub, hub_ports, hub_ttt;
    u32      *in, *out;          /* input / output device context   */
    ring_t    ep0;
    u8       *buf;               /* 4 KB control transfer buffer    */
    usb_dev_t ud;
} xdev_t;

typedef struct {
    int       used;
    int       slot, dci, tag;
    ring_t    ring;
    u8       *buf;
    u32       len;
    int       errors;
    xdev_t   *dev;
} xep_t;

typedef struct xhci {
    int       ok;
    u8       *cap, *op, *rt, *db;
    int       ports, slots, csz;
    u32      *dcbaa;
    ring_t    cmd;
    trb_t    *evr;
    u32       evdeq, evcyc;
    u8        proto[MAXPORT + 1];
    xdev_t    dev[MAXDEV];
    xep_t     ep[NXEP];
    u8        pstate[MAXPORT + 1];      /* 0 empty, 1 in use, 2 failed */
    u8        pdev[MAXPORT + 1];        /* slot of the device on a port */
    volatile int rescan;
    /* waiters */
    volatile int cmd_done, cmd_cc, cmd_slot;
    u32       cmd_ptr;
    volatile int ctl_done, ctl_cc;
    volatile u32 ctl_resid;
    u32       ctl_wait, ctl_data;
    int       id;
} xhci_t;

static xhci_t xh[MAXCTRL];
static int    nx;

static usb_hc_t xhci_hc;

/* ---- rings ------------------------------------------------------------ */

static int ring_init(ring_t *g)
{
    if (!g->r) {
        g->r = usb_dma(RING_N * 16, 1024);
        if (!g->r) return -1;
    } else {
        memset((void *)g->r, 0, RING_N * 16);
    }
    g->enq = 0;
    g->cyc = 1;
    g->r[RING_N - 1].d[0] = (u32)g->r;
    g->r[RING_N - 1].d[3] = (TRB_LINK << 10) | 2;          /* toggle cycle */
    return 0;
}

/* Queue one TRB; returns its address. The cycle bit goes in last. */
static u32 ring_put(ring_t *g, u32 d0, u32 d1, u32 d2, u32 d3)
{
    trb_t *t = &g->r[g->enq];
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2;
    __asm__ volatile("" ::: "memory");
    t->d[3] = (d3 & ~1u) | g->cyc;
    u32 addr = (u32)t;
    if (++g->enq == RING_N - 1) {
        trb_t *l = &g->r[RING_N - 1];
        l->d[3] = (l->d[3] & ~1u) | g->cyc;
        g->enq = 0;
        g->cyc ^= 1;
    }
    return addr;
}

/* ---- events ----------------------------------------------------------- */

static xep_t *find_ep(xhci_t *x, int slot, int dci)
{
    for (int i = 0; i < NXEP; i++)
        if (x->ep[i].used && x->ep[i].slot == slot && x->ep[i].dci == dci)
            return &x->ep[i];
    return NULL;
}

static void ep_queue(xhci_t *x, xep_t *e)
{
    ring_put(&e->ring, (u32)e->buf, 0, e->len,
             (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));      /* IOC + ISP */
    R32(x->db, 4 * e->slot) = (u32)e->dci;
}

static void handle_event(xhci_t *x, trb_t *t)
{
    u32 d0 = t->d[0], d2 = t->d[2], d3 = t->d[3];
    u32 type = (d3 >> 10) & 63;

    if (type == EV_CMD) {
        if (d0 == x->cmd_ptr) {
            x->cmd_cc = (int)(d2 >> 24);
            x->cmd_slot = (int)(d3 >> 24);
            x->cmd_done = 1;
        }
    } else if (type == EV_TRANSFER) {
        int slot = (int)(d3 >> 24), dci = (int)((d3 >> 16) & 31);
        int cc = (int)(d2 >> 24);
        if (dci == 1) {
            if (d0 == x->ctl_data) x->ctl_resid = d2 & 0xFFFFFF;
            if (d0 == x->ctl_wait || (cc != 1 && cc != 13)) {
                x->ctl_cc = cc;
                x->ctl_done = 1;
            }
        } else {
            xep_t *e = find_ep(x, slot, dci);
            if (!e) return;
            if (cc == 1 || cc == 13) {
                e->errors = 0;
                u32 got = e->len - (d2 & 0xFFFFFF);
                if (got > e->len) got = e->len;
                usb_hid_report(e->tag, e->buf, (int)got);
                ep_queue(x, e);
            } else if (++e->errors < 4 && cc != 6) {
                ep_queue(x, e);               /* glitch: try again        */
            }
        }
    } else if (type == EV_PORT) {
        x->rescan = 1;
    }
}

static void xhci_events(xhci_t *x)
{
    int any = 0;
    for (int guard = 0; guard < RING_N; guard++) {
        trb_t *t = &x->evr[x->evdeq];
        if ((t->d[3] & 1) != x->evcyc) break;
        handle_event(x, t);
        any = 1;
        if (++x->evdeq == RING_N) { x->evdeq = 0; x->evcyc ^= 1; }
    }
    if (any) {
        R32(x->rt, 0x20 + 0x18) = ((u32)&x->evr[x->evdeq]) | 8;   /* clear EHB */
        R32(x->rt, 0x20 + 0x1C) = 0;
    }
}

void xhci_tick(void)
{
    for (int i = 0; i < nx; i++)
        if (xh[i].ok) xhci_events(&xh[i]);
}

/* ---- commands --------------------------------------------------------- */

static int xhci_cmd(xhci_t *x, u32 d0, u32 d1, u32 d2, u32 d3, int *slot)
{
    x->cmd_done = 0;
    x->cmd_ptr = ring_put(&x->cmd, d0, d1, d2, d3);
    R32(x->db, 0) = 0;
    u32 t0 = timer_ms();
    while (!x->cmd_done) {
        xhci_events(x);
        if ((i32)(timer_ms() - t0) > 1500) return -1;
    }
    if (slot) *slot = x->cmd_slot;
    return x->cmd_cc;
}

/* ---- control transfers ------------------------------------------------ */

static int xhci_ctrl(usb_dev_t *ud, u8 rt, u8 rq, u16 val, u16 idx,
                     void *buf, u16 len)
{
    xdev_t *dv = (xdev_t *)ud->priv;
    xhci_t *x = dv->x;
    int slot = (int)(dv - x->dev) + 1;
    int in = (rt & 0x80) != 0;

    if (len > 4096) return -1;
    if (len && !in && buf) memcpy(dv->buf, buf, len);
    if (len && in) memset(dv->buf, 0, len);

    x->ctl_done = 0;
    x->ctl_data = 0;
    x->ctl_resid = 0;

    u32 trt = len ? (in ? 3u : 2u) : 0u;
    ring_put(&dv->ep0,
             (u32)rt | ((u32)rq << 8) | ((u32)val << 16),
             (u32)idx | ((u32)len << 16), 8,
             (TRB_SETUP << 10) | (1 << 6) | (trt << 16));
    if (len)
        x->ctl_data = ring_put(&dv->ep0, (u32)dv->buf, 0, len,
                               (TRB_DATA << 10) | (1 << 2) | (in ? (1u << 16) : 0));
    u32 status_dir = (len && in) ? 0u : (1u << 16);
    x->ctl_wait = ring_put(&dv->ep0, 0, 0, 0,
                           (TRB_STATUS << 10) | (1 << 5) | status_dir);
    R32(x->db, 4 * slot) = 1;

    u32 t0 = timer_ms();
    while (!x->ctl_done) {
        xhci_events(x);
        if ((i32)(timer_ms() - t0) > 1000) return -1;
    }
    int cc = x->ctl_cc;
    if (cc == 6) {
        /* stall: the endpoint halted. Reset it and skip past the dead TD. */
        xhci_cmd(x, 0, 0, 0, (CMD_RESETEP << 10) | (1u << 16) | ((u32)slot << 24), NULL);
        xhci_cmd(x, ((u32)&dv->ep0.r[dv->ep0.enq]) | dv->ep0.cyc, 0, 0,
                 (CMD_SETDEQ << 10) | (1u << 16) | ((u32)slot << 24), NULL);
        return -1;
    }
    if (cc != 1 && cc != 13) return -1;
    if (len && in && buf) memcpy(buf, dv->buf, len);
    return (int)(len - (x->ctl_resid > len ? len : x->ctl_resid));
}

/* ---- context helpers --------------------------------------------------- */

static u32 *ctx(xhci_t *x, u32 *base, int index)
{
    return (u32 *)((u8 *)base + (u32)index * (u32)x->csz);
}

static int xhci_set_mps0(usb_dev_t *ud, u16 mps)
{
    xdev_t *dv = (xdev_t *)ud->priv;
    xhci_t *x = dv->x;
    int slot = (int)(dv - x->dev) + 1;

    memset(dv->in, 0, 33 * (u32)x->csz);
    u32 *icc = ctx(x, dv->in, 0);
    icc[1] = 1u << 1;                                 /* A1: endpoint 0   */
    u32 *s = ctx(x, dv->in, 1);
    u32 *o = ctx(x, dv->out, 0);
    for (int i = 0; i < 8; i++) s[i] = o[i];
    u32 *e = ctx(x, dv->in, 2);
    u32 *oe = ctx(x, dv->out, 1);
    for (int i = 0; i < 8; i++) e[i] = oe[i];
    e[1] = (e[1] & 0xFFFFu) | ((u32)mps << 16);
    return xhci_cmd(x, (u32)dv->in, 0, 0,
                    (CMD_EVAL << 10) | ((u32)slot << 24), NULL) == 1 ? 0 : -1;
}

static int xhci_int_open(usb_dev_t *ud, u8 epaddr, u16 mps, u8 interval, int tag)
{
    xdev_t *dv = (xdev_t *)ud->priv;
    xhci_t *x = dv->x;
    int slot = (int)(dv - x->dev) + 1;
    int dci = (epaddr & 15) * 2 + 1;

    xep_t *e = NULL;
    for (int i = 0; i < NXEP; i++) if (!x->ep[i].used) { e = &x->ep[i]; break; }
    if (!e) return -1;

    if (mps > 64) mps = 64;
    if (mps < 4) mps = 8;
    if (ring_init(&e->ring)) return -1;
    if (!e->buf) e->buf = usb_dma(64, 64);
    if (!e->buf) return -1;
    memset(e->buf, 0, 64);
    e->len = mps;

    int iv;
    if (ud->speed == USB_LS || ud->speed == USB_FS) {
        int b = interval ? interval : 8;
        iv = 3;
        while (b > 1 && iv < 10) { b >>= 1; iv++; }
    } else {
        iv = interval ? interval - 1 : 3;
        if (iv > 15) iv = 15;
    }

    memset(dv->in, 0, 33 * (u32)x->csz);
    u32 *icc = ctx(x, dv->in, 0);
    icc[1] = 1u | (1u << dci);
    u32 *s = ctx(x, dv->in, 1);
    u32 *o = ctx(x, dv->out, 0);
    for (int i = 0; i < 8; i++) s[i] = o[i];
    u32 entries = s[0] >> 27;
    if ((u32)dci > entries) entries = (u32)dci;
    s[0] = (s[0] & 0x07FFFFFFu) | (entries << 27);

    u32 *c = ctx(x, dv->in, dci + 1);
    c[0] = (u32)iv << 16;
    c[1] = (3u << 1) | (7u << 3) | ((u32)mps << 16);      /* CErr 3, int IN */
    c[2] = (u32)e->ring.r | 1;
    c[3] = 0;
    c[4] = (u32)mps | ((u32)mps << 16);

    int cc = xhci_cmd(x, (u32)dv->in, 0, 0,
                      (CMD_CONFIG << 10) | ((u32)slot << 24), NULL);
    if (cc != 1) return -1;

    e->used = 1;
    e->slot = slot;
    e->dci = dci;
    e->tag = tag;
    e->errors = 0;
    e->dev = dv;
    ep_queue(x, e);
    return 0;
}

/* ---- ports and devices ------------------------------------------------- */

#define PORTSC(x, p)  R32((x)->op, 0x400 + 0x10 * ((p) - 1))
#define PORT_CLR      0x80FF0012u   /* RW1C change bits, PED, PR, LWS, WPR */
#define PORT_CHANGES  0x00FE0000u

static void port_ack(xhci_t *x, int p)
{
    u32 sc = PORTSC(x, p);
    PORTSC(x, p) = (sc & ~PORT_CLR) | (sc & PORT_CHANGES);
}

static int port_reset(xhci_t *x, int p)
{
    u32 sc = PORTSC(x, p);
    if (x->proto[p] == 3) {
        /* USB 3 ports train by themselves; wait for the link to come up */
        for (int i = 0; i < 60 && !(sc & 2); i++) { usb_msleep(10); sc = PORTSC(x, p); }
        if (sc & 2) { port_ack(x, p); return 0; }
        PORTSC(x, p) = (sc & ~PORT_CLR) | (1u << 31);           /* warm reset */
    } else {
        PORTSC(x, p) = (sc & ~PORT_CLR) | (1u << 4);            /* reset      */
    }
    u32 t0 = timer_ms();
    for (;;) {
        sc = PORTSC(x, p);
        if (sc & ((1u << 21) | (1u << 19))) break;              /* PRC / WRC  */
        if ((i32)(timer_ms() - t0) > 600) return -1;
        usb_msleep(5);
    }
    port_ack(x, p);
    usb_msleep(20);                                             /* recovery   */
    sc = PORTSC(x, p);
    return (sc & 2) ? 0 : -1;
}

static int xdev_alloc(xhci_t *x, xdev_t *dv)
{
    if (!dv->in)  dv->in  = usb_dma(33 * 64 + 64, 4096);
    if (!dv->out) dv->out = usb_dma(32 * 64 + 64, 4096);
    if (!dv->buf) dv->buf = usb_dma(4096, 4096);
    if (!dv->in || !dv->out || !dv->buf) return -1;
    memset(dv->in, 0, 33 * 64);
    memset(dv->out, 0, 32 * 64);
    return ring_init(&dv->ep0);
}

/* Enable a slot, give it its contexts and address it. `route` is the xHCI
 * route string (4 bits per hub tier), `root` the root port number; for a
 * device behind a hub, `hubdev`/`hubport` name the hub's slot and port so
 * the controller can reach it through the hub's transaction translator. */
static xdev_t *xhci_new_dev(xhci_t *x, int root, int speed, u32 route,
                            xdev_t *hub, int hubport, int hub_is_hs)
{
    int slot = 0;
    if (xhci_cmd(x, 0, 0, 0, CMD_ENABLE << 10, &slot) != 1) return NULL;
    if (slot < 1 || slot > MAXDEV) return NULL;
    xdev_t *dv = &x->dev[slot - 1];
    memset(&dv->ud, 0, sizeof dv->ud);
    dv->x = x;
    dv->used = 1;
    dv->port = (u8)root;
    dv->root = (u8)root;
    dv->speed = (u8)speed;
    dv->route = route;
    dv->is_hub = 0;
    dv->hubdev = 0; dv->hubport = 0;
    if (xdev_alloc(x, dv) < 0) return NULL;
    x->dcbaa[slot * 2]     = (u32)dv->out;
    x->dcbaa[slot * 2 + 1] = 0;

    u16 mps0 = speed == USB_SS ? 512 : speed == USB_LS ? 8 : speed == USB_HS ? 64 : 8;

    u32 *icc = ctx(x, dv->in, 0);
    icc[1] = 3;                                                  /* A0 + A1 */
    u32 *s = ctx(x, dv->in, 1);
    s[0] = (1u << 27) | ((u32)speed << 20) | (route & 0xFFFFF);
    s[1] = (u32)root << 16;
    if (hub && (speed == USB_LS || speed == USB_FS) && hub_is_hs) {
        dv->hubdev = (u8)((hub - x->dev) + 1);
        dv->hubport = (u8)hubport;
        s[2] = (u32)dv->hubdev | ((u32)hubport << 8);
    }
    u32 *e = ctx(x, dv->in, 2);
    e[1] = (3u << 1) | (4u << 3) | ((u32)mps0 << 16);
    e[2] = (u32)dv->ep0.r | 1;
    e[4] = 8;

    if (xhci_cmd(x, (u32)dv->in, 0, 0,
                 (CMD_ADDRESS << 10) | ((u32)slot << 24), NULL) != 1) {
        xhci_cmd(x, 0, 0, 0, (CMD_DISABLE << 10) | ((u32)slot << 24), NULL);
        x->dcbaa[slot * 2] = 0;
        dv->used = 0;
        return NULL;
    }

    dv->ud.hc = &xhci_hc;
    dv->ud.priv = dv;
    dv->ud.speed = (u8)speed;
    dv->ud.port = (u8)root;
    dv->ud.mps0 = mps0;
    dv->ud.tags = 0;
    dv->ud.addr = (u8)(ctx(x, dv->out, 0)[3] & 0xFF);
    return dv;
}

static int xhci_hub_config(usb_dev_t *ud, int nports, int ttt)
{
    xdev_t *dv = (xdev_t *)ud->priv;
    xhci_t *x = dv->x;
    int slot = (int)(dv - x->dev) + 1;

    memset(dv->in, 0, 33 * (u32)x->csz);
    u32 *icc = ctx(x, dv->in, 0);
    icc[1] = 1;                                              /* A0: slot ctx */
    u32 *s = ctx(x, dv->in, 1);
    u32 *o = ctx(x, dv->out, 0);
    for (int i = 0; i < 8; i++) s[i] = o[i];
    s[0] |= 1u << 26;                                        /* Hub          */
    s[1] = (s[1] & 0x00FFFFFFu) | ((u32)nports << 24);
    s[2] = (s[2] & ~(3u << 16)) | ((u32)(ttt & 3) << 16);
    if (xhci_cmd(x, (u32)dv->in, 0, 0,
                 (CMD_CONFIG << 10) | ((u32)slot << 24), NULL) != 1) return -1;
    dv->is_hub = 1;
    dv->hub_ports = (u8)nports;
    return 0;
}

static usb_dev_t *xhci_attach_child(usb_dev_t *hub, int port, int speed)
{
    xdev_t *h = (xdev_t *)hub->priv;
    xhci_t *x = h->x;
    /* route string: this hub's own route plus the new port in the next
     * free nibble */
    u32 r = h->route;
    int shift = 0;
    while (shift < 20 && ((r >> shift) & 15)) shift += 4;
    if (shift >= 20) return NULL;
    r |= (u32)(port & 15) << shift;
    xdev_t *dv = xhci_new_dev(x, h->root, speed, r, h, port, hub->speed == USB_HS);
    return dv ? &dv->ud : NULL;
}

static void xhci_drop_child(usb_dev_t *ud)
{
    xdev_t *dv = (xdev_t *)ud->priv;
    xhci_t *x = dv->x;
    int slot = (int)(dv - x->dev) + 1;
    for (int i = 0; i < NXEP; i++)
        if (x->ep[i].used && x->ep[i].slot == slot) x->ep[i].used = 0;
    xhci_cmd(x, 0, 0, 0, (CMD_DISABLE << 10) | ((u32)slot << 24), NULL);
    x->dcbaa[slot * 2] = 0;
    dv->used = 0;
}

static void xhci_port_up(xhci_t *x, int p)
{
    if (x->pstate[p]) return;
    x->pstate[p] = 2;                   /* failed unless proven otherwise */

    if (port_reset(x, p) < 0) return;
    u32 sc = PORTSC(x, p);
    int speed = (int)((sc >> 10) & 15);
    if (speed < 1 || speed > 4) return;

    xdev_t *dv = xhci_new_dev(x, p, speed, 0, NULL, 0, 0);
    if (!dv) return;
    int slot = (int)(dv - x->dev) + 1;
    int r = usb_enumerate(&dv->ud);
    if (r >= 0) {
        x->pstate[p] = 1;
        x->pdev[p] = (u8)slot;
    }
}

static void xhci_port_down(xhci_t *x, int p)
{
    int slot = x->pdev[p];
    if (slot) {
        xdev_t *dv = &x->dev[slot - 1];
        for (int i = 0; i < NXEP; i++)
            if (x->ep[i].used && x->ep[i].slot == slot) x->ep[i].used = 0;
        usb_detach(&dv->ud);              /* also drops everything behind a hub */
        xhci_cmd(x, 0, 0, 0, (CMD_DISABLE << 10) | ((u32)slot << 24), NULL);
        x->dcbaa[slot * 2] = 0;
        dv->used = 0;
    }
    x->pdev[p] = 0;
    x->pstate[p] = 0;
}

static void xhci_scan(xhci_t *x)
{
    for (int p = 1; p <= x->ports && p <= MAXPORT; p++) {
        u32 sc = PORTSC(x, p);
        if (sc & PORT_CHANGES) port_ack(x, p);
        if (sc & 1) {
            if (!x->pstate[p]) xhci_port_up(x, p);
        } else if (x->pstate[p]) {
            xhci_port_down(x, p);
        }
    }
}

void xhci_service(void)
{
    for (int i = 0; i < nx; i++) {
        xhci_t *x = &xh[i];
        if (!x->ok || !x->rescan) continue;
        x->rescan = 0;
        xhci_events(x);
        xhci_scan(x);
    }
}

/* ---- controller bring-up ------------------------------------------------ */

static int wait_bit(xhci_t *x, u32 off, u32 mask, u32 want, u32 ms)
{
    u32 t0 = timer_ms();
    while ((R32(x->op, off) & mask) != want) {
        if ((i32)(timer_ms() - t0) > (i32)ms) return -1;
    }
    return 0;
}

static int xhci_init_one(pci_dev_t *pd, int id)
{
    xhci_t *x = &xh[id];
    memset(x, 0, sizeof *x);
    x->id = id;

    u32 bar0 = pci_read32(pd->bus, pd->dev, pd->fn, 0x10);
    if (bar0 & 1) return 0;
    if (((bar0 >> 1) & 3) == 2 && pci_read32(pd->bus, pd->dev, pd->fn, 0x14))
        return 0;                    /* mapped above 4 GB: out of reach */
    u32 base = bar0 & ~0xFu;
    if (!base) return 0;
    pci_enable_bus_master(pd);

    /* Intel 7/8/9-series chipsets share the USB 2 sockets between EHCI and
     * xHCI and start with them routed to EHCI. Flip every port to xHCI. */
    if (pd->vendor == 0x8086 &&
        (pd->device == 0x1E31 || pd->device == 0x8C31 || pd->device == 0x9C31 ||
         pd->device == 0x8CB1 || pd->device == 0x9CB1)) {
        u32 u3 = pci_read32(pd->bus, pd->dev, pd->fn, 0xDC);
        u32 u2 = pci_read32(pd->bus, pd->dev, pd->fn, 0xD4);
        pci_write32(pd->bus, pd->dev, pd->fn, 0xD8, u3);
        pci_write32(pd->bus, pd->dev, pd->fn, 0xD0, u2);
    }

    x->cap = (u8 *)base;
    u32 caplen = *(volatile u8 *)x->cap;
    u32 hcs1 = R32(x->cap, 4), hcs2 = R32(x->cap, 8), hcc = R32(x->cap, 0x10);
    x->op = x->cap + caplen;
    x->db = x->cap + (R32(x->cap, 0x14) & ~3u);
    x->rt = x->cap + (R32(x->cap, 0x18) & ~0x1Fu);
    x->slots = (int)(hcs1 & 0xFF);
    x->ports = (int)(hcs1 >> 24);
    x->csz   = (hcc & (1u << 2)) ? 64 : 32;
    if (x->ports > MAXPORT) x->ports = MAXPORT;
    if (x->slots < 1 || x->ports < 1) return 0;

    /* extended capabilities: take the controller from the BIOS, and learn
     * which ports are USB 2 and which are USB 3 */
    u32 xecp = (hcc >> 16) & 0xFFFF;
    if (xecp) {
        u8 *p = x->cap + xecp * 4;
        for (int guard = 0; guard < 64; guard++) {
            u32 c0 = R32(p, 0);
            u32 id_ = c0 & 0xFF, next = (c0 >> 8) & 0xFF;
            if (id_ == 1) {
                if (c0 & (1u << 16)) {                 /* BIOS owns it */
                    R32(p, 0) = c0 | (1u << 24);
                    for (int i = 0; i < 100 && (R32(p, 0) & (1u << 16)); i++)
                        usb_msleep(10);
                }
                R32(p, 4) = 0xE0000000u;               /* no more SMIs  */
            } else if (id_ == 2) {
                u32 c2 = R32(p, 8);
                u32 off = c2 & 0xFF, cnt = (c2 >> 8) & 0xFF, major = c0 >> 24;
                for (u32 i = 0; i < cnt; i++)
                    if (off + i <= MAXPORT) x->proto[off + i] = (u8)major;
            }
            if (!next) break;
            p += next * 4;
        }
    }

    /* halt, reset */
    R32(x->op, 0) &= ~1u;
    if (wait_bit(x, 4, 1, 1, 500) < 0) return 0;
    R32(x->op, 0) |= 2u;
    if (wait_bit(x, 0, 2, 0, 1000) < 0) return 0;
    if (wait_bit(x, 4, 1u << 11, 0, 1000) < 0) return 0;

    int use = x->slots < MAXDEV ? x->slots : MAXDEV;
    R32(x->op, 0x38) = (u32)use;

    x->dcbaa = usb_dma(4096, 4096);
    if (!x->dcbaa) return 0;
    {
        u32 spb = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
        if (spb) {
            u32 *arr = usb_dma(spb * 8, 64);
            if (!arr) return 0;
            for (u32 i = 0; i < spb; i++) {
                void *pg = usb_dma(4096, 4096);
                if (!pg) return 0;
                arr[i * 2] = (u32)pg;
            }
            x->dcbaa[0] = (u32)arr;
        }
    }
    R32(x->op, 0x30) = (u32)x->dcbaa;
    R32(x->op, 0x34) = 0;

    if (ring_init(&x->cmd)) return 0;
    R32(x->op, 0x18) = (u32)x->cmd.r | 1;
    R32(x->op, 0x1C) = 0;

    x->evr = usb_dma(RING_N * 16, 1024);
    u32 *erst = usb_dma(16, 64);
    if (!x->evr || !erst) return 0;
    erst[0] = (u32)x->evr; erst[1] = 0; erst[2] = RING_N; erst[3] = 0;
    x->evdeq = 0;
    x->evcyc = 1;
    R32(x->rt, 0x20 + 0x08) = 1;
    R32(x->rt, 0x20 + 0x18) = (u32)x->evr;
    R32(x->rt, 0x20 + 0x1C) = 0;
    R32(x->rt, 0x20 + 0x10) = (u32)erst;
    R32(x->rt, 0x20 + 0x14) = 0;

    R32(x->op, 0) = 1;                                  /* run */
    if (wait_bit(x, 4, 1, 0, 500) < 0) return 0;

    x->ok = 1;
    for (int p = 1; p <= x->ports; p++) {
        u32 sc = PORTSC(x, p);
        if (!(sc & (1u << 9))) {
            PORTSC(x, p) = (sc & ~PORT_CLR) | (1u << 9);   /* power on */
        }
    }
    usb_msleep(120);                                       /* debounce */
    xhci_scan(x);
    return 1;
}

int xhci_probe(void)
{
    xhci_hc.name = "xHCI";
    xhci_hc.ctrl = xhci_ctrl;
    xhci_hc.int_open = xhci_int_open;
    xhci_hc.set_mps0 = xhci_set_mps0;
    xhci_hc.hub_config = xhci_hub_config;
    xhci_hc.attach_child = xhci_attach_child;
    xhci_hc.drop_child = xhci_drop_child;

    int found = 0;
    pci_dev_t pd;
    for (int i = 0; i < MAXCTRL; i++) {
        if (!pci_find_class(0x0C, 0x03, 0x30, i, &pd)) break;
        if (xhci_init_one(&pd, nx)) { nx++; found++; usb_count_controller(0); }
    }
    return found;
}
