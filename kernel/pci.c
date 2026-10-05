/* pci.c -- finding the hardware
 *
 * PicoOS has got away without this until now. The keyboard, the timer, the
 * serial port and the VGA card have all been at the same I/O addresses
 * since 1984, so the kernel could simply talk to them. A network card is
 * the first device with no fixed address: the firmware assigns it one at
 * boot, and where it lands depends on the machine, the slot and the day.
 *
 * So we have to ask. PCI configuration space is reachable through two I/O
 * ports -- write the address you want to 0xCF8, read the answer from 0xCFC
 * -- and every device on every bus can be enumerated by walking every
 * possible address and seeing what answers. There are 256 buses of 32
 * devices of 8 functions, which sounds enormous and takes a few
 * milliseconds, because a bus with nothing on it returns 0xFFFF for the
 * vendor immediately.
 *
 * Buses are found by following bridges. See pci_scan() below: it used to
 * look at bus 0 and 1 only, which is to say it never found the network
 * card in any machine built after about 2010.
 *
 * The first thing that actually needed this was not the network card but
 * the mouse: a USB mouse is behind a host controller whose address the
 * firmware hands out at boot, so before you can talk to the mouse you have
 * to find the controller, and before that you have to be able to ask the
 * bus what is on it.
 */

#include "pico.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

static inline u32 cfg_addr(u8 bus, u8 dev, u8 fn, u8 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11)
         | ((u32)fn << 8) | (off & 0xFCu);
}

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 v = pci_read32(bus, dev, fn, off);
    return (u16)((v >> ((off & 2) * 8)) & 0xFFFF);
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    outl(CONFIG_DATA, val);
}

void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 val)
{
    u32 v = pci_read32(bus, dev, fn, off);
    int shift = (off & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((u32)val << shift);
    pci_write32(bus, dev, fn, off, v);
}

/* ---- which buses exist ----------------------------------------------
 *
 * Very little on a machine built this century is on bus 0. The chipset
 * hangs a PCIe root port off it, and everything behind that port -- the
 * onboard network card, the USB controller, the M.2 slot -- answers on a
 * bus number the firmware invented at boot, which cannot be guessed and
 * differs between two otherwise identical machines.
 *
 * Bridges know the answer. A PCI-to-PCI bridge carries the number of the
 * bus on its far side in its configuration space, so finding every bus is
 * a matter of starting at 0 and asking each bridge where it leads. The
 * list grows while it is being walked, which is the whole of the
 * algorithm; the bound on it is only there so a bridge that lies cannot
 * send us round for ever.
 */
#define MAX_BUS 32
static u8  bus_list[MAX_BUS];
static int bus_count;
static int bus_scanned;

void pci_scan(void)
{
    if (bus_scanned) return;
    bus_scanned = 1;
    bus_count   = 0;
    bus_list[bus_count++] = 0;

    for (int i = 0; i < bus_count && bus_count < MAX_BUS; i++) {
        u8 bus = bus_list[i];
        for (u8 dev = 0; dev < 32; dev++) {
            for (u8 fn = 0; fn < 8; fn++) {
                u32 id = pci_read32(bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;          /* nothing here at all */
                    continue;
                }
                u32 c = pci_read32(bus, dev, fn, 0x08);
                if ((u8)(c >> 24) != 0x06) continue;         /* not a bridge */
                if ((u8)(c >> 16) != 0x04) continue;         /* not pci-to-pci */

                u8 sec = (u8)(pci_read32(bus, dev, fn, 0x18) >> 8);
                if (!sec || sec == 0xFF) continue;

                int seen = 0;
                for (int k = 0; k < bus_count; k++)
                    if (bus_list[k] == sec) { seen = 1; break; }
                if (!seen && bus_count < MAX_BUS) bus_list[bus_count++] = sec;
            }
        }
    }
}

int pci_bus_count(void) { pci_scan(); return bus_count; }
u8  pci_bus_at(int i)
{
    pci_scan();
    return (i >= 0 && i < bus_count) ? bus_list[i] : 0;
}

/* Look for one particular chip. Returns 1 and fills in `out` if it is
 * there, on any bus the scan found. */
int pci_find(u16 vendor, u16 device, pci_dev_t *out)
{
    pci_scan();
    for (int bi = 0; bi < bus_count; bi++) {
        u8 bus = bus_list[bi];
        for (u8 dev = 0; dev < 32; dev++) {
            for (u8 fn = 0; fn < 8; fn++) {
                u32 id = pci_read32((u8)bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;     /* no device here at all */
                    continue;
                }
                if ((id & 0xFFFF) != vendor) continue;
                if ((id >> 16) != device) continue;

                out->bus = (u8)bus;
                out->dev = dev;
                out->fn  = fn;
                out->vendor = vendor;
                out->device = device;
                return 1;
            }
        }
    }
    return 0;
}

/* Base address register: the low bits are flags, not address. Bit 0 says
 * whether this is I/O space or memory space. */
u32 pci_bar(const pci_dev_t *d, int index)
{
    u32 v = pci_read32(d->bus, d->dev, d->fn, (u8)(0x10 + index * 4));
    return (v & 1) ? (v & ~0x3u) : (v & ~0xFu);
}

int pci_bar_is_io(const pci_dev_t *d, int index)
{
    return pci_read32(d->bus, d->dev, d->fn, (u8)(0x10 + index * 4)) & 1;
}

u8 pci_irq(const pci_dev_t *d)
{
    return (u8)(pci_read32(d->bus, d->dev, d->fn, 0x3C) & 0xFF);
}

/* A card cannot write to memory on its own until someone says it may.
 * Forgetting this is the classic way to end up with a driver that
 * transmits perfectly and never receives anything. */
void pci_enable_bus_master(const pci_dev_t *d)
{
    u16 cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= (1 << 0)     /* respond to I/O space   */
         | (1 << 1)     /* respond to memory space */
         | (1 << 2);    /* act as a bus master     */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}

/* Find the Nth device of a given class. USB controllers are all class 0x0C
 * subclass 0x03, and the programming interface byte says which kind:
 * 0x00 UHCI, 0x10 OHCI, 0x20 EHCI, 0x30 xHCI. Pass progif 0xFF for "any".
 */
int pci_find_class(u8 cls, u8 sub, u8 progif, int index, pci_dev_t *out)
{
    int seen = 0;
    pci_scan();
    for (int bi = 0; bi < bus_count; bi++) {
        u8 bus = bus_list[bi];
        for (u8 dev = 0; dev < 32; dev++) {
            for (u8 fn = 0; fn < 8; fn++) {
                u32 id = pci_read32((u8)bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                u32 c = pci_read32((u8)bus, dev, fn, 0x08);
                if ((u8)(c >> 24) != cls) continue;
                if ((u8)(c >> 16) != sub) continue;
                if (progif != 0xFF && (u8)(c >> 8) != progif) continue;
                if (seen++ != index) continue;

                out->bus = (u8)bus;
                out->dev = dev;
                out->fn  = fn;
                out->vendor = (u16)(id & 0xFFFF);
                out->device = (u16)(id >> 16);
                return 1;
            }
        }
    }
    return 0;
}
