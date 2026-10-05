/* draw.pico -- paint with real pixels, not characters.
 *
 * The drawing screen is 640x480 in 16 colours (the BIOS boot) or
 * whatever the firmware left behind (a UEFI boot). Lines, boxes,
 * circles and a free-hand brush are drawn one pixel at a time through
 * the kernel's pixel call, and a picture survives the program: S packs
 * it run-length-encoded into a file, O reads it back.
 *
 * The mouse is the short path. Without one the arrow keys walk the
 * brush, and the space bar paints with the current tool.
 */

#include "picoapp.h"

#define KEY_ESC  27
#define KEY_BS   8
#define KEY_TAB  9

static int W, H;
static int is_planar;              /* the VGA planar mode, readable      */

/* ---- a 3x5 pixel font, so the screen can say what it is doing ------ */
/* each glyph: three bytes, one per column, five bits low-to-high      */
static const u8 glyph_35[41][3] = {
    {0x1e,0x05,0x1e}, {0x1f,0x15,0x0a}, {0x0e,0x11,0x11}, {0x1f,0x11,0x0e},
    {0x1f,0x15,0x11}, {0x1f,0x05,0x01}, {0x0e,0x11,0x1d}, {0x1f,0x04,0x1f},
    {0x11,0x1f,0x11}, {0x08,0x10,0x0f}, {0x1f,0x04,0x1b}, {0x1f,0x10,0x10},
    {0x1f,0x02,0x1f}, {0x1f,0x01,0x1e}, {0x0e,0x11,0x0e}, {0x1f,0x05,0x02},
    {0x06,0x09,0x16}, {0x1f,0x05,0x1a}, {0x12,0x15,0x09}, {0x01,0x1f,0x01},
    {0x1f,0x10,0x1f}, {0x0f,0x10,0x0f}, {0x1f,0x08,0x1f}, {0x1b,0x04,0x1b},
    {0x03,0x1c,0x03}, {0x19,0x15,0x13},
    /* digits */
    {0x1f,0x11,0x1f}, {0x12,0x1f,0x10}, {0x19,0x15,0x12}, {0x11,0x15,0x1f},
    {0x07,0x04,0x1f}, {0x17,0x15,0x09}, {0x1e,0x15,0x1d}, {0x01,0x19,0x07},
    {0x1f,0x15,0x1f}, {0x17,0x15,0x0f},
    /* space and a few marks */
    {0x00,0x00,0x00}, {0x04,0x04,0x04}, {0x00,0x0a,0x00}, {0x10,0x08,0x00},
    {0x00,0x10,0x00},
};

static void px(int x, int y, int c) { api->pixel(x, y, c); }

static void tiny(int x, int y, char ch, int c)
{
    int g;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') g = ch - 'A';
    else if (ch >= '0' && ch <= '9') g = ch - '0' + 26;
    else if (ch == ' ') g = 36;
    else if (ch == '-') g = 37;
    else if (ch == ':') g = 38;
    else if (ch == ',') g = 39;
    else if (ch == '.') g = 40;
    else return;
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 5; row++)
            if (glyph_35[g][col] & (1 << row))
                px(x + col, y + row, c);
}

static void tiny_str(int x, int y, const char *s, int c)
{
    while (*s) { tiny(x, y, *s++, c); x += 4; }
}

static void tiny_num(int x, int y, int v, int c)
{
    char t[8];
    int i = 0, n = 0;
    unsigned u = (unsigned)v;
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    while (i) t[n++] = t[--i];
    t[n] = 0;
    tiny_str(x, y, t, c);
}

/* ---- brush state ---------------------------------------------------- */

static int mx = 320, my = 240;         /* the brush position             */
static int have_mouse, mbtn, prev_btn;
static int tool;                       /* 0 pen 1 line 2 box 3 fill 4 circle */
static int colour = 15;
static int brush = 2;
static int ax, ay;                     /* first corner, when set         */
static int anchor;
static const char *toolname[5] = { "PEN", "LINE", "BOX", "FILLBOX", "CIRCLE" };

static const u8 pal[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const char *palname[16] = {
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "grey",
    "silver", "lblue", "lgreen", "lcyan", "lred", "lmagenta", "yellow",
    "white"
};

/* ---- shapes --------------------------------------------------------- */

static void hline(int x0, int x1, int y, int c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) px(x, y, c);
}
static void vline(int x, int y0, int y1, int c)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) px(x, y, c);
}

static void line(int x0, int y0, int x1, int y1, int c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err + err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void box(int x0, int y0, int x1, int y1, int c, int fill)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (fill) {
        for (int y = y0; y <= y1; y++) hline(x0, x1, y, c);
    } else {
        hline(x0, x1, y0, c);
        hline(x0, x1, y1, c);
        vline(x0, y0, y1, c);
        vline(x1, y0, y1, c);
    }
}

static int sq_root(unsigned n)            /* the circle radius, integer */
{
    unsigned t = 0;
    if (n > 1u << 24) return 4900;
    while ((t + 1) * (t + 1) <= n) t++;
    return (int)t;
}

static void circle(int cx, int cy, int r, int c)
{
    int x = -r, y = 0, err = 2 - 2 * r;
    if (r < 1) { px(cx, cy, c); return; }
    do {
        px(cx - x, cy + y, c); px(cx - y, cy - x, c);
        px(cx + x, cy - y, c); px(cx + y, cy + x, c);
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x)  err += ++x * 2 + 1;
    } while (x < 0);
}

/* ---- reading pixels back (VGA planar only) -------------------------- */

/* one pixel back out of the planar VGA screen: select the read plane
 * (graphics controller index 4) and test this pixel's bit, four times */
static u8 readpx(int x, int y)
{
    volatile u8 *v = (volatile u8 *)(0xA0000 + y * 80 + (x >> 3));
    u8 bit = (u8)(0x80 >> (x & 7));
    u8 c = 0;
    if (!is_planar) return 0;
    for (int p = 0; p < 4; p++) {
        unsigned got;
        __asm__ __volatile__(
            "movb $4, %%al\n\t"
            "movw $0x3CE, %%dx\n\t"
            "outb %%al, %%dx\n\t"
            "movb %%cl, %%al\n\t"
            "movw $0x3CF, %%dx\n\t"
            "outb %%al, %%dx\n\t"
            "movzbl %2, %0"
            : "=r"(got)
            : "c"((unsigned)p), "m"(*v)
            : "eax", "edx");
        if (got & bit) c |= (u8)(1u << p);
    }
    return c;
}

/* ---- save and load, run length encoded ------------------------------ */

static u8 *sbuf;                        /* heap: the encoded picture    */
static int sn, sfull;
#define SBUF_SZ 16384

static void putb(u8 b)
{
    if (sn >= SBUF_SZ) { sfull = 1; return; }
    sbuf[sn++] = b;
}

static void save_pic(void)
{
    if (!is_planar) {
        tiny_str(8, H - 12, "SAVE NEEDS THE VGA SCREEN", 12);
        return;
    }
    sn = 0;
    sfull = 0;
    putb('P'); putb('D'); putb('1');
    putb((u8)(W >> 8)); putb((u8)W);
    putb((u8)(H >> 8)); putb((u8)H);
    for (int y = 0; y < H; y++) {
        int x = 0;
        while (x < W) {
            u8 c = readpx(x, y);
            int run = 1;
            while (x + run < W && run < 255 && readpx(x + run, y) == c)
                run++;
            putb(c);
            putb((u8)run);
            x += run;
        }
    }
    if (sfull) {
        tiny_str(8, H - 12, "PICTURE TOO BUSY TO SAVE", 12);
        return;
    }
    if (api->fs_write("DRAW.PIC", (const char *)sbuf, sn) >= 0) {
        tiny_str(8, H - 12, "SAVED DRAW.PIC", 10);
    } else {
        tiny_str(8, H - 12, "SAVE FAILED", 12);
    }
}

static void load_pic(void)
{
    int n = api->fs_read("DRAW.PIC", (char *)sbuf, SBUF_SZ - 1);
    if (n < 7 || sbuf[0] != 'P' || sbuf[1] != 'D' || sbuf[2] != '1') {
        tiny_str(8, H - 12, "NO DRAW.PIC", 12);
        return;
    }
    int w = (sbuf[3] << 8) | sbuf[4];
    int h = (sbuf[5] << 8) | sbuf[6];
    if (w != W || h != H) {
        tiny_str(8, H - 12, "WRONG SIZE", 12);
        return;
    }
    int i = 7, x = 0, y = 0;
    api->fill_rect(0, 0, W, H, 0);
    while (i + 1 < n && y < H) {
        u8 c = sbuf[i++];
        u8 run = sbuf[i++];
        while (run-- && x < W) px(x++, y, c);
        if (x >= W) { x = 0; y++; }
    }
    tiny_str(8, H - 12, "LOADED", 10);
}

/* ---- the status line, in pixels -------------------------------------- */

static void status(void)
{
    api->fill_rect(0, H - 18, W, 18, 0);
    tiny_str(6, H - 12, toolname[tool], 11);
    for (int i = 0; i < 16; i++)
        api->fill_rect(90 + i * 10, H - 14, 8, 8, pal[i]);
    api->fill_rect(89 + colour * 10, H - 15, 10, 10, 15);
    api->fill_rect(91 + colour * 10, H - 13, 6, 6, pal[colour]);
    tiny_str(270, H - 12, palname[colour], 15);
    tiny_num(340, H - 12, mx, 7);
    tiny_num(390, H - 12, my, 7);
    if (anchor) tiny_str(430, H - 12, "FROM", 14);
    tiny_str(470, H - 12, "1-5 TOOL C/X COL B BRUSH S SAVE O LOAD", 8);
}

/* ---- tools on a click ------------------------------------------------ */

static void paint_at(void)
{
    switch (tool) {
    case 0: for(int yy=-brush;yy<=brush;yy++) for(int xx=-brush;xx<=brush;xx++) if(xx*xx+yy*yy<=brush*brush) px(mx+xx,my+yy,colour); break;
    case 3: box(ax, ay, mx, my, colour, 1); break;
    }
}

static void click(void)
{
    switch (tool) {
    case 0:
        px(mx, my, colour);
        break;
    case 3:
        if (!anchor) { ax = mx; ay = my; anchor = 1; }
        else { box(ax, ay, mx, my, colour, 1); anchor = 0; }
        break;
    default:
        if (!anchor) { ax = mx; ay = my; anchor = 1; }
        else {
            if (tool == 1) line(ax, ay, mx, my, colour);
            else if (tool == 2) box(ax, ay, mx, my, colour, 0);
            else if (tool == 4) {
                int dx = mx - ax, dy = my - ay;
                circle((ax + mx) / 2, (ay + my) / 2,
                       (dx * dx + dy * dy) > 4 ?
                       sq_root((unsigned)((dx * dx + dy * dy) / 4)) : 2,
                       colour);
            }
            anchor = 0;
        }
        break;
    }
    status();
}

/* ---- main loop ------------------------------------------------------- */

int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    sbuf = (u8 *)api->malloc(SBUF_SZ);
    if (!sbuf) {
        api->puts("draw: not enough memory\n");
        return 1;
    }
    if (!api->gfx_mode(1)) {
        api->puts("draw: this machine has no drawing screen\n");
        return 1;
    }
    api->gfx_size(&W, &H);
    is_planar = (*(volatile unsigned char *)(unsigned long)0x449 == 0x12);
    api->fill_rect(0, 0, W, H, 0);
    api->fill_rect(0, 0, W, 24, 8);
    tiny_str(8, 9, "PICO DRAW - ESC LEAVES, F1 FOR HELP", 0);
    status();

    int running = 1;
    int lmx = -1, lmy = -1;             /* the pointer's last resting spot */
    while (running) {
        int ox = mx, oy = my;
        int tx = mx, ty = my, tb = 0;
        have_mouse = api->mouse_px(&tx, &ty, &tb);
        if (have_mouse && (tx != lmx || ty != lmy)) {
            mx = tx;                    /* the pointer moved: it wins */
            my = ty;
            lmx = tx;
            lmy = ty;
        }
        mbtn = tb;
        if (!have_mouse) lmx = lmy = -1;
        {
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx >= W) mx = W - 1;
            if (my >= H) my = H - 1;
        }
        if (ox != mx || oy != my) {
            if (tool == 0 && have_mouse && (mbtn & 1)) paint_at();
            if (tool == 0 && have_mouse && (mbtn & 2)) { int old=colour; colour=0; paint_at(); colour=old; }
            status();
        }
        if (have_mouse && (mbtn & 1) && !(prev_btn & 1)) click();
        if (have_mouse && (mbtn & 1) && tool == 0) paint_at();
        prev_btn = mbtn;

        /* poll() hands us the key itself: it reads one if there is one */
        int k = api->poll();
        if (!k) { api->sleep(10); continue; }
        int step = (k >= 'A' && k <= 'Z') ? 8 : 1;
        switch (k) {
        case KEY_ESC: running = 0; break;
        case 0x81: my -= step; break;            /* up    */
        case 0x82: my += step; break;            /* down  */
        case 0x83: mx -= step; break;            /* left  */
        case 0x84: mx += step; break;            /* right */
        case ' ': click(); break;
        case '1': case '2': case '3': case '4': case '5':
            tool = k - '1'; anchor = 0; break;
        case 'c': case 'C': colour = (colour + 1) & 15; break;
        case 'x': case 'X': colour = (colour + 15) & 15; break;
        case 'b': case 'B': brush = brush < 8 ? brush + 1 : 1; break;
        case '0': colour = 0; break;
        case 'n': case 'N': api->fill_rect(0, 32, W, H-50, 0); anchor=0; break;
        case 's': case 'S': save_pic(); break;
        case 'o': case 'O': load_pic(); break;
        case KEY_F1: {
            api->fill_rect(0, 0, W, H, 0);
            api->fill_rect(0, 0, W, 24, 8);
            tiny_str(8, 9, "PICO DRAW HELP - ANY KEY", 0);
            int y = 40;
            tiny_str(40, y, "MOUSE PAINTS, RIGHT BUTTON ERASES", 15); y += 12;
            tiny_str(40, y, "ARROWS WALK, SHIFT-ARROWS RUN", 15); y += 12;
            tiny_str(40, y, "SPACE USES THE TOOL HERE", 15); y += 12;
            tiny_str(40, y, "1 PEN   2 LINE  3 BOX", 15); y += 12;
            tiny_str(40, y, "4 FILLED BOX   5 CIRCLE", 15); y += 12;
            tiny_str(40, y, "TWO-CLICK TOOLS: PICK TWO POINTS", 15); y += 12;
            tiny_str(40, y, "C/X CYCLE COLOUR, 0 BLACK", 15); y += 12;
            tiny_str(40, y, "S SAVE DRAW.PIC, O LOAD IT", 15); y += 12;
            while (!api->poll()) api->sleep(20);
            api->fill_rect(0, 0, W, H, 0);
            status();
            break;
        }
        default: break;
        }
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= W) mx = W - 1;
        if (my >= H) my = H - 1;
        api->sleep(10);
    }

    api->gfx_mode(0);
    api->free(sbuf);
    return 0;
}
