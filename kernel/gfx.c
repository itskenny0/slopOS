/* gfx.c -- pixels, video modes and fonts, new in 1.0
 *
 * Everything here goes through the VGA BIOS (see vgaintr.S for how a
 * 32-bit kernel calls it), which is the one interface every video card
 * since 1987 promises to honour. Two ways to draw:
 *
 *   - BIOS boot: mode 12h, 640x480 in 16 colours, planar. A pixel is
 *     one bit in each of four planes; writing it is a matter of the
 *     graphics controller's write mode 2 and a per-pixel bit mask.
 *   - UEFI boot: the loader already left us a linear framebuffer, so
 *     there is no mode to switch -- pixels go straight to memory, in
 *     whatever depth the firmware picked, through the console's packer.
 *
 * The same app code draws on both, because the differences stop at this
 * file. Also here: 80x50 text (an 8x8 font), and patching one glyph --
 * the pointing hand the mouse wears -- into whichever font is running.
 */

#include "pico.h"

u32  vga_bios_int10(u32 ax, u32 bx, u32 cx, u32 dx, u32 es, u32 bp);
extern void idt_load(void);

static int gfx;                     /* 1 = the drawing screen is up   */
static int lines = 25;              /* text rows, 25 or 50            */

/* the pointing hand, 8x16. Drawn as a filled silhouette: one finger up,
 * three fingers folded, a thumb to the right, a wrist at the bottom.
 * In 50-line mode every other row is used, which keeps the same shape. */
const u8 hand_glyph[16] = {
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x7E, 0x7E, 0x7E, 0x7F, 0x3F, 0x3E,
    0x7C, 0x78, 0x70, 0x60
};

static u32 int10(u32 ax, u32 bx, u32 cx, u32 dx, const void *p)
{
    u32 seg = p ? ((u32)p) >> 4 : 0;
    u32 off = p ? ((u32)p) & 0x0F : 0;
    u32 r;

    /* The trip runs with interrupts off, but the PIC still has whatever
     * was pending latched -- and some video BIOSes switch interrupts on
     * inside the call. In real mode a latched timer tick would vector
     * through the IVT's exception slots into a handler that expects a
     * completely different century. So: interrupts off around the whole
     * thing, every irq masked, and the kernel's IDT and the masks back
     * before anything can fire again. The thunk returns with interrupts
     * still disabled -- that is what makes the window between "the BIOS
     * answered" and "the PIC is whole again" atomic. */
    u32 f = irq_save();
    u8 m1 = inb(0x21), m2 = inb(0xA1);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    r = vga_bios_int10(ax, bx, cx, dx, seg, off);
    /* the thunk pointed the IDTR at the real-mode IVT; the kernel table
     * comes back before any irq can fire (the PIC is still masked) */
    idt_load();
    outb(0x21, m1);
    outb(0xA1, m2);
    irq_restore(f);
    return r;
}

static int is_fb(void) { return con_driver()[0] == 'f'; }

/* ---- fonts ---------------------------------------------------------- */

/* Writing into the VGA's font memory (plane 2) by hand. The BIOS offers
 * to do this (int 0x10, ax=0x1100), and that call is exactly where two
 * different video BIOSes were observed to derail when called from a
 * protected-mode kernel's trip into real mode -- so the registers are
 * programmed directly instead, which is the same six writes every time
 * and does not depend on firmware at all.
 *
 * Glyph cells are 32 bytes apart in plane 2 no matter how many rows the
 * font actually uses: an 8x8 cell is 8 bytes in a 32-byte slot. */
static u8 seq_read(u8 i) { outb(0x3C4, i); return inb(0x3C5); }
static u8 gc_read(u8 i)  { outb(0x3CE, i); return inb(0x3CF); }

static void font_write(u32 off, const u8 *rows, int n)
{
    u8 s2 = seq_read(2), s4 = seq_read(4);
    u8 g5 = gc_read(5),  g6 = gc_read(6);

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);      /* synchronous reset  */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);      /* writes go to plane 2 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06);      /* odd/even off, plain  */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);      /* reset done         */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);      /* write mode 0       */
    outb(0x3CE, 0x06); outb(0x3CF, 0x05);      /* a000 mapping, oe off */

    volatile u8 *dst = (volatile u8 *)(0xA0000 + off);
    for (int i = 0; i < n; i++) dst[i] = rows[i];

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);      /* put it all back    */
    outb(0x3C4, 0x02); outb(0x3C5, s2);
    outb(0x3C4, 0x04); outb(0x3C5, s4);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3CE, 0x05); outb(0x3CF, g5);
    outb(0x3CE, 0x06); outb(0x3CF, g6);
}

/* The 8x8 font for 50-line mode, made from the 8x16 one by keeping
 * every other row. Made once, on demand, into a scratch buffer. */
static u8 font8[256][8];          /* 2 KB of bss, reused every switch */
static int font8_ready;

static void font8_build(void)
{
    extern const u8 font8x16[256][16];
    for (int c = 0; c < 256; c++)
        for (int r = 0; r < 8; r++)
            font8[c][r] = font8x16[c][r * 2 + 1];   /* the lower half of */
    font8_ready = 1;                                /* each pair is the  */
}                                                   /* denser one        */

static void load_font8(void)
{
    if (!font8_ready) font8_build();
    for (int c = 0; c < 256; c++)
        font_write((u32)c * 32, font8[c], 8);
}

static void load_font16(void)
{
    extern const u8 font8x16[256][16];
    for (int c = 0; c < 256; c++)
        font_write((u32)c * 32, font8x16[c], 16);
}

/* Patch one character of the font the screen is using. `rows16` is 16
 * bytes (an 8x16 cell); in 50-line mode the top 8 of them are used. */
void gfx_set_glyph(int code, const u8 *rows16)
{
    if (code < 0 || code > 255 || !rows16) return;

    if (is_fb()) {
        con_font_override((u8)code, rows16);
        return;
    }

    font_write((u32)code * 32, rows16, lines == 50 ? 8 : 16);
}

static void hand_upload(void) { gfx_set_glyph(0xFF, hand_glyph); }

/* ---- VBE: 256 colours on a linear frame buffer -----------------------
 *
 * Mode 12h is four-bit planar and 1987. Everything from the mid-nineties
 * on also speaks VBE, and mode 101h is 640x480 with one byte per pixel
 * and 256 colours in a linear frame buffer (the 0x4000 bit in the mode
 * number asks for the linear one). Paging is off and the buffer sits
 * under 4 GB, so the physical address is simply an address.
 *
 * The 256-colour DAC is loaded from the same table a UEFI framebuffer
 * packs with, so the desktop looks identical either way, and programs
 * written against the sixteen CGA colours keep working: the low nibble
 * of every extended index stays the same hue. */
static void memset8(u8 *p, u8 v, u32 n) { for (u32 i = 0; i < n; i++) p[i] = v; }

static int  fb8;                       /* VBE linear 8-bit frame buffer    */
static u8 *fb8_base;
static u32 fb8_pitch;

static u8 vmi[256] __attribute__((aligned(16)));

static void dac_load(void)
{
    outb(0x3C6, 0xFF);                 /* pel mask: all bits               */
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        u32 rgb = con_pal256(i);
        outb(0x3C9, (u8)(((rgb >> 16) & 0xFF) >> 2));
        outb(0x3C9, (u8)(((rgb >> 8) & 0xFF) >> 2));
        outb(0x3C9, (u8)((rgb & 0xFF) >> 2));
    }
}

/* the same DAC, but with a caller-supplied palette (768 bytes of RGB) */
static void dac_load_raw(const u8 *pal)
{
    outb(0x3C6, 0xFF);
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        outb(0x3C9, (u8)(pal[i * 3] >> 2));
        outb(0x3C9, (u8)(pal[i * 3 + 1] >> 2));
        outb(0x3C9, (u8)(pal[i * 3 + 2] >> 2));
    }
}

/* the palette blit() uses on a firmware framebuffer (VBE holds its own
 * copy in the DAC). Set by gfx_setpal, one 768-byte copy. */
static u8 blit_pal[768];
static int blit_pal_set;

void gfx_setpal(const u8 *rgb)
{
    int i;
    if (!rgb) return;
    for (i = 0; i < 768; i++) blit_pal[i] = rgb[i];
    blit_pal_set = 1;
    if (fb8) dac_load_raw(rgb);
}

int gfx_is256(void)
{
    if (!gfx) return 0;
    if (fb8) return 1;
    if (is_fb()) return 1;
    return 0;             /* planar mode 12h: sixteen colours only */
}

/* An indexed frame (fw*fh bytes) to the screen, integer-scaled and
 * centred with black bars. VBE writes bytes straight to video memory;
 * a firmware framebuffer goes through con_fb_blit, which packs the
 * palette. Returns 0 on the planar fallback, which cannot do this. */
int gfx_blit(const u8 *frame, int fw, int fh)
{
    int sw, sh, scale, dw, dh, dx, dy, sx, sy, kx, ky;

    if (!gfx || !frame || fw <= 0 || fh <= 0 || !blit_pal_set) return 0;
    gfx_size(&sw, &sh);
    scale = sw / fw;
    if (sh / fh < scale) scale = sh / fh;
    if (scale < 1) scale = 1;
    dw = fw * scale;
    dh = fh * scale;
    dx = (sw - dw) / 2;
    dy = (sh - dh) / 2;

    if (fb8) {
        if (dy > 0) {                       /* letterbox, top and bottom */
            for (int y = 0; y < dy; y++)
                memset8(fb8_base + (u32)y * fb8_pitch, 0, (u32)sw);
            for (int y = dy + dh; y < sh; y++)
                memset8(fb8_base + (u32)y * fb8_pitch, 0, (u32)sw);
        }
        if (dx > 0) {
            for (int y = dy; y < dy + dh; y++) {
                memset8(fb8_base + (u32)y * fb8_pitch, 0, (u32)dx);
                memset8(fb8_base + (u32)y * fb8_pitch + (u32)(dx + dw),
                        0, (u32)(sw - dx - dw));
            }
        }
        for (sy = 0; sy < fh; sy++) {
            for (ky = 0; ky < scale; ky++) {
                u8 *row = fb8_base +
                    (u32)(dy + sy * scale + ky) * fb8_pitch + (u32)dx;
                for (sx = 0; sx < fw; sx++) {
                    u8 v = frame[(u32)sy * (u32)fw + (u32)sx];
                    for (kx = 0; kx < scale; kx++) row[sx * scale + kx] = v;
                }
            }
        }
        return 1;
    }
    if (is_fb()) {
        if (dy > 0) {
            con_fb_fill(0, 0, sw, dy, 0);
            con_fb_fill(0, dy + dh, sw, sh - dy - dh, 0);
        }
        if (dx > 0) {
            con_fb_fill(0, dy, dx, dh, 0);
            con_fb_fill(dx + dw, dy, sw - dx - dw, dh, 0);
        }
        con_fb_blit(frame, fw, fh, dx, dy, scale, blit_pal);
        return 1;
    }
    return 0;
}

static inline void cur_guard(int x, int y, int w, int h);
extern const u8 font8x16[256][16];

/* The desktop's screen update: copy one rectangle of a W x H indexed frame
 * to the same place on screen, no scaling. The mouse pointer is lifted out
 * of the way first and the caller puts it back. */
int gfx_blit_rect(const u8 *frame, int fw, int fh, int x, int y, int w, int h)
{
    if (!gfx || !frame || !blit_pal_set || fw <= 0 || fh <= 0) return 0;
    cur_guard(x, y, w, h);
    if (fb8) {
        int sw = 640, sh = 480;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > sw) w = sw - x;
        if (y + h > sh) h = sh - y;
        if (x + w > fw) w = fw - x;
        if (y + h > fh) h = fh - y;
        for (int r = 0; r < h; r++) {
            u8 *d = fb8_base + (u32)(y + r) * fb8_pitch + (u32)x;
            const u8 *s = frame + (u32)(y + r) * (u32)fw + (u32)x;
            for (int c = 0; c < w; c++) d[c] = s[c];
        }
        return 1;
    }
    if (is_fb()) {
        con_fb_blit_rect(frame, fw, x, y, w, h, blit_pal);
        return 1;
    }
    return 0;
}

const u8 *gfx_glyph(int c) { return font8x16[(u8)c]; }

static int vbe_try(void)
{
    u32 r, phys;
    u16 attr, pitch;
    u8  bpp, model;

    for (int i = 0; i < 256; i++) vmi[i] = 0;

    r = int10(0x4F01, 0, 0x0101, 0, vmi);      /* ask about mode 101h */
    if ((r & 0xFFFF) != 0x004F) return 0;
    attr  = (u16)(vmi[0] | (vmi[1] << 8));
    if (!(attr & 0x0001)) return 0;            /* hardware says no    */
    if (!(attr & 0x0080)) return 0;            /* no linear buffer    */
    bpp   = vmi[25];
    model = vmi[27];
    if (bpp != 8 || model != 4) return 0;      /* packed 8-bit only   */
    pitch = (u16)(vmi[16] | (vmi[17] << 8));
    phys  = (u32)vmi[40] | ((u32)vmi[41] << 8) | ((u32)vmi[42] << 16)
          | ((u32)vmi[43] << 24);
    /* 32-bit flat, paging off: anything under 4 GB is an address. A
     * frame buffer at 0xFD000000 (where an emulator puts it) or at the
     * top of a real machine's PCI space is equally fine. */
    if (!phys || (u64)phys + 640u * 480u > 0x100000000ULL) return 0;

    r = int10(0x4F02, 0x4101, 0, 0, 0);        /* set it, LFB bit on  */
    if ((r & 0xFFFF) != 0x004F) return 0;

    fb8_base  = (u8 *)phys;
    fb8_pitch = pitch ? pitch : 640;
    fb8 = 1;
    dac_load();    return 1;
}

/* ---- text modes ------------------------------------------------------ */

/* 25 or 50 lines of text, by hand rather than by int 0x10:
 *
 *  - 50 lines: load the 8-row font, tell the CRTC the character cell
 *    is 8 scanlines (register 9), and the same 400 scanlines that held
 *    25 rows of 16 now hold 50 rows of 8. The cursor shrinks too.
 *  - 25 lines: put the 16-row font back and the cell with it.
 *
 * A framebuffer machine has its geometry fixed by the firmware loader;
 * this says so rather than pretending. */
int gfx_lines(int want)
{
    if (is_fb()) return 0;

    if (want == 50) {
        load_font8();
        outb(0x3D4, 0x09); outb(0x3D5, 0x07);      /* 8-line cells    */
        outb(0x3D4, 0x0A); outb(0x3D5, 0x06);      /* cursor 6..7     */
        outb(0x3D4, 0x0B); outb(0x3D5, 0x07);
        lines = 50;
    } else {
        outb(0x3D4, 0x09); outb(0x3D5, 0x0F);      /* 16-line cells   */
        outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);      /* cursor 14..15   */
        outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);
        load_font16();
        lines = 25;
    }
    con_set_rows(lines);
    hand_upload();
    return lines;
}

int gfx_lines_now(void) { return lines; }

/* ---- the drawing screen ---------------------------------------------- */

static int cur_on, cur_vis;          /* the mouse pointer, see below */

int gfx_mode(int on)
{
    cur_vis = 0;                     /* the screen is about to be repainted */
    cur_on = 0;
    if (is_fb()) {
        /* already a framebuffer: nothing to switch, just remember it,
         * and point the mouse at the pixel grid */
        gfx = on ? 1 : 0;
        if (on) {
            int w = 0, h = 0;
            con_fb_dims(&w, &h);
            mouse_pixel_mode(w, h);
        } else {
            mouse_pixel_mode(0, 0);
            con_clear();
        }
        return 1;
    }

    if (on) {
        fb8 = 0;
        blit_pal_set = 0;
        if (vbe_try()) {                    /* 640x480x256 first */
            gfx = 1;
            mouse_pixel_mode(640, 480);
            return 1;
        }
        int10(0x0012, 0, 0, 0, 0);           /* 640x480x16 */
        /* write mode 2: the byte written is the colour; the bit mask
         * register decides which of the eight bits in the addressed
         * byte it lands in. Set once, used per pixel. */
        outb(0x3CE, 5);  outb(0x3CF, 2);
        outb(0x3CE, 8);  outb(0x3CF, 0xFF);
        gfx = 1;
        mouse_pixel_mode(640, 480);
    } else {
        int10(0x0003, 0, 0, 0, 0);           /* back to 80x(25|50) text */
        gfx = 0;
        fb8 = 0;
        mouse_pixel_mode(0, 0);
        con_set_rows(lines);                 /* re-clear, reset CRTC    */
        hand_upload();
    }
    return 1;
}

int gfx_ready(void) { return gfx; }

/* ---- the speaker's off switch ----------------------------------------
 * The PC speaker is a square wave with no volume knob -- the two honest
 * controls are pitch and silence, so those are the two there are. */
static int snd = 1;

void sound_set(int on) { snd = on ? 1 : 0; }
int  sound_is_on(void) { return snd; }


/* ---- the mouse pointer ------------------------------------------------
 *
 * A software pointer, drawn by the kernel straight on the framebuffer so
 * every program that asks for the mouse gets one for free. It remembers the
 * pixels underneath and puts them back before moving. A program that draws
 * on top of it (gfx_pixel / gfx_fill) makes it hide first, and it comes
 * back the next time the pointer is updated -- the program polls the mouse
 * every frame, so that is within a few tens of milliseconds.
 *
 * Only on the 256-colour VBE screen and the firmware framebuffer: the
 * 16-colour planar fallback has no cheap way to read a pixel back. */
#define CUR_W 12
#define CUR_H 19
static const char *const cur_art[CUR_H] = {
    "X           ", "XX          ", "XOX         ", "XOOX        ",
    "XOOOX       ", "XOOOOX      ", "XOOOOOX     ", "XOOOOOOX    ",
    "XOOOOOOOX   ", "XOOOOOOOOX  ", "XOOOOOOXXXXX", "XOOXOOX     ",
    "XOX XOOX    ", "XX  XOOX    ", "X    XOOX   ", "     XOOX   ",
    "      XOOX  ", "      XOOX  ", "       XX   "
};
static int  cur_on, cur_vis, cur_x, cur_y, cur_sc = 1;
static u32  cur_save[CUR_W * CUR_H * 4];

extern u32  con_fb_peek(int x, int y);
extern void con_fb_poke(int x, int y, u32 v);

static int cur_ok(void) { return gfx && (fb8 || is_fb()); }

static void cur_dims(int *w, int *h)
{
    if (fb8) { *w = 640; *h = 480; }
    else con_fb_dims(w, h);
}

static u32 raw_get(int x, int y)
{
    if (fb8) return fb8_base[(u32)y * fb8_pitch + (u32)x];
    return con_fb_peek(x, y);
}

static void raw_put(int x, int y, u32 v)
{
    if (fb8) fb8_base[(u32)y * fb8_pitch + (u32)x] = (u8)v;
    else con_fb_poke(x, y, v);
}

static void cur_plot(int x, int y, int idx)
{
    if (fb8) fb8_base[(u32)y * fb8_pitch + (u32)x] = (u8)idx;
    else con_fb_putpixel(x, y, (u8)idx);
}

static void cur_hide(void)
{
    if (!cur_vis) return;
    int sw, sh;
    cur_dims(&sw, &sh);
    int n = 0;
    for (int r = 0; r < CUR_H * cur_sc; r++)
        for (int c = 0; c < CUR_W * cur_sc; c++, n++) {
            int x = cur_x + c, y = cur_y + r;
            if (x >= 0 && y >= 0 && x < sw && y < sh) raw_put(x, y, cur_save[n]);
        }
    cur_vis = 0;
}

static void cur_show(void)
{
    if (!cur_on || cur_vis || !cur_ok()) return;
    int sw, sh;
    cur_dims(&sw, &sh);
    int n = 0;
    for (int r = 0; r < CUR_H * cur_sc; r++)
        for (int c = 0; c < CUR_W * cur_sc; c++, n++) {
            int x = cur_x + c, y = cur_y + r;
            cur_save[n] = (x >= 0 && y >= 0 && x < sw && y < sh) ? raw_get(x, y) : 0;
        }
    for (int r = 0; r < CUR_H * cur_sc; r++)
        for (int c = 0; c < CUR_W * cur_sc; c++) {
            char ch = cur_art[r / cur_sc][c / cur_sc];
            int x = cur_x + c, y = cur_y + r;
            if (ch == ' ' || x < 0 || y < 0 || x >= sw || y >= sh) continue;
            cur_plot(x, y, ch == 'X' ? 0 : 15);
        }
    cur_vis = 1;
}

/* a program is about to draw here: get the pointer out of the way */
static inline void cur_guard(int x, int y, int w, int h)
{
    if (cur_vis &&
        x < cur_x + CUR_W * cur_sc && x + w > cur_x &&
        y < cur_y + CUR_H * cur_sc && y + h > cur_y)
        cur_hide();
}

void gfx_cursor(int x, int y, int on)
{
    if (!on || !cur_ok()) {
        cur_hide();
        cur_on = 0;
        return;
    }
    int sw, sh;
    cur_dims(&sw, &sh);
    cur_sc = (sw >= 1800) ? 2 : 1;
    cur_on = 1;
    if (cur_vis && x == cur_x && y == cur_y) return;
    cur_hide();
    cur_x = x;
    cur_y = y;
    cur_show();
}

/* 8x16 text in the kernel font: crisp, and fast enough for a desktop.
 * bg < 0 leaves the background alone. */
void gfx_text(int x, int y, const char *s, int fg, int bg)
{
    if (!gfx) return;
    int len = 0;
    while (s[len]) len++;
    if (bg >= 0) gfx_fill(x, y, len * 8, 16, bg);
    for (int i = 0; i < len; i++) {
        const u8 *gl = font8x16[(u8)s[i]];
        for (int r = 0; r < 16; r++) {
            u8 bits = gl[r];
            if (!bits) continue;
            for (int c = 0; c < 8; c++)
                if (bits & (0x80 >> c)) gfx_pixel(x + i * 8 + c, y + r, fg);
        }
    }
}

void gfx_size(int *w, int *h)
{
    if (is_fb()) {
        con_fb_dims(w, h);
    } else {
        if (w) *w = 640;
        if (h) *h = 480;
    }
}

void gfx_pixel(int x, int y, int c)
{
    if (!gfx) return;
    cur_guard(x, y, 1, 1);
    /* PICO-FIX-gfxclip: on a firmware framebuffer the screen is bigger
     * than 640x480 (usually 1280x800) and con_fb_putpixel already clips
     * at the real fb_w/fb_h, so the legacy 640x480 check must not run
     * there -- it silently ate every icon and text pixel past x=639. */
    if (!is_fb() && (x < 0 || y < 0 || x > 639 || y > 479)) return;

    if (fb8) {                           /* VBE: a byte per pixel      */
        fb8_base[(u32)y * fb8_pitch + (u32)x] = (u8)(c & 255);
        return;
    }
    if (is_fb()) {
        con_fb_putpixel(x, y, (u8)(c & 255));
        return;
    }
    c &= 15;

    /* mode 12h: one byte covers 8 horizontal pixels; latch a read so
     * the other seven survive, mask our bit, write the colour */
    volatile u8 *v = (volatile u8 *)(0xA0000 + y * 80 + (x >> 3));
    (void)*v;                                 /* load the latches      */
    outb(0x3CE, 8);
    outb(0x3CF, 0x80 >> (x & 7));
    *v = (u8)c;
}

void gfx_fill(int x, int y, int w, int h, int c)
{
    if (!gfx || w <= 0 || h <= 0) return;
    cur_guard(x, y, w, h);

    if (fb8) {                           /* VBE: a byte per pixel      */
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > 640) w = 640 - x;
        if (y + h > 480) h = 480 - y;
        if (w <= 0 || h <= 0) return;
        u8 v = (u8)(c & 255);
        for (int j = 0; j < h; j++)
            memset8(fb8_base + (u32)(y + j) * fb8_pitch + (u32)x, v, (u32)w);
        return;
    }
    if (is_fb()) {
        con_fb_fill(x, y, w, h, (u8)(c & 255));
        return;
    }

    /* mode 12h, filled a byte at a time. A per-pixel fill pays two port
     * writes per pixel -- nearly a million of them for one full screen,
     * which a virtual machine turns into a slow scan you can watch.
     * Whole bytes need the mask register set once: two mask writes per
     * row at the edges, and the middle of the row is plain stores.    */
    c &= 15;
    int x1 = x + w - 1;
    if (x1 > 639) x1 = 639;
    if (x < 0) x = 0;
    outb(0x3CE, 8);
    for (int yy = y; yy < y + h && yy < 480; yy++) {
        volatile u8 *row = (volatile u8 *)(0xA0000 + yy * 80);
        int i  = x >> 3;
        int i1 = x1 >> 3;
        u8 lm = (u8)(0xFF >> (x & 7));
        u8 rm = (u8)~(0x7F >> (x1 & 7));
        if (i == i1) {
            outb(0x3CF, lm & rm);
            row[i] = (u8)c;
        } else {
            outb(0x3CF, lm);
            row[i++] = (u8)c;
            outb(0x3CF, 0xFF);
            while (i < i1) row[i++] = (u8)c;
            outb(0x3CF, rm);
            row[i] = (u8)c;
        }
    }
}

