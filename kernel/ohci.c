/* ohci.c -- USB 1.1 host controller, the other flavour (memory mapped)
 *
 * OHCI is what AMD, NVIDIA, SiS, ALi, OPTi and Apple chipsets used, and it
 * is also the usual companion controller next to an EHCI one on non-Intel
 * boards. Unlike UHCI the hardware does more: it walks endpoint descriptors
 * (EDs) with a queue of transfer descriptors (TDs) each.
 *
 *   HCCA.int_table[0..31] --> ED chain (every interrupt endpoint, 1 ms)
 *   control list          --> one ED, rewritten for every control transfer
 *
 * Polled, no interrupts, no bulk or isochronous. Hubs are supported.
 */
#include "usb.h"

#define MAXO  4
#define NIEP  4
#define NTD   24
#define R32(b, o)  (*(volatile u32 *)((u8 *)(b) + (o)))

typedef struct { volatile u32 flags, tail, head, next; } oed_t;        /* 16 B */
typedef struct { volatile u32 flags, cbp, next, be; } otd_t;           /* 16 B */

typedef struct {
    int       used, errors, cur;
    oed_t    *ed;
    otd_t    *td[2];
    u8       *buf;
    u16       mps;
    int       tag;
    usb_dev_t *dev;
} oiep_t;

typedef struct {
    int       ok;
    u8       *reg;
    int       nports;
    u8       *hcca;
    oed_t    *ed_ctl;
    otd_t    *td;                  /* NTD + 1 for control transfers */
    u8       *buf;
    oiep_t    iep[NIEP];
    usb_dev_t dev[8];
    usb_dev_t kid[12];
    u8        kused[12];
    u8        pstate[8];
    int       next_addr;
} ohci_t;

static ohci_t oh[MAXO];
static int    no;
static usb_hc_t ohci_hc;

#define CC(f)  ((f) >> 28)

static int ohci_ctrl(usb_dev_t *ud, u8 rt, u8 rq, u16 val, u16 idx,
                     void *buf, u16 len)
{
    ohci_t *o = (ohci_t *)ud->priv;
    int in = (rt & 0x80) != 0;
    u32 mps = ud->mps0 ? ud->mps0 : 8;
    if (len > 512) return -1;
    u8 *sp = o->buf, *dp = o->buf + 16;

    sp[0] = rt; sp[1] = rq; sp[2] = (u8)val; sp[3] = (u8)(val >> 8);
    sp[4] = (u8)idx; sp[5] = (u8)(idx >> 8); sp[6] = (u8)len; sp[7] = (u8)(len >> 8);
    if (len && !in && buf) memcpy(dp, buf, len);
    if (len && in) memset(dp, 0, len);

    int n = 0;
    o->td[n].flags = (0xFu << 28) | (7u << 21) | (2u << 24) | (0u << 19) | (1u << 18);
    o->td[n].cbp = (u32)sp; o->td[n].be = (u32)sp + 7;
    n++;
    int tog = 1;
    for (u32 off = 0; off < len; off += mps) {
        u32 c = len - off < mps ? len - off : mps;
        if (n >= NTD - 2) return -1;
        o->td[n].flags = (0xFu << 28) | (7u << 21) | ((u32)(2 | tog) << 24) |
                         ((in ? 2u : 1u) << 19) | (1u << 18);
        o->td[n].cbp = (u32)(dp + off); o->td[n].be = (u32)(dp + off + c - 1);
        tog ^= 1;
        n++;
    }
    o->td[n].flags = (0xFu << 28) | (7u << 21) | (3u << 24) |
                     (((len && in) ? 1u : 2u) << 19);
    o->td[n].cbp = 0; o->td[n].be = 0;
    n++;
    for (int i = 0; i < n; i++) o->td[i].next = (u32)&o->td[i + 1];
    o->td[n].next = 0;
    o->td[n].flags = 0;

    oed_t *e = o->ed_ctl;
    e->flags = (u32)ud->addr | (mps << 16) | (ud->speed == USB_LS ? (1u << 13) : 0);
    e->tail = (u32)&o->td[n];
    e->head = (u32)&o->td[0];
    R32(o->reg, 0x08) = 2;                              /* control list filled */

    u32 t0 = timer_ms();
    int err = 0;
    for (;;) {
        u32 h = e->head;
        if (h & 1) { err = 1; break; }                  /* halted */
        if ((h & ~0xFu) == (u32)&o->td[n]) break;       /* all retired */
        if ((i32)(timer_ms() - t0) > 500) { err = 1; break; }
    }
    if (!err)
        for (int i = 0; i < n; i++) {
            u32 c = CC(o->td[i].flags);
            if (c != 0) { err = 1; break; }
        }
    if (err) {
        e->flags |= 1u << 14;                           /* skip */
        usb_msleep(2);
        e->head = e->tail;
        return -1;
    }
    if (len && in && buf) memcpy(buf, dp, len);
    return len;
}

static void iep_arm(oiep_t *e)
{
    otd_t *t = e->td[e->cur], *dummy = e->td[e->cur ^ 1];
    t->flags = (0xFu << 28) | (7u << 21) | (2u << 19) | (1u << 18);   /* IN, toggle from ED */
    t->cbp = (u32)e->buf; t->be = (u32)e->buf + e->mps - 1;
    t->next = (u32)dummy;
    dummy->flags = 0xF0000000u; dummy->next = 0; dummy->cbp = 0; dummy->be = 0;
    e->ed->tail = (u32)dummy;
}

static int ohci_int_open(usb_dev_t *ud, u8 ep, u16 mps, u8 interval, int tag)
{
    (void)interval;
    ohci_t *o = (ohci_t *)ud->priv;
    oiep_t *e = NULL;
    int idx = 0;
    for (; idx < NIEP; idx++) if (!o->iep[idx].used) { e = &o->iep[idx]; break; }
    if (!e) return -1;
    if (mps > 64) mps = 64;
    if (!e->td[0]) { e->td[0] = usb_dma(16, 16); e->td[1] = usb_dma(16, 16); }
    if (!e->buf) e->buf = usb_dma(64, 64);
    if (!e->td[0] || !e->td[1] || !e->buf) return -1;
    memset(e->buf, 0, 64);
    e->mps = mps; e->tag = tag; e->errors = 0; e->dev = ud; e->cur = 0;

    oed_t *ed = e->ed;
    ed->flags = (u32)ud->addr | ((u32)(ep & 15) << 7) | (2u << 11) |
                (ud->speed == USB_LS ? (1u << 13) : 0) | ((u32)mps << 16) | (1u << 14);
    ed->head = (u32)e->td[0];
    ed->tail = (u32)e->td[1];
    iep_arm(e);
    ed->head = (u32)e->td[0];                           /* clears halt + toggle */
    ed->flags &= ~(1u << 14);                           /* un-skip */
    e->used = 1;
    return 0;
}

void ohci_tick(void)
{
    for (int k = 0; k < no; k++) {
        ohci_t *o = &oh[k];
        if (!o->ok) continue;
        for (int i = 0; i < NIEP; i++) {
            oiep_t *e = &o->iep[i];
            if (!e->used) continue;
            otd_t *t = e->td[e->cur];
            u32 c = CC(t->flags);
            if (c == 0xF) continue;                      /* not yet done */
            if (c == 0) {
                int got = e->mps;
                if (t->cbp) got = (int)(t->cbp - (u32)e->buf);
                else got = (int)((t->be - (u32)e->buf) + 1);
                if (got > 0 && got <= e->mps) usb_hid_report(e->tag, e->buf, got);
                e->errors = 0;
            } else if (++e->errors > 20) {
                e->used = 0;
                e->ed->flags |= 1u << 14;
                continue;
            }
            e->cur ^= 1;
            if (e->ed->head & 1) e->ed->head &= ~3u;     /* clear halt */
            iep_arm(e);
        }
    }
}

/* ---- ports ---------------------------------------------------------------- */

#define PS(o, p)  R32((o)->reg, 0x54 + 4 * (p))

static usb_dev_t *ohci_attach_child(usb_dev_t *hub, int port, int speed)
{
    ohci_t *o = (ohci_t *)hub->priv;
    int k;
    for (k = 0; k < 12 && o->kused[k]; k++) ;
    if (k == 12) return NULL;
    usb_dev_t *d = &o->kid[k];
    memset(d, 0, sizeof *d);
    d->hc = &ohci_hc; d->priv = o;
    d->speed = (u8)(speed == USB_LS ? USB_LS : USB_FS);
    d->port = (u8)port; d->mps0 = 8; d->parent = hub;
    int addr = o->next_addr++;
    if (o->next_addr > 120) o->next_addr = 1;
    if (ohci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return NULL;
    usb_msleep(12);
    d->addr = (u8)addr;
    o->kused[k] = 1;
    return d;
}

static void ohci_drop_child(usb_dev_t *d)
{
    ohci_t *o = (ohci_t *)d->priv;
    for (int i = 0; i < NIEP; i++)
        if (o->iep[i].used && o->iep[i].dev == d) {
            o->iep[i].used = 0; o->iep[i].ed->flags |= 1u << 14;
        }
    for (int k = 0; k < 12; k++) if (&o->kid[k] == d) o->kused[k] = 0;
}

static void ohci_port_up(ohci_t *o, int p)
{
    o->pstate[p] = 2;
    PS(o, p) = 1u << 4;                                  /* reset */
    for (int i = 0; i < 100 && !(PS(o, p) & (1u << 20)); i++) usb_msleep(2);
    PS(o, p) = 1u << 20;                                 /* clear PRSC */
    usb_msleep(12);
    u32 sc = PS(o, p);
    if (!(sc & 2)) { PS(o, p) = 2; usb_msleep(5); sc = PS(o, p); }
    if (!(sc & 2)) return;

    usb_dev_t *d = &o->dev[p];
    memset(d, 0, sizeof *d);
    d->hc = &ohci_hc; d->priv = o;
    d->speed = (sc & (1u << 9)) ? USB_LS : USB_FS;
    d->port = (u8)(p + 1); d->mps0 = 8;
    int addr = o->next_addr++;
    if (o->next_addr > 120) o->next_addr = 1;
    if (ohci_ctrl(d, 0x00, 5, (u16)addr, 0, NULL, 0) < 0) return;
    usb_msleep(12);
    d->addr = (u8)addr;
    if (usb_enumerate(d) >= 0) o->pstate[p] = 1;
}

static void ohci_scan(ohci_t *o)
{
    for (int p = 0; p < o->nports; p++) {
        u32 sc = PS(o, p);
        if (sc & 0x001F0000u) PS(o, p) = sc & 0x001F0000u;   /* ack changes */
        if (sc & 1) {
            if (!o->pstate[p]) ohci_port_up(o, p);
        } else if (o->pstate[p]) {
            for (int i = 0; i < NIEP; i++)
                if (o->iep[i].used && usb_is_below(o->iep[i].dev, &o->dev[p])) {
                    o->iep[i].used = 0; o->iep[i].ed->flags |= 1u << 14;
                }
            usb_detach(&o->dev[p]);
            o->pstate[p] = 0;
        }
    }
}

void ohci_service(void)
{
    for (int k = 0; k < no; k++) if (oh[k].ok) ohci_scan(&oh[k]);
}

/* ---- bring-up ---------------------------------------------------------------- */

static int ohci_init_one(pci_dev_t *pd)
{
    ohci_t *o = &oh[no];
    memset(o, 0, sizeof *o);
    u32 bar0 = pci_read32(pd->bus, pd->dev, pd->fn, 0x10);
    if ((bar0 & 1) || !(bar0 & ~0xFu)) return 0;
    o->reg = (u8 *)(bar0 & ~0xFu);
    pci_enable_bus_master(pd);

    /* take it from the BIOS/SMM if something is running it */
    if (R32(o->reg, 0x04) & (1u << 8)) {
        R32(o->reg, 0x08) = 1u << 3;                     /* OwnershipChangeRequest */
        for (int i = 0; i < 100 && (R32(o->reg, 0x04) & (1u << 8)); i++) usb_msleep(10);
    }
    R32(o->reg, 0x14) = 0xC000007Fu;                     /* no interrupts */

    u32 fm = R32(o->reg, 0x34);
    R32(o->reg, 0x08) = 1;                               /* reset */
    for (int i = 0; i < 100 && (R32(o->reg, 0x08) & 1); i++) usb_msleep(1);
    if (R32(o->reg, 0x08) & 1) return 0;

    o->hcca   = usb_dma(256, 256);
    o->ed_ctl = usb_dma(16, 16);
    o->td     = usb_dma((NTD + 1) * 16, 16);
    o->buf    = usb_dma(1024, 64);
    if (!o->hcca || !o->ed_ctl || !o->td || !o->buf) return 0;
    for (int i = 0; i < NIEP; i++) {
        o->iep[i].ed = usb_dma(16, 16);
        if (!o->iep[i].ed) return 0;
        o->iep[i].ed->flags = 1u << 14;                  /* skip until used */
        o->iep[i].ed->next = (i + 1 < NIEP) ? (u32)o->iep[i + 1].ed : 0;
    }
    o->ed_ctl->flags = 1u << 14;
    for (int i = 0; i < 32; i++) ((u32 *)o->hcca)[i] = (u32)o->iep[0].ed;

    u32 fi = fm & 0x3FFF;
    if (fi < 0x2000 || fi > 0x3000) fi = 0x2EDF;
    R32(o->reg, 0x34) = fi | ((((fi - 210) * 6) / 7) << 16) | ((fm ^ 0x80000000u) & 0x80000000u);
    R32(o->reg, 0x40) = (fi * 9) / 10;
    R32(o->reg, 0x18) = (u32)o->hcca;
    R32(o->reg, 0x20) = (u32)o->ed_ctl;
    R32(o->reg, 0x28) = 0;
    R32(o->reg, 0x04) = 0x80 | 0x10 | 0x04 | 3;          /* operational, control + periodic */

    u32 ra = R32(o->reg, 0x48);
    o->nports = (int)(ra & 0xFF);
    if (o->nports > 8) o->nports = 8;
    if (!o->nports) return 0;
    R32(o->reg, 0x50) = 1u << 16;                        /* global port power on */
    usb_msleep((u32)((ra >> 24) & 0xFF) * 2 + 20);
    for (int p = 0; p < o->nports; p++) PS(o, p) = 1u << 8;

    o->next_addr = 1;
    o->ok = 1;
    usb_msleep(150);
    ohci_scan(o);
    return 1;
}

int ohci_probe(void)
{
    ohci_hc.name = "OHCI";
    ohci_hc.ctrl = ohci_ctrl;
    ohci_hc.int_open = ohci_int_open;
    ohci_hc.set_mps0 = NULL;
    ohci_hc.hub_config = NULL;
    ohci_hc.attach_child = ohci_attach_child;
    ohci_hc.drop_child = ohci_drop_child;
    int found = 0;
    pci_dev_t pd;
    for (int i = 0; i < MAXO; i++) {
        if (!pci_find_class(0x0C, 0x03, 0x10, i, &pd)) break;
        if (ohci_init_one(&pd)) { no++; found++; usb_count_controller(2); }
    }
    return found;
}
