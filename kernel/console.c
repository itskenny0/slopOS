#include "pico.h"

/* One console API, two backends:
 *   - legacy VGA text memory at 0xB8000 (BIOS boot)
 *   - a linear framebuffer with an 8x16 software font (UEFI boot)
 */

#define GLYPH_W 8
#define GLYPH_H 16
#define MAX_COLS 256

static int use_fb = 0;
static int cols = 80, rows = 25;
static int cx = 0, cy = 0;

/* The colour is per task, kept in the task struct, so a background program
 * cannot recolour the foreground's output. Before task_init runs there are
 * no tasks, so we fall back to a plain global -- the whole boot sequence
 * draws through this one. */
static u8  color0 = 0x07;
static inline u8 *ccol(void)
{
    u8 *p = task_colorp();
    return p ? p : &color0;
}
#define color (*ccol())

/* --- VGA text state ---
 *
 * `origin` is where the visible page starts inside the 32 KB text plane,
 * counted in characters. Scrolling a text console the obvious way means
 * moving 1920 characters down one line, and every one of those is a read
 * and a write across the bus to the video card. On the 2011 desktop this
 * was first noticed on, `help` printing fifty lines took visibly longer
 * than reading them.
 *
 * The VGA CRTC has kept the answer in two registers since 1987: tell it
 * which character to start displaying at and the whole screen shifts with
 * no memory traffic at all. So scrolling is `origin += 80`, clear the line
 * that just came into view, and write two registers. Every 179 scrolls the
 * page runs out of plane and we copy it back to the start -- one real
 * scroll for every 179 free ones. */
static volatile u16 *vram = (u16 *)0xB8000;
#define PLANE_WORDS  16384              /* 32 KB of text plane */
static u32 origin = 0;

static inline volatile u16 *cell(int x, int y)
{
    return &vram[origin + (u32)(y * cols + x)];
}

static void crtc_origin(void);

/* --- framebuffer state ---
 *
 * `shadow` is what is currently on the screen, one 16-bit character-and-
 * attribute per cell, exactly like the VGA text plane. It exists to avoid
 * ever reading the framebuffer back.
 *
 * Reading video memory is the expensive direction. Writes are posted -- the
 * processor hands them to the bus and moves on -- but a read stalls until
 * the data comes back across the same bus, and the framebuffer is mapped
 * uncached, so nothing is helping. Scrolling by copying the framebuffer up
 * one row was 1.5 MB read and 1.5 MB written on a 1024x768 screen, in a
 * byte-at-a-time memcpy, for every single line of output. That is the
 * "help scrolls slowly" bug, and it never showed up in an emulator because
 * an emulator's framebuffer is ordinary host memory.
 *
 * With a shadow the scroll becomes: shift 10 KB of RAM, then redraw only
 * the cells whose contents actually changed. Most of a terminal is blank,
 * so a screen of help text redraws a few hundred glyphs instead of moving a
 * megabyte and a half. Nothing is read back from the card at all.
 *
 * The buffer is allocated after the heap comes up. If it cannot be had, the
 * old copy path is still there. */
static u8  *fb;
static u32  fb_pitch, fb_bpp_bytes, fb_w, fb_h;
static int  fb_bgr;
static u16 *shadow;

/* the classic 16-colour CGA palette, as 0x00RRGGBB */
static const u32 palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* the 256-colour palette. Indices 0-15 are the classic CGA set -- every
 * app ever built against the pixel calls uses those and keeps working.
 * Above that, index & 15 stays the same hue, darker or lighter: a
 * desktop picked for 256 colours degrades to a sensible 16-colour look
 * on a machine that only has the old planar mode. */
u32 pal256[256];

static void pal256_init(void)
{
    for (int i = 0; i < 256; i++) {
        int h = i & 15, s = i >> 4;
        u32 base = palette[h];
        u32 r = (base >> 16) & 0xFF, g = (base >> 8) & 0xFF, b = base & 0xFF;
        if (s) {
            int num, den;
            if (s <= 8) { num = 16 - 2 * s; den = 16; }
            else        { num = 12 + (s - 8); den = 8; }
            r = r * num / den; g = g * num / den; b = b * num / den;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
        }
        pal256[i] = (r << 16) | (g << 8) | b;
    }
    /* a few hand-mixed tones the desktop likes: every one of them keeps
     * its CGA hue in the low nibble */
    pal256[33] = 0x102A66;      /* deep navy        (blue)      */
    pal256[17] = 0x18224E;      /* darker navy      (blue)      */
    pal256[49] = 0x3E6EE0;      /* strong blue      (blue)      */
    pal256[34] = 0x46B45C;      /* green            (green)     */
    pal256[39] = 0x6E6A66;      /* bevel shadow     (grey)      */
    pal256[55] = 0xD4D0C8;      /* light panel      (grey)      */
    pal256[63] = 0xF0F4F8;      /* highlight        (white)     */
    pal256[51] = 0x7CC8FF;      /* bright cyan      (cyan)      */
    pal256[44] = 0xE06030;      /* orange           (red)       */
    pal256[18] = 0x1E6432;      /* dark green       (green)     */
    pal256[62] = 0xE8E8E0;      /* warm white       (yellow)    */

    /* the 2.1 desktop: a dark theme with a blue accent. These sit on
     * indices the older programs never use. */
    pal256[80]  = 0x0B1626;  pal256[96]  = 0x12294A;  pal256[112] = 0x1C4577;
    pal256[128] = 0x2A6AA8;  pal256[144] = 0x4A8FD0;           /* wallpaper */
    pal256[160] = 0x141820;  pal256[176] = 0x222833;  pal256[192] = 0x2E3646;
    pal256[208] = 0x3A4458;  pal256[224] = 0x566176;  pal256[240] = 0xF3F5F8;
    pal256[161] = 0x1B2029;  pal256[177] = 0x1A1F29;  pal256[193] = 0x2A3140;
    pal256[81]  = 0x2F7BF0;  pal256[97]  = 0x5B9BFF;           /* accent    */
    pal256[82]  = 0x35C46B;  pal256[84]  = 0xE5484D;  pal256[86]  = 0xF08C2E;
    pal256[85]  = 0xB07CF0;  pal256[83]  = 0x3CC8D8;
    pal256[200] = 0xA8B2C4;  pal256[216] = 0x7C879B;           /* text      */
}

/* the DAC on a VBE machine and the packer on a framebuffer machine both
 * want the same table, so it lives here and is filled on first use */
u32 con_pal256(int idx)
{
    static int ready;
    if (!ready) { pal256_init(); ready = 1; }
    return pal256[idx & 255];
}


static inline u32 pack(u32 rgb)
{
    if (fb_bgr) return rgb;                       /* BGRA memory order */
    u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (b << 16) | (g << 8) | r;
}

static inline void put_pixel(u32 x, u32 y, u32 c)
{
    u8 *p = fb + y * fb_pitch + x * fb_bpp_bytes;
    if (fb_bpp_bytes == 4)      *(u32 *)p = c;
    else if (fb_bpp_bytes == 3) { p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16); }
    else if (fb_bpp_bytes == 2) {
        u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        *(u16 *)p = (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
}

/* Fill a band of scanlines with one colour, four bytes at a time.
 * put_pixel in a double loop was costing a multiply and a bounds-free
 * pointer rebuild per pixel; clearing a 1024x768 screen that way is three
 * quarters of a million of them. */
static void fb_fill_rows(u32 y0, u32 y1, u32 c)
{
    if (y1 > fb_h) y1 = fb_h;
    for (u32 y = y0; y < y1; y++) {
        u8 *p = fb + y * fb_pitch;
        if (fb_bpp_bytes == 4) {
            u32 *q = (u32 *)p;
            for (u32 x = 0; x < fb_w; x++) q[x] = c;
        } else {
            for (u32 x = 0; x < fb_w; x++) put_pixel(x, y, c);
        }
    }
}

/* memcpy over a framebuffer is a byte loop over uncached video memory --
 * four bus transactions where one would do. */
static void fb_move_up(u32 bytes, u32 delta)
{
    u32 *d = (u32 *)fb;
    const u32 *s = (const u32 *)(fb + delta);
    u32 n = bytes / 4;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}

/* ---- glyph overrides, new in 1.0 ------------------------------------
 * Eight cells at the top of the character set can be replaced at run
 * time. The mouse pointer is one drawn glyph instead of a letter, which
 * is how a text screen gets a hand with a finger on it. */
static struct { u8 rows[16]; u8 set; } glyph_ovr[8];

static void fb_draw_glyph(int col, int row, char ch, u8 attr)
{
    const u8 *g = font8x16[(u8)ch];
    /* cells the owner has patched (the hand the mouse wears) override
     * the built-in font */
    if ((u8)ch >= 0xF8 && glyph_ovr[(u8)ch - 0xF8].set)
        g = glyph_ovr[(u8)ch - 0xF8].rows;
    u32 fg = pack(palette[attr & 0x0F]);
    u32 bg = pack(palette[(attr >> 4) & 0x0F]);
    u32 x0 = (u32)col * GLYPH_W, y0 = (u32)row * GLYPH_H;

    for (int y = 0; y < GLYPH_H; y++) {
        u8 bits = g[y];
        for (int x = 0; x < GLYPH_W; x++)
            put_pixel(x0 + (u32)x, y0 + (u32)y, (bits & (0x80 >> x)) ? fg : bg);
    }
}

void con_font_override(u8 code, const u8 *rows16)
{
    if (code < 0xF8) return;
    int i = code - 0xF8;
    for (int y = 0; y < 16; y++) glyph_ovr[i].rows[y] = rows16[y];
    glyph_ovr[i].set = 1;
}

/* One pixel straight to the framebuffer, in a CGA-palette colour. For
 * the drawing program on a firmware-booted machine. */
void con_fb_putpixel(int x, int y, u8 c)
{
    if (!use_fb) return;
    if (x < 0 || y < 0 || (u32)x >= fb_w || (u32)y >= fb_h) return;
    put_pixel((u32)x, (u32)y, pack(con_pal256(c)));
}

/* A rectangle straight to the framebuffer, one row of u32s at a time:
 * a full-screen repaint through put_pixel walks two million addresses
 * one at a time, and this walks each row once. */
void con_fb_fill(int x, int y, int w, int h, u8 c)
{
    if (!use_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if ((u32)(x + w) > fb_w) w = (int)fb_w - x;
    if ((u32)(y + h) > fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    u32 v = pack(con_pal256(c));
    if (fb_bpp_bytes != 4) {
        for (int r = 0; r < h; r++)
            for (int i = 0; i < w; i++)
                put_pixel((u32)(x + i), (u32)(y + r), v);
        return;
    }
    for (int r = 0; r < h; r++) {
        u32 *p = (u32 *)(fb + ((u32)(y + r) * fb_pitch) + (u32)x * 4u);
        for (int i = 0; i < w; i++) p[i] = v;
    }
}

/* Raw framebuffer access for the software mouse pointer: whatever bytes are
 * on screen, read and put back unchanged. */
u32 con_fb_peek(int x, int y)
{
    if (!use_fb || x < 0 || y < 0 || (u32)x >= fb_w || (u32)y >= fb_h) return 0;
    u8 *p = fb + (u32)y * fb_pitch + (u32)x * fb_bpp_bytes;
    u32 v = 0;
    for (u32 i = 0; i < fb_bpp_bytes; i++) v |= (u32)p[i] << (8 * i);
    return v;
}

void con_fb_poke(int x, int y, u32 v)
{
    if (!use_fb || x < 0 || y < 0 || (u32)x >= fb_w || (u32)y >= fb_h) return;
    u8 *p = fb + (u32)y * fb_pitch + (u32)x * fb_bpp_bytes;
    for (u32 i = 0; i < fb_bpp_bytes; i++) p[i] = (u8)(v >> (8 * i));
}

void con_fb_dims(int *w, int *h)
{
    if (w) *w = (int)fb_w;
    if (h) *h = (int)fb_h;
}

/* An indexed frame straight to the firmware framebuffer, integer-scaled
 * by `scale` and placed at (dx,dy), through a caller-supplied palette
 * (768 bytes of RGB). This is DOOM's whole screen update: the palette is
 * packed once per call, then every destination pixel is one table lookup
 * and one store, with a fast 32-bit path for the common case. */
/* Copy one rectangle of an indexed frame to the same coordinates on the
 * screen (scale 1). The desktop draws into a memory buffer and sends only
 * what changed, which is what stops it flickering. */
void con_fb_blit_rect(const u8 *frame, int fw, int x, int y, int w, int h,
                      const u8 *pal)
{
    u32 packpal[256];

    if (!use_fb || !frame || !pal || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if ((u32)x >= fb_w || (u32)y >= fb_h) return;
    if ((u32)(x + w) > fb_w) w = (int)fb_w - x;
    if ((u32)(y + h) > fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < 256; i++) {
        u32 rgb = ((u32)pal[i * 3] << 16) |
                  ((u32)pal[i * 3 + 1] << 8) | (u32)pal[i * 3 + 2];
        packpal[i] = pack(rgb);
    }
    for (int r = 0; r < h; r++) {
        const u8 *src = frame + (u32)(y + r) * (u32)fw + (u32)x;
        if (fb_bpp_bytes == 4) {
            u32 *row = (u32 *)(fb + (u32)(y + r) * fb_pitch) + x;
            for (int c = 0; c < w; c++) row[c] = packpal[src[c]];
        } else {
            for (int c = 0; c < w; c++)
                put_pixel((u32)(x + c), (u32)(y + r), packpal[src[c]]);
        }
    }
}

void con_fb_blit(const u8 *frame, int fw, int fh, int dx, int dy,
                 int scale, const u8 *pal)
{
    u32 packpal[256];
    int sx, sy, kx, ky;

    if (!use_fb || !frame || !pal || fw <= 0 || fh <= 0 || scale < 1)
        return;
    for (int i = 0; i < 256; i++) {
        u32 rgb = ((u32)pal[i * 3] << 16) |
                  ((u32)pal[i * 3 + 1] << 8) | (u32)pal[i * 3 + 2];
        packpal[i] = pack(rgb);
    }
    if (fb_bpp_bytes == 4) {
        for (sy = 0; sy < fh; sy++) {
            for (ky = 0; ky < scale; ky++) {
                int y = dy + sy * scale + ky;
                u32 *row;
                if (y < 0 || (u32)y >= fb_h) continue;
                row = (u32 *)(fb + (u32)y * fb_pitch) + dx;
                for (sx = 0; sx < fw; sx++) {
                    u32 v = packpal[frame[(u32)sy * (u32)fw + (u32)sx]];
                    for (kx = 0; kx < scale; kx++) {
                        int x = dx + sx * scale + kx;
                        if (x >= 0 && (u32)x < fb_w) row[sx * scale + kx] = v;
                    }
                }
            }
        }
        return;
    }
    /* 24- and 16-bit firmwares: correct before fast */
    for (sy = 0; sy < fh; sy++)
        for (sx = 0; sx < fw; sx++) {
            u32 v = packpal[frame[(u32)sy * (u32)fw + (u32)sx]];
            for (ky = 0; ky < scale; ky++)
                for (kx = 0; kx < scale; kx++)
                    put_pixel((u32)(dx + sx * scale + kx),
                              (u32)(dy + sy * scale + ky), v);
        }
}

/* A different number of text rows (80x50 on a BIOS boot). VGA text only:
 * the shadow buffer belongs to the framebuffer path and is sized for the
 * rows the loader left us, so this never touches it. */
void con_set_rows(int r)
{
    if (use_fb) return;
    if (r != 25 && r != 50) return;
    rows = r;
    origin = 0;
    crtc_origin();
    cx = cy = 0;
    con_clear();
}

/* ------------------------------------------------------------------ */
static int cursor_on = 1;

static void hw_cursor(void)
{
    if (!cursor_on) return;
    if (use_fb) {
        /* software cursor: a two-pixel bar under the character cell */
        u32 x0 = (u32)cx * GLYPH_W, y0 = (u32)cy * GLYPH_H;
        u32 c = pack(palette[color & 0x0F]);
        for (int y = GLYPH_H - 3; y < GLYPH_H - 1; y++)
            for (int x = 0; x < GLYPH_W; x++)
                put_pixel(x0 + (u32)x, y0 + (u32)y, c);
        return;
    }
    u16 pos = (u16)(origin + (u32)(cy * cols + cx));
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

/* Put back whatever the cursor was drawn on top of.
 *
 * This used to repaint the cursor bar in the current background colour,
 * which is only correct if nothing changed colour in between. A program
 * that draws in several colours -- a desktop, say -- left a trail of stray
 * underscores in the wrong colour all over the screen. The shadow buffer
 * knows the character and the attribute of every cell, so redraw the cell
 * properly and the question of which colour to use does not arise. */
static void erase_cursor(void)
{
    if (!use_fb || !cursor_on) return;

    if (shadow) {
        u16 cell = shadow[cy * cols + cx];
        fb_draw_glyph(cx, cy, (char)(cell & 0xFF), (u8)(cell >> 8));
        return;
    }

    u32 x0 = (u32)cx * GLYPH_W, y0 = (u32)cy * GLYPH_H;
    u32 c = pack(palette[(color >> 4) & 0x0F]);
    for (int y = GLYPH_H - 3; y < GLYPH_H - 1; y++)
        for (int x = 0; x < GLYPH_W; x++)
            put_pixel(x0 + (u32)x, y0 + (u32)y, c);
}

/* A full-screen program has no use for a text cursor following its drawing
 * around, and on the framebuffer console every cursor move costs a glyph
 * redraw. Switching it off is one register on VGA and one flag here. */
void con_cursor(int on)
{
    if (on == cursor_on) return;
    if (!on) erase_cursor();
    cursor_on = on;
    if (!use_fb) {
        outb(0x3D4, 0x0A);
        outb(0x3D5, on ? 14 : 0x20);        /* bit 5 hides it */
    }
    if (on) hw_cursor();
}

void con_setcolor(u8 fg, u8 bg) { color = (u8)((bg << 4) | (fg & 0x0F)); }
u8   con_getcolor(void)         { return color; }
int  con_width(void)            { return cols; }
int  con_height(void)           { return rows; }
const char *con_driver(void)    { return use_fb ? "framebuffer" : "VGA text"; }

void con_clear(void)
{
    if (!task_owns_console()) return;
    if (use_fb) {
        fb_fill_rows(0, fb_h, pack(palette[(color >> 4) & 0x0F]));
        if (shadow) {
            u16 blank = (u16)((color << 8) | ' ');
            for (int i = 0; i < cols * rows; i++) shadow[i] = blank;
        }
    } else {
        /* back to the top of the plane: a clear is the natural moment to
         * reclaim everything the hardware scroll has walked through */
        origin = 0;
        crtc_origin();
        for (int i = 0; i < cols * rows; i++)
            vram[i] = (u16)(color << 8) | ' ';
    }
    cx = cy = 0;
    hw_cursor();
}

/* Called once the heap exists. Until then the console works without a
 * shadow, which is fine: the boot messages do not scroll. */
void con_shadow_init(void)
{
    if (!use_fb || shadow) return;

    u32 cells = (u32)cols * (u32)rows;
    shadow = (u16 *)kmalloc(cells * sizeof(u16));
    if (!shadow) return;

    /* The screen is whatever con_clear last left: blanks in the current
     * colour, plus the boot text. We only know the blanks for certain, so
     * record those and let the next scroll redraw the rest. */
    u16 blank = (u16)((color << 8) | ' ');
    for (u32 i = 0; i < cells; i++) shadow[i] = blank;
}

void con_init(void)
{
    bootinfo_t *bi = bootinfo();

    if (bi && bi->fb_base) {
        use_fb   = 1;
        fb       = (u8 *)bi->fb_base;
        fb_w     = bi->fb_width;
        fb_h     = bi->fb_height;
        fb_pitch = bi->fb_pitch;
        fb_bpp_bytes = (bi->fb_bpp + 7) / 8;
        fb_bgr   = (int)bi->fb_bgr;
        cols = (int)(fb_w / GLYPH_W);
        rows = (int)(fb_h / GLYPH_H);
        if (cols > MAX_COLS) cols = MAX_COLS;
    } else {
        use_fb = 0;
        cols = 80; rows = 25;
        outb(0x3D4, 0x0A); outb(0x3D5, 14);     /* show the hardware cursor */
        outb(0x3D4, 0x0B); outb(0x3D5, 15);
    }
    con_setcolor(LGREY, BLACK);
    con_clear();
}

static void scroll(void)
{
    if (cy < rows) return;

    if (use_fb && shadow) {
        /* Walk top down. Row y takes what row y+1 has, and row y+1 has not
         * been touched yet at that point, so one buffer is enough. A cell
         * that ends up with the same character and colour it already had
         * is left alone -- that is where the saving comes from. */
        u16 blank = (u16)((color << 8) | ' ');

        for (int y = 0; y < rows; y++) {
            const u16 *src = (y + 1 < rows) ? &shadow[(y + 1) * cols] : NULL;
            u16 *dst = &shadow[y * cols];

            for (int x = 0; x < cols; x++) {
                u16 want = src ? src[x] : blank;
                if (dst[x] == want) continue;
                dst[x] = want;
                fb_draw_glyph(x, y, (char)(want & 0xFF), (u8)(want >> 8));
            }
        }
    } else if (use_fb) {
        u32 line = (u32)GLYPH_H * fb_pitch;
        fb_move_up((u32)(rows - 1) * line, line);
        fb_fill_rows((u32)(rows - 1) * GLYPH_H, (u32)rows * GLYPH_H,
                     pack(palette[(color >> 4) & 0x0F]));
    } else {
        u32 next = origin + (u32)cols;

        /* Out of plane: fold the visible page back to the start. This is
         * the only copy the text console ever does, and it happens once
         * every (16384 - 2000) / 80 lines. */
        if (next + (u32)(rows * cols) > PLANE_WORDS) {
            for (int i = 0; i < (rows - 1) * cols; i++)
                vram[i] = vram[next + (u32)i];
            origin = 0;
        } else {
            origin = next;
        }

        crtc_origin();
        for (int i = 0; i < cols; i++)
            *cell(i, rows - 1) = (u16)(color << 8) | ' ';
    }
    cy = rows - 1;
}

static void raw_put(char c)
{
    if (use_fb) {
        fb_draw_glyph(cx, cy, c, color);
        if (shadow) shadow[cy * cols + cx] = (u16)((color << 8) | (u8)c);
    } else {
        *cell(cx, cy) = (u16)(color << 8) | (u8)c;
    }
}

/* Point the CRTC at the current origin. Two 8-bit registers, one 16-bit
 * character address, and the entire screen has scrolled. */
static void crtc_origin(void)
{
    outb(0x3D4, 0x0C); outb(0x3D5, (u8)((origin >> 8) & 0xFF));
    outb(0x3D4, 0x0D); outb(0x3D5, (u8)(origin & 0xFF));
}

void con_putc(char c)
{
    /* putc() already caught the background case and buffered it; this
     * guard is for the few places that draw straight to the console. */
    if (!task_owns_console()) return;

    erase_cursor();

    if (c == '\n')      { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\t') { cx = (cx + 4) & ~3; }
    else if (c == '\b') {
        if (cx == 0 && cy == 0) { hw_cursor(); return; }
        if (cx == 0) { cx = cols - 1; cy--; } else cx--;
        char sp = ' ';
        raw_put(sp);
        hw_cursor();
        return;
    }
    else { raw_put(c); cx++; }

    if (cx >= cols) { cx = 0; cy++; }
    scroll();
    hw_cursor();
}

void con_backspace(void) { con_putc('\b'); }
void con_puts(const char *s) { while (*s) con_putc(*s++); }

void con_set_cursor(int x, int y)
{
    if (!task_owns_console()) return;
    erase_cursor();
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    if (y < 0) y = 0;
    if (y >= rows) y = rows - 1;
    cx = x; cy = y;
    hw_cursor();
}
