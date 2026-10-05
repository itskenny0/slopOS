#include "pico.h"

/* CPU identification is kept in one small driver rather than duplicated in
 * the shell and the desktop. It uses only the architected CPUID instruction,
 * so it is safe on the 386/early-486 machines PicoOS still supports. */

static int available;
static u32 max_basic, max_extended;
static u32 family_no, model_no, stepping_no, signature;
static char vendor_text[13];
static char brand_text[49];

static int cpuid_available(void)
{
    u32 before, after;
    __asm__ volatile(
        "pushfl\n"
        "popl %0\n"
        "movl %0, %1\n"
        "xorl $0x200000, %1\n"
        "pushl %1\n"
        "popfl\n"
        "pushfl\n"
        "popl %1\n"
        "popfl\n"
        : "=r"(before), "=r"(after));
    return ((before ^ after) & 0x200000u) != 0;
}

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b),
                     "=c"(*c), "=d"(*d) : "a"(leaf));
}

static void put4(char *out, int at, u32 v)
{
    out[at + 0] = (char)(v & 0xFF);
    out[at + 1] = (char)((v >> 8) & 0xFF);
    out[at + 2] = (char)((v >> 16) & 0xFF);
    out[at + 3] = (char)((v >> 24) & 0xFF);
}

static void trim_brand(void)
{
    int first = 0, last = 48;
    while (first < 48 && (brand_text[first] == ' ' || brand_text[first] == 0)) first++;
    while (last > first && (brand_text[last - 1] == ' ' || brand_text[last - 1] == 0)) last--;
    if (first) {
        int i = 0;
        while (first < last) brand_text[i++] = brand_text[first++];
        brand_text[i] = 0;
    } else {
        brand_text[48] = 0;
    }
}

void cpu_init(void)
{
    u32 a, b, c, d;
    available = cpuid_available();
    vendor_text[0] = 0;
    brand_text[0] = 0;
    family_no = model_no = stepping_no = signature = 0;
    max_basic = max_extended = 0;

    if (!available) {
        strcpy(vendor_text, "unknown");
        strcpy(brand_text, "i386-compatible CPU");
        return;
    }

    cpuid(0, &a, &b, &c, &d);
    max_basic = a;
    put4(vendor_text, 0, b);
    put4(vendor_text, 4, d);
    put4(vendor_text, 8, c);
    vendor_text[12] = 0;

    if (max_basic >= 1) {
        cpuid(1, &a, &b, &c, &d);
        signature = a;
        stepping_no = a & 0xF;
        family_no = (a >> 8) & 0xF;
        model_no = (a >> 4) & 0xF;
        if (family_no == 0xF) family_no += (a >> 20) & 0xFF;
        if (family_no == 0x6 || family_no == 0xF)
            model_no += ((a >> 16) & 0xF) << 4;
    }

    cpuid(0x80000000u, &a, &b, &c, &d);
    max_extended = a;
    if (max_extended >= 0x80000004u) {
        cpuid(0x80000002u, &a, &b, &c, &d);
        put4(brand_text, 0, a);  put4(brand_text, 4, b);
        put4(brand_text, 8, c);  put4(brand_text, 12, d);
        cpuid(0x80000003u, &a, &b, &c, &d);
        put4(brand_text, 16, a); put4(brand_text, 20, b);
        put4(brand_text, 24, c); put4(brand_text, 28, d);
        cpuid(0x80000004u, &a, &b, &c, &d);
        put4(brand_text, 32, a); put4(brand_text, 36, b);
        put4(brand_text, 40, c); put4(brand_text, 44, d);
        brand_text[48] = 0;
        trim_brand();
    }
    if (!brand_text[0]) strcpy(brand_text, "compatible x86 processor");
}

int cpu_has_cpuid(void) { return available; }
const char *cpu_vendor(void) { return vendor_text; }
const char *cpu_name(void) { return brand_text; }
u32 cpu_family(void) { return family_no; }
u32 cpu_model(void) { return model_no; }
u32 cpu_stepping(void) { return stepping_no; }
u32 cpu_signature(void) { return signature; }

static void append(char *out, u32 max, u32 *at, const char *s)
{
    while (*s && *at + 1 < max) out[(*at)++] = *s++;
    if (*at < max) out[*at] = 0;
}

static void append_num(char *out, u32 max, u32 *at, u32 n)
{
    char t[12]; int k = 0;
    if (!n) t[k++] = '0';
    while (n && k < 11) { t[k++] = (char)('0' + n % 10); n /= 10; }
    while (k && *at + 1 < max) out[(*at)++] = t[--k];
    if (*at < max) out[*at] = 0;
}

void cpu_report(char *out, u32 max)
{
    u32 at = 0;
    if (!max) return;
    out[0] = 0;
    append(out, max, &at, "CPU: "); append(out, max, &at, brand_text);
    append(out, max, &at, "\nVendor: "); append(out, max, &at, vendor_text);
    append(out, max, &at, "\nFamily "); append_num(out, max, &at, family_no);
    append(out, max, &at, " Model "); append_num(out, max, &at, model_no);
    append(out, max, &at, " Stepping "); append_num(out, max, &at, stepping_no);
}
