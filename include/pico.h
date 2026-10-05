#ifndef PICO_H
#define PICO_H

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef signed short       i16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef unsigned long long u64;
typedef unsigned int       size_t;

#define NULL ((void*)0)
#define TRUE 1
#define FALSE 0

/* ---------------- port I/O ---------------- */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline u16  inw(u16 p)         { u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32  inl(u16 p)         { u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_wait(void)      { outb(0x80, 0); }
static inline void cli(void)          { __asm__ volatile("cli"); }
static inline void sti(void)          { __asm__ volatile("sti"); }
static inline void hlt(void)          { __asm__ volatile("hlt"); }

/* Turn interrupts off and remember whether they were on, so that a critical
 * section can be nested inside another one without switching them back on
 * too early. This is the only lock PicoOS has -- with one CPU and no user
 * mode it is also the only one it needs. */
static inline u32 irq_save(void)
{
    u32 f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(u32 f)
{
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* ---------------- handover from the bootloader ---------------- */
#define BOOTINFO_MAGIC 0x30434950u      /* 'PIC0' */
#define FW_BIOS 0
#define FW_UEFI 1

typedef struct {
    u32 magic;
    u32 firmware;      /* FW_BIOS or FW_UEFI                        */
    u32 fb_base;       /* linear framebuffer, 0 = VGA text mode     */
    u32 fb_width;
    u32 fb_height;
    u32 fb_pitch;      /* bytes per scanline                        */
    u32 fb_bpp;
    u32 fb_bgr;        /* 1 = BGR byte order, 0 = RGB               */
    u32 heap_base;     /* usable RAM the kernel may own             */
    u32 heap_size;
    u32 archive_addr;  /* PAPP archive in RAM, 0 = right behind kernel */
    u32 archive_size;  /* bytes the archive occupies there            */
} bootinfo_t;

extern u32 boot_info_ptr;          /* set by entry.S from EBX */
bootinfo_t *bootinfo(void);        /* NULL unless a loader gave us one */

/* ---------------- processor identification ---------------- */
void cpu_init(void);
int  cpu_has_cpuid(void);
const char *cpu_vendor(void);
const char *cpu_name(void);
u32  cpu_family(void);
u32  cpu_model(void);
u32  cpu_stepping(void);
u32  cpu_signature(void);
void cpu_report(char *out, u32 max);

/* ---------------- console (VGA text or framebuffer) ---------------- */
enum {
    BLACK=0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGREY,
    DGREY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE
};
void con_init(void);
void con_clear(void);
void con_putc(char c);
void con_puts(const char *s);
void con_setcolor(u8 fg, u8 bg);
u8   con_getcolor(void);
void con_shadow_init(void);   /* needs the heap; makes scrolling cheap */
void con_set_cursor(int x, int y);
void con_cursor(int on);          /* show or hide the text cursor */
void con_backspace(void);
int  con_width(void);
int  con_height(void);
const char *con_driver(void);

/* ---- graphics, new in 1.0 ---- */
void con_font_override(u8 code, const u8 *rows16);
void con_fb_putpixel(int x, int y, u8 c);
void con_fb_blit(const u8 *frame, int fw, int fh, int dx, int dy,
                 int scale, const u8 *pal);
u32  con_pal256(int idx);           /* the 256-colour table, idx&15=CGA  */
void con_fb_fill(int x, int y, int w, int h, u8 c);
void con_fb_dims(int *w, int *h);
void con_set_rows(int r);

int  gfx_mode(int on);            /* 1 = the 640x480 drawing screen  */
int  gfx_ready(void);
void gfx_size(int *w, int *h);
void gfx_pixel(int x, int y, int c);
void gfx_fill(int x, int y, int w, int h, int c);
int  gfx_lines(int want);         /* 25 or 50 text rows              */
int  gfx_lines_now(void);
void gfx_set_glyph(int code, const u8 *rows16);
int  gfx_is256(void);                /* 256 colours in this gfx mode?   */
void gfx_setpal(const u8 *rgb);      /* 768 bytes, see api->setpal      */
int  gfx_blit(const u8 *frame, int fw, int fh);
int  gfx_blit_rect(const u8 *frame, int fw, int fh, int x, int y, int w, int h);
const u8 *gfx_glyph(int c);
void con_fb_blit_rect(const u8 *frame, int fw, int x, int y, int w, int h, const u8 *pal);
void gfx_cursor(int x, int y, int on);       /* kernel-drawn mouse pointer  */
void gfx_text(int x, int y, const char *s, int fg, int bg);   /* 8x16 font */
extern const u8 hand_glyph[16];

extern const u8 font8x16[256][16];

/* ---------------- formatted output ---------------- */
void kprintf(const char *fmt, ...);
void putc(char c);
void puts(const char *s);

/* ---------------- string / mem helpers ---------------- */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strncat(char *d, const char *s, size_t max);
int    atoi(const char *s);
void   itoa(int v, char *buf, int base);
void   utoa(u32 v, char *buf, int base);

/* ---------------- interrupts ---------------- */
typedef struct {
    u32 ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
} regs_t;

typedef void (*isr_handler_t)(regs_t *);

void idt_init(void);
void irq_install(int irq, isr_handler_t h);
void isr_install(int n, isr_handler_t h);

/* ---------------- drivers ---------------- */
void rtc_time(int *hour, int *min, int *sec);
void rtc_date(int *year, int *month, int *day);
void rtc_set_time(int hour, int min, int sec);
void rtc_set_date(int year, int month, int day);

/* the pointer: cells or pixels */
void mouse_pixel_mode(int w, int h);
int  mouse_get_px(int *x, int *y, int *buttons);

/* the speaker's honest controls: pitch (in the beep calls) and on/off */
void sound_set(int on);
int  sound_is_on(void);
void timer_init(u32 hz);
u32  timer_ticks(void);
u32  timer_ms(void);
void sleep_ms(u32 ms);

/* keys that have no ASCII value: returned by kbd_getchar/kbd_poll as-is */
#define KEY_UP     0x81
#define KEY_DOWN   0x82
#define KEY_LEFT   0x83
#define KEY_RIGHT  0x84
#define KEY_HOME   0x85
#define KEY_END    0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
#define KEY_DEL    0x89
#define KEY_INS    0x8A
#define KEY_SUPER  0x9A        /* Windows / Super key */
#define KEY_F1     0x90        /* F1..F10 = 0x90..0x99 */

void kbd_init(void);
int  kbd_getchar(void);      /* blocking, returns ASCII */
int  kbd_poll(void);         /* non-blocking, 0 if nothing */
void kbd_flush(void);        /* PICO-FIX-kbdflush: drop queued + held keys */
int  kbd_down(int code);     /* is this scancode held right now? */
int  kbd_answers(void);      /* does the keyboard still reply? */
void kbd_readline(char *buf, int max);

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
int  serial_poll(void);          /* -1 when nothing has arrived        */
void serial_set_baud(u32 baud);
void serial_mute(int on);        /* stop echoing the console to COM1   */
int  serial_is_muted(void);

/* ---------------- memory ---------------- */
void  mem_init(void);
u32   stack_size(void);      /* bytes reserved for the kernel stack     */
u32   stack_used(void);      /* high-water mark, measured not guessed   */
u32   mem_footprint(void);   /* highest physical byte PicoOS ever uses  */
u32   mem_base(void);        /* where the heap starts                   */
void *kmalloc(size_t n);
void  mem_disown(void *p);     /* give a block to the kernel (DMA)   */
void  mem_free_task(int id);   /* everything a task forgot to free */
void  kfree(void *p);
u32   mem_total(void);
u32   mem_used(void);
u32   mem_free(void);
int   mem_blocks(void);

/* ---------------- ram filesystem ---------------- */
#define FS_MAXFILES 32
#define FS_NAMELEN  16
typedef struct {
    char  name[FS_NAMELEN];
    char *data;
    u32   size;
    u32   cap;          /* 0 = mounted in place, not heap-owned */
    int   used;
} file_t;

/* ---------------- the app archive glued behind the kernel ---------------- */
/* ---- compression (tools/lzss.py packs, unpack.c unpacks) ---- */
int  unpack(const u8 *src, u32 srclen, u8 *dst, u32 max);

/* ---- the program store: packed programs, unpacked on demand ---- */
#define STORE_MAX     24
#define STORE_ENOENT  (-1)      /* no such program in the store   */
#define STORE_EEXIST  (-2)      /* already unpacked               */
#define STORE_ENOMEM  (-3)      /* not enough heap to unpack it   */
#define STORE_EDATA   (-4)      /* the packed stream is corrupt   */
#define STORE_EFULL   (-5)      /* ramdisk directory is full      */

typedef struct {
    char      name[FS_NAMELEN];
    const u8 *data;
    u32       packed;
    u32       orig;
} store_ent_t;

/* ---- PCI: how we find a device that has no fixed address ---- */
typedef struct {
    u8  bus, dev, fn;
    u16 vendor, device;
} pci_dev_t;

void pci_scan(void);            /* find every bus, following the bridges  */
int  pci_bus_count(void);
u8   pci_bus_at(int i);
u32  pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
u16  pci_read16(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val);
void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 val);
int  pci_find(u16 vendor, u16 device, pci_dev_t *out);
int  pci_find_class(u8 cls, u8 sub, u8 progif, int index, pci_dev_t *out);
u32  pci_bar(const pci_dev_t *d, int index);
int  pci_bar_is_io(const pci_dev_t *d, int index);
u8   pci_irq(const pci_dev_t *d);
void pci_enable_bus_master(const pci_dev_t *d);

int  mouse_enable(void);
void mouse_disable(void);
int  mouse_present(void);
int  mouse_step(void);
int  mouse_alive(void);
int  mouse_ps2(void);      /* the aux port answered the handshake      */
int  mouse_get(int *x, int *y, int *btn);
void kbd_scancode(u8 sc);
/* ---- USB (kernel/usb.c) ---- */
void usb_service(void);           /* hot-plug housekeeping, task context */
void usb_tick(void);              /* 100 Hz from the timer interrupt     */
void mouse_feed(int dx, int dy, int wheel, int buttons);
void mouse_usb_attach(int delta);
int  mouse_usb_present(void);
int  mouse_wheel_take(void);

/* There is no native USB driver: USB keyboards and mice work through the
 * firmware's PS/2 emulation, which needs no driver and no setup wizard.
 * The usb_* slots in the api table below are kept (returning 0) so that
 * programs built against older kernels keep loading. */

void acpi_init(void);        /* find the shutdown registers, once */
int  acpi_available(void);   /* did we find them?                 */
int  acpi_poweroff(void);    /* returns only on failure           */

void store_init(void);
int  store_add(const char *name, const u8 *data, u32 packed, u32 orig);
int  store_count(void);
const store_ent_t *store_at(int i);
const store_ent_t *store_find(const char *name);
int  store_extract(const char *name);
u32  store_packed_bytes(void);
u32  store_orig_bytes(void);

u32  papp_size(void);        /* bytes, 0 if there is no archive */
u32  papp_base(void);        /* where it sits in memory */
int  papp_count(void);       /* how many files it holds */
void papp_install(void);     /* mount them all, in place */

void    fs_init(void);
file_t *fs_find(const char *name);
file_t *fs_create(const char *name);
int     fs_mount(const char *name, const char *data, u32 len);
int     fs_write(const char *name, const char *data, u32 len);
int     fs_append(const char *name, const char *data, u32 len);
int     fs_delete(const char *name);
int     fs_count(void);
file_t *fs_at(int i);
const char *fs_mapdata(const char *name, u32 *size);

/* ---------------- .pico executables ----------------
 *
 * A .pico file is a 40-byte header followed by flat 32-bit code linked at
 * PICO_LINK_ADDR, plus a list of the places inside it that hold an absolute
 * address. The kernel checks the header, copies the image wherever there
 * happens to be room, adds the difference to every address on that list,
 * zeroes the bss behind it and calls the entry point as
 *
 *     int main(picoapi_t *api, int argc, char **argv)
 *
 * The program talks to the kernel only through the api table it is handed,
 * so a .pico built today keeps working as the kernel grows.
 */
#define PICO_MAGIC      0x4F434950u        /* 'PICO' little endian */
#define PICO_FMT        3
#define PICO_LINK_ADDR  0x00020000u        /* what the linker assumed...   */
#define PICO_MAX_IMAGE  0x00100000u        /* ...but it can load anywhere  */
/* Which release this is. It used to be written out by hand in two places,
 * and one of them kept saying 0.7 long after the thing said 0.9 on the
 * boot screen -- the about box in the menu lied about what you were
 * running. One definition, and both places read it. */
#ifdef PICO_PRO
#define PICOOS_VERSION  "Pro 2.1"
#else
#define PICOOS_VERSION  "2.1"
#endif

#define PICOAPI_VERSION 15

/* Which machine the code inside a .pico was compiled for. The kernel refuses
 * anything that is not its own, so an x86 program on an ARM PicoOS gets a
 * clear error instead of a crash. */
#define PICO_ARCH_X86_32  1
#define PICO_ARCH_ARM32   2
#define PICO_ARCH_ARM64   3
#ifndef PICO_ARCH
#define PICO_ARCH PICO_ARCH_X86_32
#endif

typedef struct {
    u32 magic;          /* PICO_MAGIC                                   */
    u16 fmt;            /* PICO_FMT                                     */
    u8  arch;           /* PICO_ARCH_*                                  */
    u8  flags;          /* reserved, must be 0                          */
    u32 link_addr;      /* address the linker assumed                   */
    u32 entry;          /* entry point, as linked                       */
    u32 image_size;     /* bytes of image following this header         */
    u32 bss_size;       /* zero-filled bytes after the image            */
    u32 checksum;       /* sum of the image bytes, mod 2^32             */
    u32 api_min;        /* lowest picoapi version the program accepts   */
    u32 reloc_count;    /* how many 32-bit words need fixing up         */
    u32 reloc_off;      /* where that list starts, from the image start */
} pico_hdr_t;

/* What the task manager sees. Kept flat and free of pointers so that a
 * program compiled against one kernel keeps working on the next. */
typedef struct {
    int  pid;
    int  foreground;
    int  is_program;
    u32  cpu_ticks;              /* how long it has actually run    */
    u32  age_ticks;              /* how long since it was created   */
    u32  stack_used, stack_size;
    int  waiting;                /* parked on a key or the clock    */
    char name[16];
} pico_task_t;

typedef struct {
    u32 total, used, free_bytes;
    u32 blocks;
    u32 kernel_bytes;            /* the kernel image itself */
} pico_mem_t;

/* Everything a .pico program is allowed to do. Fields are only ever added
 * at the end, and `size` says how many are present, so old programs keep
 * running on newer kernels. */
typedef struct {
    u32 version;
    u32 size;

    void (*putc)(char c);
    void (*puts)(const char *s);
    void (*printf)(const char *fmt, ...);
    void (*clear)(void);
    void (*setcolor)(u8 fg, u8 bg);
    void (*gotoxy)(int x, int y);
    int  (*width)(void);
    int  (*height)(void);

    int  (*getkey)(void);            /* blocks until a key arrives      */
    int  (*poll)(void);              /* 0 when nothing was pressed      */

    u32  (*ticks)(void);             /* 100 per second since boot       */
    u32  (*ms)(void);
    void (*sleep)(u32 ms);

    void *(*malloc)(size_t n);
    void  (*free)(void *p);

    u32  (*rand)(void);
    void (*beep)(u32 hz, u32 ms);

    int  (*fs_read)(const char *name, char *buf, u32 max);
    int  (*fs_write)(const char *name, const char *buf, u32 len);
    void (*exit)(int code);

    /* ---- added in api v2, for the menu and the task manager ---- */

    /* Returns 1 and fills in the pointer position, in character cells, if
     * there is a working mouse. Enables it on the first call: a program
     * that never asks never switches the hardware on. */
    int  (*mouse)(int *x, int *y, int *buttons);

    /* Walk the ramdisk. Returns 0 when `index` is past the end, so the
     * caller does not need to know how many files there are first. */
    int  (*files)(int index, char *name, u32 *size);

    /* Walk the task table, same convention. */
    int  (*tasks)(int index, pico_task_t *out);

    void (*meminfo)(pico_mem_t *out);

    /* Run another program and wait for it. This is what makes a menu a
     * menu rather than a list. */
    int  (*run)(const char *name, int argc, char **argv);

    /* v3: switch the machine off or restart it. A desktop needs a way to
     * shut down, and a program cannot reach the shell's halt command. */
    void (*power)(int off_is_zero_reboot_is_one);

    /* Every program on the machine, packed or not. pi->files only sees the
     * RAM disk, and a program that has never been run is still sitting
     * compressed in the store -- so a menu built on files() would show you
     * an empty machine. Returns 0 when index runs past the last one. */
    int  (*programs)(int index, char *name16, u32 *size);

    /* Hide the text cursor. A full-screen program does not want one
     * trailing its drawing around; the kernel puts it back when the
     * program exits, so forgetting to is not fatal. */
    void (*cursor)(int on);

    /* v4: fetch a page.
     *
     * A program that can read the network is a browser, and a browser is
     * the one program an operating system of this size would otherwise
     * have to build into itself -- so the kernel hands out the socket and
     * gets out of the way. Returns 0 and leaves only the body of the page
     * in `buf`, or a negative number: -2 no such name, -3 nothing
     * answered, -10 the encrypted connection could not be made. */
    int  (*net_get)(const char *url, char *buf, u32 max, u32 *got,
                    char *status, int status_max);

    /* Why the last encrypted fetch failed, in words. A handshake can go
     * wrong for a dozen reasons -- a cipher this machine does not speak, a
     * certificate it cannot read, a server that wants something newer --
     * and "it did not work" helps nobody. */
    const char *(*tls_error)(void);

    /* What time of day it is, from the clock on the motherboard. Without
     * this a desktop can only tell you how long ago it started. */
    void (*rtc)(int *hour, int *min, int *sec);

    /* What the network looks like, in one line, for the corner of a
     * screen: "rtl8139 10.0.2.15", or "no network card". Returns 0 when
     * there is a card to talk about and -1 when there is not. */
    int  (*net_status)(char *buf, int max);

    /* Joining a wireless network, through an esp module on a serial port.
     * wifi_join gives -1 when there is no module, 0 when it joined; the
     * other two say which network you are on. */
    /* Ask the network for an address, the way `dhcp` does. 0 when the
     * machine is on the network and -1 when it is not. */
    int  (*net_up)(void);

    int  (*wifi_join)(const char *ssid, const char *pass);
    int  (*wifi_joined)(void);
    const char *(*wifi_name)(void);

    /* ---- v7: pixels, the pointer, the speaker -- new in 1.0 ---- */

    /* The drawing screen: 640x480 in 16 colours on a BIOS boot, the
     * firmware framebuffer unchanged on a UEFI one. gfx_mode(1) enters,
     * gfx_mode(0) returns to text. Returns 0 when the machine cannot. */
    int  (*gfx_mode)(int on);
    int  (*gfx_size)(int *w, int *h);
    void (*pixel)(int x, int y, int c);
    void (*fill_rect)(int x, int y, int w, int h, int c);

    /* Replace one character cell of the running font -- this is how the
     * text-mode desktop gets a hand with a finger on it for a pointer. */
    void (*glyph)(int code, const u8 *rows16);

    /* The pointer in pixels, for the drawing screen. Same convention as
     * mouse(): 0 when there is no mouse. */
    int  (*mouse_px)(int *x, int *y, int *buttons);

    /* The speaker has no volume knob -- it is a square wave -- but it
     * can be silenced and it can change pitch, so those are the knobs. */
    void (*sound)(int on_is_zero_off_is_one);

    /* The whole clock and calendar in one read. */
    void (*clock)(int *hour, int *min, int *sec,
                  int *year, int *month, int *day);

    /* v8-v12 USB slots: the native USB driver was removed. USB input
     * works through the firmware's PS/2 emulation instead, with no setup
     * step. These are kept (always 0) so old programs keep loading. */
    int  (*usb_take)(void);
    int  (*usb_give_back)(void);
    int  (*usb_mouse_present)(void);

    /* v9: return to the text shell from the graphical menu. The shell's
     * `exit` command returns to the calling menu instead of ending PicoOS. */
    void (*shell)(void);

    int  (*usb_hid_pending)(void);
    int  (*usb_hid_enable)(void);
    int  (*usb_hid_rescan)(void);
    int  (*usb_hid_probe_existing)(void);

    /* ---- v13: what DOOM needs that nothing before it did ---- */

    /* 1 when the drawing screen does 256 colours (VBE or firmware
     * framebuffer), 0 on the 16-colour planar fallback. Check after
     * gfx_mode(1): DOOM refuses to start on planar. */
    int  (*gfx256)(void);

    /* Load a 256-colour palette (768 bytes, RGB triplets). On VBE this
     * programs the DAC; on a firmware framebuffer it is remembered for
     * blit(). The desktop palette comes back with the next gfx_mode. */
    void (*setpal)(const u8 *rgb);

    /* Copy an indexed frame (w*h bytes) to the screen, integer-scaled
     * and centred, using the palette from setpal(). Returns 0 when the
     * screen cannot do 256 colours. */
    int  (*blit)(const u8 *frame, int w, int h);

    /* A direct pointer to a file's bytes (NULL when missing). The WAD is
     * mounted in place, so DOOM reads megabytes without copying them.
     * Valid until the file is written or deleted. */
    const char *(*fsmap)(const char *name, u32 *size);

    /* 1 when a key is currently held down. The code is the PS/2 set-1
     * scancode (0-127), or 0x100|sc for E0-prefixed keys. Polling beats
     * the key queue for games: holding still counts after the press. */
    int  (*keydown)(int code);

    /* ---- v14: PicoOS Pro 2.1 -- USB input and a real pointer ---- */

    /* Wheel clicks since the last call (positive = away from you). */
    int  (*mouse_wheel)(void);
    /* Draw (on=1) or hide (on=0) the kernel's mouse pointer at x,y. The
     * kernel keeps it on top of whatever the program paints. */
    void (*cursor_at)(int x, int y, int on);
    /* 8x16 text in the kernel font. bg < 0 keeps the background. */
    void (*text)(int x, int y, const char *s, int fg, int bg);
    /* One line saying which USB controllers and devices were found. */
    int  (*usb_info)(char *buf, int max);
    /* Description of the i-th USB device, 0 when there is none. */
    int  (*usb_line)(int i, char *buf, int max);

    /* ---- v15: flicker-free drawing ---- */

    /* Copy the rectangle (x,y,w,h) of a fw x fh indexed frame to the same
     * place on screen, using the palette from setpal. Unlike blit() there
     * is no scaling and nothing outside the rectangle is touched. */
    int  (*blit_rect)(const u8 *frame, int fw, int fh, int x, int y, int w, int h);
    /* The 16 bytes of the 8x16 glyph for a character (bit 7 = left). */
    const u8 *(*glyph_rows)(int c);
} picoapi_t;

int  pico_exec(const char *name, int argc, char **argv);
int  pico_bg(const char *name, int argc, char **argv);   /* returns a pid */
int  pico_fg(int pid);                                   /* wait for one   */
void apps_install(void);             /* generated by tools/mkapps.py     */
const char *pico_error(int code);
const char *pico_running(void);      /* NULL when no program is loaded  */
void pico_fault(void);               /* called by the exception handler  */

/* ---------------- tasks ----------------
 *
 * Round-robin, pre-emptive, one address space. A task is a stack plus a
 * struct, both from the heap, so multitasking costs nothing until it is
 * used. Exactly one task owns the screen and the keyboard; the rest print
 * into a buffer that is replayed when you bring them to the foreground.
 */
#define TASK_MAX 8

typedef u32 pico_jmpbuf[6];
int  pico_setjmp(pico_jmpbuf b);
void pico_longjmp(pico_jmpbuf b, int v);

void task_init(void);                /* wraps the shell into task 1      */
int  task_create(const char *name, void (*fn)(void *), void *arg,
                 void *image, void *argblk, int foreground);
void task_exit(int code);            /* never returns                    */
void task_yield(void);               /* give up the rest of the slice    */
void task_wait(void);                /* park until an interrupt          */
void task_tick(void);                /* from the timer irq               */
void task_resched(void);             /* end of the irq, after the EOI    */
void task_list(void);                /* the `ps` table                   */
int  task_kill(int pid);
int  task_setfg(int pid);
int  task_done(int pid);
int  task_exit_code(int pid);
void task_reap(int pid);             /* 0 = every finished task          */
int  task_sigint(void);              /* ctrl-c hit the foreground task   */
int  task_self(void);
int  task_stack_size(void);   /* bytes a task gets for its stack */
int  task_running_count(void);
int  task_info(int index, pico_task_t *out);
int  task_is_prog(void);
const char *task_name(void);
u32 *task_jmpbuf(void);
void task_set_exit(int code);
int  task_get_exit(void);
int  task_owns_console(void);
u8  *task_colorp(void);       /* this task's colour byte, NULL before mt */
void task_log(char c);

#define PICO_OK          0
#define PICO_ENOFILE    -1
#define PICO_EMAGIC     -2
#define PICO_EFMT       -3
#define PICO_EADDR      -4
#define PICO_ESIZE      -5
#define PICO_ECKSUM     -6
#define PICO_EAPI       -7
#define PICO_ETRUNC     -8
#define PICO_EARCH      -9
#define PICO_ENOMEM     -10
#define PICO_ERELOC     -11
#define PICO_ETASKS     -12

/* ---------------- shell ---------------- */
void shell_run(void);
void shell_open(void);  /* nested shell; `exit` returns to its caller */

/* ---------------- power ---------------- */
void reboot(void);
void shutdown(void);
void panic(const char *msg);

#endif


