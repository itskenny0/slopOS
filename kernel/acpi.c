/* acpi.c -- turning the machine off, properly
 *
 * `shutdown` used to write to three I/O ports that QEMU, Bochs and
 * VirtualBox happen to watch, then park the CPU with interrupts off. On an
 * emulator that looks exactly like a shutdown. On a real machine the fans
 * keep spinning and you end up holding the power button, which is how this
 * bug went unnoticed through every test that ever ran here: the tests were
 * all emulators, and the emulators all lied.
 *
 * Doing it for real means ACPI, and ACPI means finding four numbers:
 *
 *   1. the RSDP, a signed 16-byte-aligned structure somewhere in low memory,
 *      which points at the RSDT
 *   2. the FADT inside the RSDT, which holds the PM1 control ports and the
 *      handshake for taking the machine out of legacy mode
 *   3. the DSDT, which the FADT points at
 *   4. the sleep type for S5 ("soft off"), which lives inside the DSDT
 *      encoded as AML bytecode
 *
 * The fourth is the awkward one. AML is a full bytecode language and PicoOS
 * is never going to have an interpreter for it. But the thing we need is one
 * fixed shape near the start of a package named _S5_, and it can be found by
 * pattern rather than parsed: search the DSDT for the four bytes "_S5_",
 * step over the package header, and read the first one or two integers out
 * of it. This is what every small kernel does, it has worked since 1999, and
 * the failure mode when it does not match is that we fall through to the old
 * behaviour rather than write a wrong value to a hardware register.
 *
 * Everything here is read-only and range-checked. A firmware table is
 * attacker-adjacent input as far as a kernel is concerned: the machine wrote
 * it, but the machine may have written it badly, and a table that claims to
 * be four gigabytes long should cost us a failed shutdown and nothing else.
 */

#include "pico.h"

typedef struct {
    char sig[8];            /* "RSD PTR " */
    u8   checksum;
    char oemid[6];
    u8   revision;
    u32  rsdt;
} __attribute__((packed)) rsdp_t;

typedef struct {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oemid[6];
    char oemtable[8];
    u32  oemrev;
    u32  creator;
    u32  creatorrev;
} __attribute__((packed)) sdt_hdr_t;

/* Only the fields we use, at their fixed offsets in the FADT. */
typedef struct {
    sdt_hdr_t h;
    u32  firmware_ctrl;     /* 36 */
    u32  dsdt;              /* 40 */
    u8   reserved;          /* 44 */
    u8   preferred_pm;      /* 45 */
    u16  sci_int;           /* 46 */
    u32  smi_cmd;           /* 48 */
    u8   acpi_enable;       /* 52 */
    u8   acpi_disable;      /* 53 */
    u8   s4bios_req;        /* 54 */
    u8   pstate_cnt;        /* 55 */
    u32  pm1a_evt_blk;      /* 56 */
    u32  pm1b_evt_blk;      /* 60 */
    u32  pm1a_cnt_blk;      /* 64 */
    u32  pm1b_cnt_blk;      /* 68 */
} __attribute__((packed)) fadt_t;

#define SLP_EN   (1u << 13)
#define SCI_EN   1u

static int   acpi_ready;
static u32   pm1a_cnt, pm1b_cnt;
static u16   slp_typa, slp_typb;
static u32   smi_cmd;
static u8    acpi_enable_val;

static int checksum_ok(const u8 *p, u32 len)
{
    u8 sum = 0;
    while (len--) sum = (u8)(sum + *p++);
    return sum == 0;
}

/* The RSDP is either in the first kilobyte of the EBDA, whose segment the
 * BIOS leaves at 0x40E, or in the read-only BIOS area at the top of the
 * first megabyte. Both are 16-byte aligned by specification. */
static const rsdp_t *find_rsdp(void)
{
    /* The compiler assumes a dereference of a small constant is a null
     * pointer bug. In a kernel it is the BIOS data area. Launder the
     * address through a register so it stops recognising the constant. */
    volatile u16 *bda = (volatile u16 *)0x40E;
    __asm__ __volatile__("" : "+r"(bda));
    u32 ebda = (u32)(*bda) << 4;

    for (int pass = 0; pass < 2; pass++) {
        u32 start, end;
        if (pass == 0) {
            if (ebda < 0x400 || ebda > 0x9FC00) continue;
            start = ebda; end = ebda + 1024;
        } else {
            start = 0xE0000; end = 0x100000;
        }
        for (u32 a = start; a + sizeof(rsdp_t) <= end; a += 16) {
            const rsdp_t *r = (const rsdp_t *)a;
            if (memcmp(r->sig, "RSD PTR ", 8) != 0) continue;
            if (!checksum_ok((const u8 *)r, 20)) continue;   /* v1 part only */
            return r;
        }
    }
    return NULL;
}

static const sdt_hdr_t *find_table(const sdt_hdr_t *rsdt, const char *sig)
{
    if (rsdt->length < sizeof *rsdt || rsdt->length > (1u << 20)) return NULL;

    u32 n = (rsdt->length - sizeof *rsdt) / 4;
    const u32 *ptr = (const u32 *)(rsdt + 1);

    for (u32 i = 0; i < n; i++) {
        if (ptr[i] < 0x1000 || ptr[i] >= 0xFFF00000u) continue;
        const sdt_hdr_t *t = (const sdt_hdr_t *)ptr[i];
        if (memcmp(t->sig, sig, 4) == 0) return t;
    }
    return NULL;
}

/* Pull SLP_TYPa/b out of the _S5_ package.
 *
 * The AML we are looking for is, in bytes:
 *
 *     5F 53 35 5F   "_S5_"
 *     12            PackageOp
 *     <pkglen>      1 to 4 bytes, top two bits of the first say how many
 *     <count>       number of elements, always 2 or 4 in practice
 *     0A xx | 00 | 01    the first element: byte constant, Zero, or One
 *     ...                the second element, same encoding
 *
 * Before the "_S5_" there may or may not be a NameOp (0x08); we do not care,
 * because we start reading after the name. Anything that does not match this
 * shape exactly is refused. */
static int parse_s5(const sdt_hdr_t *dsdt)
{
    if (dsdt->length < sizeof *dsdt || dsdt->length > (1u << 20)) return 0;

    const u8 *p   = (const u8 *)dsdt;
    const u8 *end = p + dsdt->length;

    for (const u8 *q = p + sizeof *dsdt; q + 8 < end; q++) {
        if (q[0] != '_' || q[1] != 'S' || q[2] != '5' || q[3] != '_') continue;

        const u8 *a = q + 4;
        if (*a != 0x12) continue;               /* PackageOp */
        a++;

        u32 lenbytes = (u32)(*a >> 6);          /* PkgLength encoding */
        a += 1 + lenbytes;
        if (a >= end) return 0;

        u8 count = *a++;                        /* element count */
        if (count < 1 || a >= end) return 0;

        u16 v[2] = { 0, 0 };
        for (int i = 0; i < 2 && i < count && a < end; i++) {
            if (*a == 0x0A) {                   /* BytePrefix */
                a++;
                if (a >= end) return 0;
                v[i] = *a++;
            } else if (*a == 0x00) { v[i] = 0; a++; }   /* ZeroOp */
            else if (*a == 0x01)   { v[i] = 1; a++; }   /* OneOp  */
            else if (*a == 0x0B) {              /* WordPrefix */
                a++;
                if (a + 1 >= end) return 0;
                v[i] = (u16)(a[0] | (a[1] << 8));
                a += 2;
            } else return 0;                    /* not a shape we know */
        }
        slp_typa = v[0];
        slp_typb = v[1];
        return 1;
    }
    return 0;
}

/* Called once at boot. Everything it learns is cached, because by the time
 * `shutdown` runs we may be in no state to go walking through memory. */
void acpi_init(void)
{
    acpi_ready = 0;

    const rsdp_t *rsdp = find_rsdp();
    if (!rsdp || rsdp->rsdt < 0x1000) return;

    const sdt_hdr_t *rsdt = (const sdt_hdr_t *)rsdp->rsdt;
    if (memcmp(rsdt->sig, "RSDT", 4) != 0) return;

    const sdt_hdr_t *f = find_table(rsdt, "FACP");
    if (!f || f->length < sizeof(fadt_t)) return;

    const fadt_t *fadt = (const fadt_t *)f;
    if (!fadt->pm1a_cnt_blk || !fadt->dsdt) return;

    const sdt_hdr_t *dsdt = (const sdt_hdr_t *)fadt->dsdt;
    if (memcmp(dsdt->sig, "DSDT", 4) != 0) return;
    if (!parse_s5(dsdt)) return;

    pm1a_cnt        = fadt->pm1a_cnt_blk;
    pm1b_cnt        = fadt->pm1b_cnt_blk;
    smi_cmd         = fadt->smi_cmd;
    acpi_enable_val = fadt->acpi_enable;
    acpi_ready      = 1;
}

int acpi_available(void) { return acpi_ready; }

/* Hand the machine to ACPI mode if the firmware left it in legacy mode.
 * Some boards ignore a sleep request until SCI_EN is set. */
static void acpi_enable_mode(void)
{
    if (inw((u16)pm1a_cnt) & SCI_EN) return;
    if (!smi_cmd || !acpi_enable_val) return;

    outb((u16)smi_cmd, acpi_enable_val);

    for (int i = 0; i < 300; i++) {
        if (inw((u16)pm1a_cnt) & SCI_EN) return;
        sleep_ms(10);
    }
}

int acpi_poweroff(void)
{
    if (!acpi_ready) return 0;

    acpi_enable_mode();

    outw((u16)pm1a_cnt, (u16)((slp_typa << 10) | SLP_EN));
    if (pm1b_cnt) outw((u16)pm1b_cnt, (u16)((slp_typb << 10) | SLP_EN));

    /* If the machine is still here after this, ACPI declined. */
    for (volatile int i = 0; i < 1000000; i++) ;
    return 0;
}
