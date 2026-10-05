#ifndef USB_H
#define USB_H

#include "pico.h"

/* ---- USB for PicoOS Pro 2.1 ----------------------------------------
 *
 * Four kinds of host controller exist, one per generation of USB, and
 * every PC has some mix of them:
 *
 *    xHCI  (USB 3.x, and every slower device plugged into its ports)
 *    EHCI  (USB 2.0 high speed)
 *    OHCI  (USB 1.1, Compaq/AMD/NVIDIA/SiS/OPTi lineage)
 *    UHCI  (USB 1.1, Intel/VIA lineage)
 *
 * Each driver knows how to reset its ports, give a device an address and
 * run control and interrupt transfers. Everything above that -- reading
 * descriptors, choosing the HID interface, parsing mouse and keyboard
 * reports -- is the same for all of them and lives in usb.c.
 *
 * Speeds use the xHCI numbering everywhere.
 */
#define USB_FS 1
#define USB_LS 2
#define USB_HS 3
#define USB_SS 4

#define HID_MOUSE 1
#define HID_KBD   2

struct usb_dev;

typedef struct usb_hc {
    const char *name;
    /* Control transfer on endpoint 0. Returns bytes moved, -1 on error. */
    int (*ctrl)(struct usb_dev *d, u8 rt, u8 rq, u16 val, u16 idx,
                void *buf, u16 len);
    /* Start polling an interrupt-IN endpoint. Every report is passed on
     * with usb_hid_report(tag, ...). Returns 0 on success. */
    int (*int_open)(struct usb_dev *d, u8 ep, u16 mps, u8 interval, int tag);
    /* The real wMaxPacketSize of endpoint 0, once known. */
    int (*set_mps0)(struct usb_dev *d, u16 mps);
    /* Hubs: tell the controller this device is a hub (xHCI needs to know),
     * and create + address a device on one of its downstream ports. */
    int (*hub_config)(struct usb_dev *hub, int nports, int ttt);
    struct usb_dev *(*attach_child)(struct usb_dev *hub, int port, int speed);
    void (*drop_child)(struct usb_dev *child);
} usb_hc_t;

typedef struct usb_dev {
    usb_hc_t *hc;
    void     *priv;         /* the driver's own bookkeeping            */
    u8        addr;
    u8        speed;        /* USB_FS .. USB_SS                        */
    u8        port;
    u16       mps0;
    int       tags;         /* bit 0 mouse attached, bit 1 keyboard    */
    struct usb_dev *parent;  /* the hub above, NULL on a root port      */
    struct usb_dev *kids[8]; /* devices on this hub's ports, 1-based-1  */
    u8        nports;       /* > 0 when this device is a hub           */
    u8        route_port;   /* port number on the parent hub           */
} usb_dev_t;

/* common layer (usb.c) */
int  usb_enumerate(usb_dev_t *d);          /* descriptors + HID attach */
void usb_detach(usb_dev_t *d);             /* the device went away     */
int  usb_is_below(usb_dev_t *d, usb_dev_t *root);   /* d == root or under it */
void usb_hid_report(int tag, const u8 *data, int len);
void usb_msleep(u32 ms);
void *usb_dma(u32 size, u32 align);        /* zeroed, aligned, kernel-owned */
int  usb_busy_enter(void);
void usb_busy_leave(void);

void usb_init(void);
int  usb_mouse_count(void);
int  usb_kbd_count(void);
int  usb_controller_count(void);
void usb_summary(char *out, int max);
void usb_count_controller(int kind);          /* 0 xhci 1 ehci 2 ohci 3 uhci */
const char *usb_devlog_line(int i);

/* controller drivers */
int  xhci_probe(void);                     /* returns controllers found */
void xhci_tick(void);                      /* from the timer interrupt  */
void xhci_service(void);                   /* hot-plug, task context    */
int  ehci_probe(void);
void ehci_tick(void);
void ehci_service(void);
int  uhci_probe(void);
void uhci_tick(void);
void uhci_service(void);
int  ohci_probe(void);
void ohci_tick(void);
void ohci_service(void);

#endif
