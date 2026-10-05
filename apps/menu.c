/* menu.c -- the PicoOS 2.1 desktop
 *
 * A light desktop built around the star wallpaper: a taskbar with a star
 * Start button and a search box, a Start menu with every program, desktop
 * icons, a clock widget, and real windows you can drag, minimise and close.
 *
 * NO FLICKER. Nothing here draws straight on the screen. Everything is drawn
 * into a memory buffer, and only the rectangle that actually changed is
 * copied to the screen (api->blit_rect). Moving the mouse repaints nothing
 * at all (the kernel draws the pointer); hovering a button repaints that
 * button; scrolling a list repaints that window.
 *
 * The colours are the desktop's own: it loads a private palette, so it does
 * not depend on the system palette and puts it back after every program.
 *
 * Keyboard: Windows/Super key or F1 opens Start, Esc closes things, F4
 * closes the front window. In Start: type to search, arrows + Enter.
 */
#include "picoapp.h"
#include "wallpaper.h"

/* ---- the palette (indices into the 256-colour table loaded below) ---- */
enum {
    P_BLACK = 0, P_SHADOW = 1, P_WALL = 2, P_ACC = 3, P_ACCD = 4,
    P_GREEN = 5, P_RED = 6, P_ORANGE = 7, P_PURPLE = 8, P_CYAN = 9,
    P_TASK = 10, P_TASKH = 11, P_WIN = 12, P_BODY = 13, P_TITLE = 14,
    P_WHITE = 15,
    P_ALT = 32, P_HOVER = 33, P_BORDER = 34, P_TEXT = 35, P_MUTED = 36,
    P_DIM = 37, P_ONDARK = 38, P_TITLEIN = 39, P_ACCS = 40, P_ORANGEL = 41,
    P_TASKB = 42, P_RAMP = 64
};

#define KEY_UP     0x81
#define KEY_DOWN   0x82
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
#define KEY_F1     0x90
#define KEY_F4     0x93
#define KEY_SUPER  0x9A

#define NICON 5
#define TB_H     40
#define TITLE_H  30
#define BTN_X0   222            /* first window button on the taskbar */
#define BTN_W    150

#define W_NONE  0
#define W_FILES 1
#define W_SYS   2
#define W_USB   3
#define W_NOTE  4
#define W_ABOUT 5
#define W_TASK  6
#define W_SETTINGS 7
#define W_CALC 8
#define NWIN    9

typedef struct {
    int type, open, min;
    int x, y, w, h;
    int scroll, sel;
    char file[16];
} win_t;

static win_t wins[NWIN];
static int   order[NWIN];          /* z-order, order[0] is the back */
static int   nord;

static int W, H;
static u8 *bb, *wp;                /* back buffer, rendered wallpaper */
static u8  pal[768];

static int kx0, ky0, kx1, ky1;     /* clip rectangle while painting */
#define NDMG 8
static int dmg[NDMG][4];           /* damage: rectangles that must reach the screen */
static int ndmg;                   /* (x0, y0, x1, y1), several so far-apart changes stay small */

static int running = 1;
static int mx, my, mb, old_mb;
static int have_mouse;

static int start_open, power_open;
static int start_sel;
static char query[24];
static int  qlen;
static int  hover = -1;
static int  last_min = -1;

/* Calculator state: kept inside the desktop shell so Calculator never leaves
 * the graphical desktop. */
static char calc_disp[32] = "0";
static long calc_acc;
static int calc_op;
static int calc_new = 1;

/* Task Manager history.  The graph uses a real bottom-origin 0..100 scale. */
#define GRAPH_N 48
static int ram_hist[GRAPH_N];
static int cpu_hist[GRAPH_N];
static int hist_n, hist_head;
static u32 hist_last_ms, hist_last_ticks;

static int  desk_sel = -1;
static int  last_click_id = -1;
static u32  last_click_t;
static int  drag = -1, drag_dx, drag_dy;
static int  icon_drag = -1, icon_dx, icon_dy;
static int  icon_x[NICON], icon_y[NICON];
static int  context_open, context_x, context_y;
static int  wall_theme = 0;

static char prog[48][20];
static int  nprog;
static int  shown[48];
static int  nshown;

static int build_wallpaper(void);
static void paint_context(void);

/* ---- tiny helpers (no libc here) ---- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void scat(char *d, const char *s) { d += slen(d); scpy(d, s); }
static void num(char *d, int v) { char t[16]; app_itoa(v, t); scat(d, t); }
static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && lc(hay[i + j]) == lc(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* ---- damage: remember what changed, send only that ---- */

static int touches(const int *r, int x0, int y0, int x1, int y1)
{
    return x0 <= r[2] && x1 >= r[0] && y0 <= r[3] && y1 >= r[1];
}

static void damage(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x >= x1 || y >= y1) return;
    for (int i = 0; i < ndmg; i++) {
        int *r = dmg[i];
        if (!touches(r, x, y, x1, y1)) continue;
        if (x < r[0]) r[0] = x;                         /* merge into this one */
        if (y < r[1]) r[1] = y;
        if (x1 > r[2]) r[2] = x1;
        if (y1 > r[3]) r[3] = y1;
        return;
    }
    if (ndmg == NDMG) {                                 /* full: fold into the first */
        int *r = dmg[0];
        if (x < r[0]) r[0] = x;
        if (y < r[1]) r[1] = y;
        if (x1 > r[2]) r[2] = x1;
        if (y1 > r[3]) r[3] = y1;
        return;
    }
    dmg[ndmg][0] = x; dmg[ndmg][1] = y; dmg[ndmg][2] = x1; dmg[ndmg][3] = y1;
    ndmg++;
}
static void damage_all(void) { damage(0, 0, W, H); }

/* ---- drawing into the back buffer, clipped to the dirty rectangle ---- */

static void R(int x, int y, int w, int h, int c)
{
    int x1 = x + w, y1 = y + h;
    if (x < kx0) x = kx0;
    if (y < ky0) y = ky0;
    if (x1 > kx1) x1 = kx1;
    if (y1 > ky1) y1 = ky1;
    if (x >= x1 || y >= y1) return;
    for (; y < y1; y++) {
        u8 *p = bb + y * W + x;
        for (int i = x; i < x1; i++) *p++ = (u8)c;
    }
}

static void T(int x, int y, const char *s, int fg)
{
    if (y + 16 <= ky0 || y >= ky1 || x >= kx1) return;
    for (; *s; s++, x += 8) {
        if (x + 8 <= kx0) continue;
        if (x >= kx1) break;
        const u8 *g = api->glyph_rows((u8)*s);
        for (int r = 0; r < 16; r++) {
            int yy = y + r;
            u8 b = g[r];
            if (!b || yy < ky0 || yy >= ky1) continue;
            u8 *row = bb + yy * W;
            for (int c = 0; c < 8; c++)
                if (b & (0x80 >> c)) {
                    int xx = x + c;
                    if (xx >= kx0 && xx < kx1) row[xx] = (u8)fg;
                }
        }
    }
}
static void TC(int x, int y, int w, const char *s, int fg)
{
    T(x + (w - slen(s) * 8) / 2, y, s, fg);
}
static void TW(int x, int y, int w, const char *s, int fg)   /* clipped to w pixels */
{
    char b[64];
    int n = w / 8, i = 0;
    if (n > 62) n = 62;
    while (s[i] && i < n) { b[i] = s[i]; i++; }
    b[i] = 0;
    T(x, y, b, fg);
}
/* big text: every pixel of the 8x16 glyph becomes a k x k block */
static void TB(int x, int y, const char *s, int fg, int k)
{
    for (; *s; s++, x += 8 * k) {
        const u8 *g = api->glyph_rows((u8)*s);
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c)) R(x + c * k, y + r * k, k, k, fg);
    }
}
static void frame(int x, int y, int w, int h, int c)
{
    R(x, y, w, 1, c); R(x, y + h - 1, w, 1, c);
    R(x, y, 1, h, c); R(x + w - 1, y, 1, h, c);
}
static void card(int x, int y, int w, int h, int c)          /* corners cut */
{
    R(x + 2, y, w - 4, h, c);
    R(x, y + 2, w, h - 4, c);
    R(x + 1, y + 1, w - 2, h - 2, c);
}
static void card_frame(int x, int y, int w, int h, int fill, int line)
{
    card(x, y, w, h, line);
    card(x + 1, y + 1, w - 2, h - 2, fill);
}

/* a five-pointed star: the logo of this desktop */
static const short star_pt[10][2] = {
    {0, -1000}, {225, -309}, {951, -309}, {363, 118}, {588, 809},
    {0, 382}, {-588, 809}, {-363, 118}, {-951, -309}, {-225, -309}
};
static void star(int cx, int cy, int r, int c)
{
    for (int y = -r; y <= r; y++) {
        int xs[10], n = 0;
        int fy = y * 1000;
        for (int i = 0; i < 10; i++) {
            int j = (i + 1) % 10;
            int y0 = star_pt[i][1] * r, y1 = star_pt[j][1] * r;
            int x0 = star_pt[i][0] * r, x1 = star_pt[j][0] * r;
            if ((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy))
                xs[n++] = (x0 + (x1 - x0) * (fy - y0) / (y1 - y0)) / 1000;
        }
        for (int a = 1; a < n; a++) {                       /* sort, n <= 10 */
            int v = xs[a], b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < n; a += 2)
            R(cx + xs[a], cy + y, xs[a + 1] - xs[a] + 1, 1, c);
    }
}

/* ---- icons, drawn from rectangles (no bitmaps to keep in step) ---- */

static void ic_pc(int x, int y)
{
    card(x + 3, y + 5, 34, 24, P_ACCD);
    R(x + 6, y + 8, 28, 18, P_ACCS);
    R(x + 8, y + 10, 10, 3, P_WIN);
    R(x + 17, y + 29, 6, 4, P_ACCD);
    R(x + 10, y + 33, 20, 3, P_ACCD);
}
static void ic_folder(int x, int y)
{
    R(x + 4, y + 7, 15, 6, P_ORANGE);
    card(x + 3, y + 11, 34, 24, P_ORANGE);
    card(x + 3, y + 16, 34, 19, P_ORANGEL);
}
static void ic_usb(int x, int y)
{
    R(x + 19, y + 9, 3, 26, P_ACC);
    for (int k = 0; k < 6; k++) R(x + 20 - k, y + 3 + k, 2 * k + 1, 1, P_ACC);
    R(x + 10, y + 17, 10, 3, P_ACC);  R(x + 7, y + 13, 7, 7, P_ACC);
    R(x + 21, y + 24, 9, 3, P_ACC);   R(x + 28, y + 20, 7, 7, P_ACC);
    R(x + 16, y + 33, 9, 7, P_ACC);
}
static void ic_term(int x, int y)
{
    card(x + 3, y + 5, 34, 30, P_TASK);
    R(x + 3, y + 5, 34, 5, P_ACCD);
    T(x + 8, y + 15, ">_", P_GREEN);
}
static void ic_apps(int x, int y)
{
    card(x + 4, y + 4, 14, 14, P_ACC);    card(x + 22, y + 4, 14, 14, P_GREEN);
    card(x + 4, y + 22, 14, 14, P_ORANGE); card(x + 22, y + 22, 14, 14, P_PURPLE);
}
static void ic_file(int x, int y, int c)
{
    R(x, y, 12, 14, c);
    R(x + 8, y, 4, 4, P_WIN);
    R(x + 2, y + 6, 8, 2, P_WIN);
    R(x + 2, y + 10, 8, 2, P_WIN);
}

/* ---- programs ---- */

static int hidden_prog(const char *n)
{
    return seq(n, "menu.pico") || seq(n, "classic.pico");
}

static void display_name(const char *file, char *out)
{
    int i = 0;
    while (file[i] && file[i] != '.' && i < 18) { out[i] = file[i]; i++; }
    out[i] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

static void load_programs(void)
{
    char n[20]; u32 sz; int raw = 0;
    nprog = 0;
    while (nprog < 48 && api->programs(raw, n, &sz)) {
        raw++;
        if (hidden_prog(n)) continue;
        scpy(prog[nprog], n);
        nprog++;
    }
}

static void filter_programs(void)
{
    nshown = 0;
    for (int i = 0; i < nprog; i++) {
        char d[20];
        display_name(prog[i], d);
        if (contains(d, query)) shown[nshown++] = i;
    }
    if (start_sel >= nshown) start_sel = nshown - 1;
    if (start_sel < 0) start_sel = 0;
}

static int prog_colour(int i)
{
    static const int pc[6] = { P_ACC, P_GREEN, P_ORANGE, P_PURPLE, P_CYAN, P_RED };
    int h = 0;
    for (const char *s = prog[i]; *s; s++) h = h * 31 + *s;
    if (h < 0) h = -h;
    return pc[h % 6];
}

/* ---- geometry shared by painting, hit testing and damage ---- */

static int tb_y(void) { return H - TB_H; }

#define SM_W   380
#define SM_ROW 38
static int sm_h(void) { int h = H - TB_H - 16; return h > 520 ? 520 : h; }
static int sm_x(void) { return 8; }
static int sm_y(void) { return H - TB_H - sm_h() - 8; }
static int sm_rows(void) { return (sm_h() - 56 - 78 - 6) / SM_ROW; }
static int sm_first(void) { int r = sm_rows(); return start_sel >= r ? start_sel - r + 1 : 0; }

static void icon_pos(int i, int *x, int *y)
{
    if (icon_x[i] == 0 && icon_y[i] == 0) {
        *x = 22; *y = 14 + i * 88;
    } else {
        *x = icon_x[i]; *y = icon_y[i];
    }
}
static const char *icon_name[NICON] = { "This PC", "Files", "USB", "Terminal", "Programs" };

/* taskbar buttons shrink when many windows are open, so none are lost */
static int task_btn_w(void)
{
    int n = nord ? nord : 1;
    int w = (W - 150 - BTN_X0) / n - 6;
    if (w > BTN_W) w = BTN_W;
    if (w < 44) w = 44;
    return w;
}
static int task_btn_x(int id)
{
    for (int i = 0; i < nord; i++) if (order[i] == id) return BTN_X0 + i * (task_btn_w() + 6);
    return -1;
}

/* the rectangle a hover highlight occupies, so only it is repainted */
static int hover_rect(int id, int *x, int *y, int *w, int *h)
{
    int sx = sm_x(), sy = sm_y(), by = sm_y() + sm_h() - 56;
    if (id == 100) { *x = 0; *y = tb_y(); *w = 52; *h = TB_H; return 1; }
    if (id == 110) { *x = 60; *y = tb_y(); *w = 154; *h = TB_H; return 1; }
    if (id >= 200 && id < 300) {
        int bx = task_btn_x(id - 200);
        if (bx < 0) return 0;
        *x = bx; *y = tb_y(); *w = task_btn_w(); *h = TB_H; return 1;
    }
    if (id >= 300 && id < 399) {
        int r = id - 300 - sm_first();
        *x = sx + 8; *y = sy + 78 + r * SM_ROW; *w = SM_W - 16; *h = SM_ROW; return 1;
    }
    if (id == 420) { *x = sx + 8; *y = by + 4; *w = 200; *h = 46; return 1; }
    if (id == 400) { *x = sx + SM_W - 54; *y = by + 8; *w = 40; *h = 38; return 1; }
    if (id >= 410 && id <= 412) {
        int pw = 170, ph = 3 * 36 + 12;
        *x = sx + SM_W - pw - 8; *y = by - ph - 6; *w = pw + 6; *h = ph + 6; return 1;
    }
    if (id >= 500 && id < 500 + NICON) {
        int ix, iy;
        icon_pos(id - 500, &ix, &iy);
        *x = ix - 8; *y = iy - 8; *w = 80; *h = 84; return 1;
    }
    if (id >= 1000 && id < 1100) {
        win_t *wn = &wins[id - 1000];
        *x = wn->x + wn->w - 30; *y = wn->y; *w = 30; *h = TITLE_H; return 1;
    }
    if (id >= 1100 && id < 1200) {
        win_t *wn = &wins[id - 1100];
        *x = wn->x + wn->w - 70; *y = wn->y; *w = 30; *h = TITLE_H; return 1;
    }
    return 0;
}

static void damage_hover(int id)
{
    int x, y, w, h;
    if (hover_rect(id, &x, &y, &w, &h)) damage(x, y, w, h);
}
static void damage_start(void) { damage(sm_x(), sm_y(), SM_W + 8, sm_h() + 8); }
static void damage_task(void)  { damage(0, tb_y(), W, TB_H); }
static void damage_win(int id)
{
    win_t *x = &wins[id];
    damage(x->x - 1, x->y - 1, x->w + 8, x->h + 8);
}

/* ---- windows ---- */

static int win_find(int type)
{
    for (int i = 0; i < NWIN; i++) if (wins[i].open && wins[i].type == type) return i;
    return -1;
}

static void bring_front(int id)
{
    int p = -1;
    for (int i = 0; i < nord; i++) if (order[i] == id) p = i;
    if (p < 0) return;
    for (int i = p; i < nord - 1; i++) order[i] = order[i + 1];
    order[nord - 1] = id;
}

static void win_close(int id)
{
    damage_win(id);
    wins[id].open = 0;
    int p = 0;
    for (int i = 0; i < nord; i++) if (order[i] != id) order[p++] = order[i];
    nord = p;
    damage_task();
}

static int win_open(int type, int w, int h, const char *file)
{
    int id = (type == W_NOTE) ? -1 : win_find(type);
    if (id < 0) {
        for (int i = 0; i < NWIN; i++) if (!wins[i].open) { id = i; break; }
        if (id < 0) { id = order[0]; damage_win(id); }
        else order[nord++] = id;
        win_t *x = &wins[id];
        x->type = type; x->open = 1; x->min = 0;
        if (w > W - 40) w = W - 40;
        if (h > H - TB_H - 30) h = H - TB_H - 30;
        x->w = w; x->h = h;
        int n = nord - 1;
        x->x = 130 + n * 34; x->y = 30 + n * 30;
        if (x->x + w > W) x->x = W - w - 10;
        if (x->y + h > H - TB_H) x->y = H - TB_H - h - 6;
        if (x->x < 4) x->x = 4;
        if (x->y < 4) x->y = 4;
        x->scroll = 0; x->sel = 0;
        x->file[0] = 0;
    }
    wins[id].min = 0;
    if (file) scpy(wins[id].file, file);
    bring_front(id);
    if (start_open || power_open) { start_open = power_open = 0; damage_start(); }
    damage_win(id);
    damage_task();
    return id;
}

static const char *win_title(win_t *x)
{
    switch (x->type) {
    case W_FILES: return "Files";
    case W_SYS:   return "This PC";
    case W_USB:   return "USB Devices";
    case W_NOTE:  return x->file[0] ? x->file : "Viewer";
    case W_ABOUT: return "About PicoOS";
    case W_TASK: return "Task Manager";
    case W_SETTINGS: return "Desktop Settings";
    case W_CALC: return "Calculator";
    }
    return "";
}

/* ---- window contents ---- */

static char note_buf[3072];
static int  note_len, note_lines;

static void note_load(win_t *x)
{
    int got = api->fs_read(x->file, note_buf, (int)sizeof(note_buf) - 1);
    if (got < 0) got = 0;
    note_buf[got] = 0;
    note_len = got;
    note_lines = 1;
    for (int i = 0; i < got; i++) if (note_buf[i] == '\n') note_lines++;
}

static int count_files(void)
{
    char n[20]; u32 s; int i = 0;
    while (api->files(i, n, &s)) i++;
    return i;
}

static void fmt_size(u32 s, char *out)
{
    out[0] = 0;
    if (s >= 1024 * 1024) { num(out, (int)(s / (1024 * 1024))); scat(out, " MB"); }
    else if (s >= 1024)   { num(out, (int)(s / 1024)); scat(out, " KB"); }
    else                  { num(out, (int)s); scat(out, " B"); }
}

static void paint_files(win_t *x, int cx, int cy, int cw, int ch)
{
    R(cx, cy, cw, ch, P_BODY);
    R(cx, cy, cw, 26, P_TITLEIN);
    R(cx, cy + 25, cw, 1, P_BORDER);
    T(cx + 34, cy + 5, "Name", P_MUTED);
    T(cx + cw - 90, cy + 5, "Size", P_MUTED);
    int rows = (ch - 26) / 24;
    int total = count_files();
    if (x->scroll > total - rows) x->scroll = total - rows;
    if (x->scroll < 0) x->scroll = 0;
    if (x->sel >= total) x->sel = total - 1;
    if (x->sel < 0) x->sel = 0;
    for (int r = 0; r < rows; r++) {
        int idx = x->scroll + r;
        char n[20]; u32 s;
        if (!api->files(idx, n, &s)) break;
        int ry = cy + 26 + r * 24;
        int sel = idx == x->sel;
        R(cx, ry, cw, 24, sel ? P_ACC : (r & 1 ? P_ALT : P_BODY));
        ic_file(cx + 12, ry + 5, sel ? P_WIN : P_ACC);
        TW(cx + 34, ry + 4, cw - 140, n, sel ? P_WHITE : P_TEXT);
        char z[16]; fmt_size(s, z);
        T(cx + cw - 90, ry + 4, z, sel ? P_WHITE : P_MUTED);
    }
    if (total > rows) {                                     /* scrollbar */
        int bh = (ch - 26) * rows / total;
        if (bh < 16) bh = 16;
        int by = cy + 26 + (ch - 26 - bh) * x->scroll / (total - rows);
        R(cx + cw - 6, cy + 26, 5, ch - 26, P_ALT);
        R(cx + cw - 6, by, 5, bh, P_DIM);
    }
}

static void paint_note(win_t *x, int cx, int cy, int cw, int ch)
{
    R(cx, cy, cw, ch, P_WIN);
    note_load(x);
    int cols = (cw - 24) / 8;
    int rows = (ch - 12) / 16;
    if (cols > 100) cols = 100;
    int line = 0, col = 0;
    int maxs = note_lines - rows;
    if (maxs < 0) maxs = 0;
    if (x->scroll > maxs) x->scroll = maxs;
    if (x->scroll < 0) x->scroll = 0;
    char b[104];
    int bl = 0;
    for (int i = 0; i <= note_len; i++) {
        char c = note_buf[i];
        if (c == '\r') continue;
        if (c == '\n' || c == 0 || col >= cols) {
            b[bl] = 0;
            if (line >= x->scroll && line - x->scroll < rows)
                T(cx + 12, cy + 6 + (line - x->scroll) * 16, b, P_TEXT);
            line++; bl = 0; col = 0;
            if (c == 0) break;
            if (c == '\n') continue;
        }
        if (c < 32) c = ' ';
        b[bl++] = c; col++;
    }
}

static void bar(int x, int y, int w, int pct, int c)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    R(x, y, w, 10, P_ALT);
    R(x, y, w * pct / 100, 10, c);
}

static void paint_sys(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x;
    R(cx, cy, cw, ch, P_BODY);
    char s[96], cpu[128];
    int y = cy + 14;
    ic_pc(cx + 14, y);
    T(cx + 70, y + 2, "PicoOS Pro 2.1", P_TEXT);
    s[0] = 0; num(s, W); scat(s, "x"); num(s, H); scat(s, " screen");
    T(cx + 70, y + 22, s, P_MUTED);
    y += 64;

    int got = api->fs_read("cpuinfo.txt", cpu, 127);
    if (got < 0) got = 0;
    cpu[got] = 0;
    int e = 0;
    while (cpu[e] && cpu[e] != '\n') e++;
    cpu[e] = 0;
    T(cx + 16, y, "Processor", P_DIM);
    TW(cx + 16, y + 18, cw - 32, cpu[0] ? cpu : "unknown", P_TEXT);
    y += 52;

    pico_mem_t m;
    api->meminfo(&m);
    T(cx + 16, y, "Memory", P_DIM);
    s[0] = 0; num(s, (int)(m.used / 1024)); scat(s, " KB of "); num(s, (int)(m.total / 1024)); scat(s, " KB used");
    T(cx + 16, y + 18, s, P_TEXT);
    int pct = m.total ? (int)(m.used / (m.total / 100 + 1)) : 0;
    bar(cx + 16, y + 42, cw - 32, pct, pct > 80 ? P_RED : P_ACC);
    y += 70;

    u32 sec = api->ms() / 1000;
    s[0] = 0;
    num(s, (int)(sec / 3600)); scat(s, "h "); num(s, (int)((sec / 60) % 60)); scat(s, "m ");
    num(s, (int)(sec % 60)); scat(s, "s");
    T(cx + 16, y, "Uptime", P_DIM);
    T(cx + 16, y + 18, s, P_TEXT);
    s[0] = 0; num(s, nprog); scat(s, " programs, "); num(s, count_files()); scat(s, " files");
    T(cx + cw / 2, y + 18, s, P_MUTED);
    y += 50;

    api->net_status(s, (int)sizeof s);
    T(cx + 16, y, "Network", P_DIM);
    TW(cx + 16, y + 18, cw - 32, s[0] ? s : "no network card", P_TEXT);
}

static void paint_usb(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x;
    R(cx, cy, cw, ch, P_BODY);
    char s[96];
    int y = cy + 14;
    T(cx + 16, y, "Controllers and devices", P_DIM);
    api->usb_info(s, (int)sizeof s);
    TW(cx + 16, y + 20, cw - 32, s, P_TEXT);
    y += 52;

    int pm = api->usb_mouse_present();
    R(cx + 16, y, 10, 10, pm ? P_GREEN : P_DIM);
    s[0] = 0;
    if (pm) { num(s, pm); scat(s, pm == 1 ? " USB mouse connected" : " USB mice connected"); }
    else scat(s, have_mouse ? "PS/2 mouse (no USB mouse)" : "no mouse detected");
    T(cx + 34, y - 3, s, P_TEXT);
    s[0] = 0; scat(s, "pointer "); num(s, mx); scat(s, ", "); num(s, my);
    T(cx + 34, y + 17, s, P_MUTED);
    y += 48;

    T(cx + 16, y, "Attached", P_DIM);
    y += 22;
    char l[80];
    int any = 0;
    for (int i = 0; i < 8 && api->usb_line(i, l, (int)sizeof l); i++) {
        any = 1;
        R(cx + 16, y + 2, 8, 8, P_ACC);
        TW(cx + 32, y - 3, cw - 48, l, P_TEXT);
        y += 22;
    }
    if (!any) T(cx + 16, y, "nothing found on any USB port", P_MUTED);
}

static void paint_about(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x;
    R(cx, cy, cw, ch, P_BODY);
    star(cx + cw / 2, cy + 62, 40, P_ACC);
    star(cx + cw / 2 - 74, cy + 36, 11, P_ACCS);
    star(cx + cw / 2 + 74, cy + 36, 11, P_ACCS);
    TC(cx, cy + 118, cw, "PicoOS Pro 2.1", P_TEXT);
    TC(cx, cy + 142, cw, "built from scratch", P_MUTED);
    TC(cx, cy + 170, cw, "USB 1.1 / 2.0 / 3.x mouse + keyboard", P_MUTED);
    TC(cx, cy + 192, cw, "free and open source (MIT licence)", P_MUTED);
}



static long calc_value(void)
{
    long v = 0; int i = 0;
    int neg = 0;
    if (calc_disp[0] == '-') { neg = 1; i = 1; }
    while (calc_disp[i] >= '0' && calc_disp[i] <= '9') { v = v * 10 + (calc_disp[i] - '0'); i++; }
    return neg ? -v : v;
}
static void calc_set(long v)
{
    char t[24]; app_itoa((int)v, t); scpy(calc_disp, t);
    calc_new = 0;
}
static void calc_push_digit(int d)
{
    int n = slen(calc_disp);
    if (calc_new || seq(calc_disp, "0")) { calc_disp[0] = (char)('0' + d); calc_disp[1] = 0; calc_new = 0; return; }
    if (n < 20) { calc_disp[n] = (char)('0' + d); calc_disp[n + 1] = 0; }
}
static void calc_apply(void)
{
    long b = calc_value(), r = b;
    switch (calc_op) {
    case 1: r = calc_acc + b; break;
    case 2: r = calc_acc - b; break;
    case 3: r = calc_acc * b; break;
    case 4: r = b ? calc_acc / b : 0; break;
    }
    calc_set(r); calc_acc = r; calc_op = 0;
}
static void calc_operator(int op)
{
    if (calc_op) calc_apply(); else calc_acc = calc_value();
    calc_op = op; calc_new = 1;
}
static void calc_button(int id)
{
    if (id >= 0 && id <= 9) calc_push_digit(id);
    else if (id == 10) { calc_disp[0] = '0'; calc_disp[1] = 0; calc_acc = 0; calc_op = 0; calc_new = 1; }
    else if (id == 11) { int n = slen(calc_disp); if (n > 1) calc_disp[n-1] = 0; else { calc_disp[0]='0'; calc_disp[1]=0; } }
    else if (id >= 20 && id <= 23) calc_operator(id - 19);
    else if (id == 24) { if (calc_op) calc_apply(); else calc_set(calc_value()); calc_new = 1; }
}
static int calc_hit(int x, int y, int cx, int cy, int cw, int ch)
{
    int bw = 52, bh = 40, gap = 7, sx = cx + 18, sy = cy + 74;
    static const int ids[20] = { 7,8,9,20, 4,5,6,21, 1,2,3,22, 0,11,10,23, 24,24,24,24 };
    for (int r=0;r<5;r++) for(int c=0;c<4;c++) {
        int bx=sx+c*(bw+gap), by=sy+r*(bh+gap), ww=bw;
        if (r==4 && c==0) ww=bw*3+gap*2;
        if (r==4 && c>0) continue;
        if (inside(x,y,bx,by,ww,bh)) return ids[r*4+c];
    }
    (void)cw; (void)ch; return -1;
}
static void paint_calc(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x; R(cx,cy,cw,ch,P_BODY);
    R(cx+14,cy+14,cw-28,42,P_TITLEIN); card_frame(cx+14,cy+14,cw-28,42,P_TITLEIN,P_BORDER);
    TW(cx+24,cy+24,cw-48,calc_disp,P_TEXT);
    int bw=52,bh=40,gap=7,sx=cx+18,sy=cy+74;
    static const char *lab[25]={"0","1","2","3","4","5","6","7","8","9","C","DEL","+","-","*","/","="};
    static const int ids[20]={7,8,9,20,4,5,6,21,1,2,3,22,0,11,10,23,24,24,24,24};
    for(int r=0;r<5;r++) for(int c=0;c<4;c++) {
        if(r==4 && c>0) continue;
        int id=ids[r*4+c], bx=sx+c*(bw+gap), by=sy+r*(bh+gap), ww=(r==4&&c==0)?bw*3+gap*2:bw;
        int col=(id>=20||id==24)?P_ACCS:P_WIN;
        card_frame(bx,by,ww,bh,col,P_BORDER);
        const char *z="";
        if(id<=9) z=lab[id]; else if(id==10) z="C"; else if(id==11) z="DEL"; else if(id==20) z="+"; else if(id==21) z="-"; else if(id==22) z="*"; else if(id==23) z="/"; else if(id==24) z="=";
        TC(bx,by+12,ww,z,P_TEXT);
    }
}
static void task_hist_update(void)
{
    u32 now=api->ms(), ticks=api->ticks();
    if (!hist_last_ms) { hist_last_ms=now; hist_last_ticks=ticks; return; }
    if (now-hist_last_ms < 100) return;
    pico_mem_t m; api->meminfo(&m);
    int ram=m.total ? (int)((m.used*100)/m.total) : 0;
    u32 dt=ticks-hist_last_ticks, ms=now-hist_last_ms;
    int cpu=(ms && dt)?(int)((dt*1000)/(ms*10)):0;
    if(cpu>100) cpu=100; if(cpu<0) cpu=0;
    ram_hist[hist_head]=ram; cpu_hist[hist_head]=cpu;
    hist_head=(hist_head+1)%GRAPH_N; if(hist_n<GRAPH_N) hist_n++;
    hist_last_ms=now; hist_last_ticks=ticks;
}
static void graph_line(int x,int y,int w,int h,const int *a,int n,int head,int col)
{
    if(n<2)return;
    int px=x, py=y+h-(a[(head-n+GRAPH_N)%GRAPH_N]*h/100);
    for(int i=1;i<n;i++) { int v=a[(head-n+i+GRAPH_N)%GRAPH_N]; int nx=x+(w-1)*i/(n-1), ny=y+h-(v*h/100); 
        int dx=nx-px,dy=ny-py,steps=dx>dy?dx:dy; if(steps<1)steps=1;
        for(int q=0;q<=steps;q++){int xx=px+dx*q/steps, yy=py+dy*q/steps; R(xx,yy,2,2,col);} px=nx;py=ny; }
}
static void paint_graph(win_t *x,int cx,int cy,int cw,int ch)
{
    (void)x; int gx=cx+16, gy=cy+102, gw=cw-32, gh=100;
    R(gx,gy,gw,gh,P_WIN); frame(gx,gy,gw,gh,P_BORDER);
    for(int i=1;i<5;i++) { int yy=gy+gh*i/5; R(gx+1,yy,gw-2,1,P_ALT); }
    T(gx+4,gy+4,"100%",P_DIM); T(gx+4,gy+gh-18,"0%",P_DIM);
    graph_line(gx+28,gy+2,gw-34,gh-4,ram_hist,hist_n,hist_head,P_ACC);
    graph_line(gx+28,gy+2,gw-34,gh-4,cpu_hist,hist_n,hist_head,P_ORANGE);
    T(gx+gw-125,gy+5,"RAM",P_ACC); T(gx+gw-85,gy+5,"CPU",P_ORANGE);
}

static void paint_taskmgr(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x; task_hist_update(); R(cx,cy,cw,ch,P_BODY);
    char s[80]; pico_mem_t mi; api->meminfo(&mi);
    R(cx,cy,cw,34,P_TASK); T(cx+14,cy+9,"Task Manager",P_ONDARK);
    s[0]=0; scat(s,"RAM "); num(s,(int)(mi.used/1024)); scat(s," / "); num(s,(int)(mi.total/1024)); scat(s," KB");
    T(cx+cw-190,cy+9,s,P_ACCS);
    paint_graph(x,cx,cy,cw,ch);
    T(cx+18,cy+88,"Performance history",P_TEXT);
    T(cx+18,cy+214,"Processes",P_MUTED);
    int rows=(ch-242)/25; pico_task_t t; int count=0;
    for(int i=0;i<rows && api->tasks(i,&t);i++) { int yy=cy+236+i*25; R(cx+14,yy,cw-28,23,(i&1)?P_ALT:P_WIN); char id[16]; id[0]=0; num(id,t.pid); T(cx+22,yy+4,id,P_MUTED); T(cx+58,yy+4,t.name,P_TEXT); T(cx+cw-105,yy+4,t.foreground?"ACTIVE":"IDLE",t.foreground?P_GREEN:P_MUTED); count++; }
    int used=(int)(mi.used/1024), freeb=(int)(mi.free_bytes/1024), kern=(int)(mi.kernel_bytes/1024);
    s[0]=0; scat(s,"Used ");num(s,used);scat(s," KB   Free ");num(s,freeb);scat(s," KB   OS ");num(s,kern);scat(s," KB");
    T(cx+18,cy+ch-22,s,P_DIM);
}

static void paint_settings(win_t *x, int cx, int cy, int cw, int ch)
{
    (void)x;
    R(cx, cy, cw, ch, P_BODY);
    T(cx + 18, cy + 18, "Desktop settings", P_TEXT);
    T(cx + 18, cy + 48, "Wallpaper", P_MUTED);
    static const char *names[3] = { "Pico Blue", "Reverse Blue", "Midnight" };
    for (int i = 0; i < 3; i++) {
        int bx = cx + 18 + i * 112;
        card_frame(bx, cy + 74, 100, 58, i == wall_theme ? P_ACCS : P_WIN, i == wall_theme ? P_ACC : P_BORDER);
        R(bx + 8, cy + 82, 84, 22, P_RAMP + (i == 0 ? 11 : i == 1 ? 4 : 1));
        T(bx + 10, cy + 108, names[i], i == wall_theme ? P_ACC : P_TEXT);
    }
    T(cx + 18, cy + 154, "Desktop", P_MUTED);
    T(cx + 18, cy + 176, "Drag icons to reposition them.", P_TEXT);
    T(cx + 18, cy + 198, "Right-click the background for more actions.", P_TEXT);
    T(cx + 18, cy + 220, "Start button / F1 opens the app launcher.", P_TEXT);
    T(cx + 18, cy + 242, "Files shows the live PicoOS RAM filesystem.", P_TEXT);
    T(cx + 18, cy + 264, "Physical HDD/SSD drivers are not in this kernel yet.", P_ORANGE);
}

static void paint_window(int id, int active)
{
    win_t *x = &wins[id];
    R(x->x + 5, x->y + 5, x->w, x->h, P_SHADOW);
    R(x->x, x->y, x->w, x->h, P_WIN);
    frame(x->x, x->y, x->w, x->h, active ? P_ACC : P_BORDER);
    R(x->x + 1, x->y + 1, x->w - 2, TITLE_H - 1, active ? P_TITLE : P_TITLEIN);
    R(x->x + 1, x->y + TITLE_H - 1, x->w - 2, 1, P_BORDER);
    star(x->x + 17, x->y + 15, 7, active ? P_ACC : P_DIM);
    TW(x->x + 32, x->y + 7, x->w - 110, win_title(x), active ? P_TEXT : P_MUTED);
    R(x->x + x->w - 70, x->y + 1, 30, TITLE_H - 2,
      hover == 1100 + id ? P_HOVER : (active ? P_TITLE : P_TITLEIN));
    R(x->x + x->w - 63, x->y + 18, 14, 2, hover == 1100 + id ? P_ACC : P_MUTED);
    R(x->x + x->w - 30, x->y + 1, 29, TITLE_H - 2,
      hover == 1000 + id ? P_RED : (active ? P_TITLE : P_TITLEIN));
    T(x->x + x->w - 22, x->y + 7, "x", hover == 1000 + id ? P_WHITE : P_TEXT);
    int cx = x->x + 1, cy = x->y + TITLE_H, cw = x->w - 2, ch = x->h - TITLE_H - 1;
    switch (x->type) {
    case W_FILES: paint_files(x, cx, cy, cw, ch); break;
    case W_NOTE:  paint_note(x, cx, cy, cw, ch); break;
    case W_SYS:   paint_sys(x, cx, cy, cw, ch); break;
    case W_USB:   paint_usb(x, cx, cy, cw, ch); break;
    case W_ABOUT: paint_about(x, cx, cy, cw, ch); break;
    case W_TASK: paint_taskmgr(x, cx, cy, cw, ch); break;
    case W_CALC: paint_calc(x, cx, cy, cw, ch); break;
    case W_SETTINGS: paint_settings(x, cx, cy, cw, ch); break;
    }
}

/* ---- desktop: wallpaper, icons, clock widget ---- */

static void paint_wallpaper(void)
{
    for (int y = ky0; y < ky1; y++) {
        u8 *d = bb + y * W + kx0, *s = wp + y * W + kx0;
        for (int i = kx0; i < kx1; i++) *d++ = *s++;
    }
}

static void paint_icons(void)
{
    for (int i = 0; i < NICON; i++) {
        int x, y;
        icon_pos(i, &x, &y);
        if (!(x - 8 < kx1 && x + 72 > kx0 && y - 8 < ky1 && y + 80 > ky0)) continue;
        if (i == desk_sel) { card_frame(x - 6, y - 6, 76, 80, P_ACCS, P_ACC); }
        else if (hover == 500 + i) card(x - 6, y - 6, 76, 80, P_HOVER);
        switch (i) {
        case 0: ic_pc(x + 12, y + 2); break;
        case 1: ic_folder(x + 12, y + 2); break;
        case 2: ic_usb(x + 12, y + 2); break;
        case 3: ic_term(x + 12, y + 2); break;
        default: ic_apps(x + 12, y + 2); break;
        }
        TC(x - 6, y + 50, 76, icon_name[i], P_TEXT);
    }
}

static const char *const mon_name[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *const day_name[7] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };

static void paint_widget(void)
{
    int h, m, s, y, mo, d;
    api->clock(&h, &m, &s, &y, &mo, &d);
    int wx = W - 190, wy = 16, ww = 174, wh = 98;
    if (!(wx < kx1 && wx + ww + 6 > kx0 && wy < ky1 && wy + wh + 6 > ky0)) return;
    R(wx + 4, wy + 4, ww, wh, P_SHADOW);
    card_frame(wx, wy, ww, wh, P_WIN, P_BORDER);
    star(wx + 14, wy + 14, 6, P_ACC);
    char t[8];
    t[0] = (char)('0' + h / 10); t[1] = (char)('0' + h % 10); t[2] = ':';
    t[3] = (char)('0' + m / 10); t[4] = (char)('0' + m % 10); t[5] = 0;
    TB(wx + (ww - 120) / 2, wy + 14, t, P_TEXT, 3);
    int mi = mo >= 1 && mo <= 12 ? mo - 1 : 0;
    static const int tt[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = mo < 3 ? y - 1 : y;
    int dow = (yy + yy / 4 - yy / 100 + yy / 400 + tt[mi] + d) % 7;
    char line[40];
    line[0] = 0;
    scat(line, day_name[dow]); scat(line, " "); num(line, d); scat(line, " "); scat(line, mon_name[mi]);
    TC(wx, wy + 70, ww, line, P_MUTED);
}

/* ---- taskbar ---- */

static void paint_taskbar(void)
{
    int y = tb_y();
    R(0, y, W, TB_H, P_TASK);
    R(0, y, W, 1, P_TASKB);

    /* the star Start button */
    int open = start_open;
    int hv = hover == 100;
    if (open) card(5, y + 5, 42, TB_H - 10, P_ACC);
    else if (hv) card(5, y + 5, 42, TB_H - 10, P_TASKH);
    star(26, y + 21, 12, open ? P_WHITE : P_ACCS);
    star(26, y + 21, 7, open ? P_ACC : P_TASK);              /* hollow centre: an outline star */
    star(26, y + 21, 3, open ? P_WHITE : P_ACCS);

    /* search box */
    card(60, y + 6, 150, TB_H - 12, hover == 110 ? P_TASKH : P_TASKB);
    frame(70, y + 12, 11, 11, P_DIM); frame(71, y + 13, 9, 9, P_DIM);   /* magnifier */
    R(79, y + 22, 2, 2, P_DIM); R(81, y + 24, 2, 2, P_DIM); R(83, y + 26, 2, 2, P_DIM);
    T(94, y + 12, "Search", P_DIM);

    for (int i = 0; i < nord; i++) {
        win_t *x = &wins[order[i]];
        int bw = task_btn_w();
        int bx = BTN_X0 + i * (bw + 6);
        int front = (i == nord - 1) && !x->min;
        int h2 = hover == 200 + order[i];
        card(bx, y + 5, bw, TB_H - 10, front ? P_TASKH : (h2 ? P_TASKB : P_TASK));
        if (front) R(bx + 8, y + TB_H - 7, bw - 16, 2, P_ACC);
        star(bx + 14, y + 20, 6, x->min ? P_DIM : P_ACCS);
        if (bw > 70) TW(bx + 28, y + 12, bw - 34, win_title(x), x->min ? P_DIM : P_ONDARK);
    }

    int hh, mm, ss;
    api->rtc(&hh, &mm, &ss);
    char t[8];
    t[0] = (char)('0' + hh / 10); t[1] = (char)('0' + hh % 10); t[2] = ':';
    t[3] = (char)('0' + mm / 10); t[4] = (char)('0' + mm % 10); t[5] = 0;
    T(W - 60, y + 12, t, P_ONDARK);
    int um = api->usb_mouse_present();
    R(W - 134, y + 12, 9, 15, um ? P_GREEN : P_DIM);               /* a tiny mouse */
    R(W - 130, y + 12, 1, 6, P_TASK);
    T(W - 118, y + 12, um ? "USB" : "---", um ? P_ONDARK : P_DIM);
}

/* ---- start menu ---- */

static void paint_start(void)
{
    int x = sm_x(), y = sm_y(), w = SM_W, h = sm_h();
    R(x + 6, y + 6, w, h, P_SHADOW);
    card_frame(x, y, w, h, P_WIN, P_BORDER);

    card_frame(x + 14, y + 14, w - 28, 32, P_BODY, P_BORDER);
    if (qlen) T(x + 26, y + 22, query, P_TEXT);
    else T(x + 26, y + 22, "Type to search programs", P_DIM);
    R(x + 26 + qlen * 8, y + 22, 2, 16, P_ACC);

    T(x + 18, y + 56, qlen ? "Results" : "All programs", P_DIM);

    int rows = sm_rows();
    int first = sm_first();
    int ly = y + 78;
    for (int r = 0; r < rows && first + r < nshown; r++) {
        int i = shown[first + r];
        int yy = ly + r * SM_ROW;
        int sel = (first + r == start_sel);
        int hv = hover == 300 + first + r;
        if (sel || hv) card(x + 8, yy, w - 16, SM_ROW - 2, sel ? P_ACC : P_HOVER);
        card(x + 18, yy + 5, 26, 26, sel ? P_WHITE : prog_colour(i));
        char d[20];
        display_name(prog[i], d);
        char c[2];
        c[0] = d[0]; c[1] = 0;
        T(x + 27, yy + 10, c, sel ? P_ACC : P_WHITE);
        T(x + 56, yy + 10, d, sel ? P_WHITE : P_TEXT);
    }
    if (!nshown) T(x + 24, ly + 8, "No program matches", P_MUTED);
    if (nshown > rows) {                                    /* scrollbar */
        int th = rows * SM_ROW;
        int bh = th * rows / nshown;
        if (bh < 16) bh = 16;
        int by2 = ly + (th - bh) * first / (nshown - rows);
        R(x + w - 9, ly, 4, th, P_ALT);
        R(x + w - 9, by2, 4, bh, P_DIM);
    }

    int by = y + h - 56;
    R(x + 2, by, w - 4, 54, P_BODY);
    R(x + 2, by, w - 4, 1, P_BORDER);
    if (hover == 420) card(x + 8, by + 4, 200, 46, P_HOVER);
    card(x + 16, by + 10, 34, 34, P_ACC);
    star(x + 33, by + 28, 12, P_WHITE);
    T(x + 60, by + 19, "Pico User", P_TEXT);
    int pbx = x + w - 54, pby = by + 8;
    card(pbx, pby, 40, 38, power_open ? P_RED : (hover == 400 ? P_HOVER : P_ALT));
    {   /* a power symbol: a ring open at the top with a bar through the gap */
        int cx = pbx + 13, cy = pby + 11;
        int ink = power_open ? P_WHITE : P_TEXT;
        frame(cx, cy, 14, 14, ink);
        frame(cx + 1, cy + 1, 12, 12, ink);
        R(cx + 4, cy - 1, 6, 4, power_open ? P_RED : (hover == 400 ? P_HOVER : P_ALT));
        R(cx + 6, cy - 3, 2, 9, ink);
    }

    if (power_open) {
        int pw = 170, ph = 3 * 36 + 12;
        int px = x + w - pw - 8, py = by - ph - 6;
        R(px + 4, py + 4, pw, ph, P_SHADOW);
        card_frame(px, py, pw, ph, P_WIN, P_BORDER);
        static const char *pl[3] = { "Shut down", "Restart", "Terminal" };
        for (int i = 0; i < 3; i++) {
            int iy = py + 6 + i * 36;
            int hv = hover == 410 + i;
            if (hv) card(px + 6, iy, pw - 12, 32, i == 0 ? P_RED : P_ACC);
            T(px + 22, iy + 8, pl[i], hv ? P_WHITE : P_TEXT);
        }
    }
}

/* ---- repaint: draw the damaged part into the buffer, send it ---- */

static void paint_all(void)
{
    paint_wallpaper();
    paint_widget();
    paint_icons();
    for (int i = 0; i < nord; i++) {
        win_t *x = &wins[order[i]];
        if (x->open && !x->min) paint_window(order[i], i == nord - 1);
    }
    paint_taskbar();
    if (start_open) paint_start();
    if (context_open) paint_context();
}

static void flush(void)
{
    if (!ndmg) return;
    int n = ndmg;
    ndmg = 0;                                   /* painting must not add damage */
    for (int i = 0; i < n; i++) {
        kx0 = dmg[i][0]; ky0 = dmg[i][1]; kx1 = dmg[i][2]; ky1 = dmg[i][3];
        paint_all();
        api->blit_rect(bb, W, H, kx0, ky0, kx1 - kx0, ky1 - ky0);
    }
    if (have_mouse) api->cursor_at(mx, my, 1);                 /* pointer back on top */
}

/* ---- wallpaper and palette ---- */

static void set_col(int i, u32 rgb)
{
    pal[i * 3] = (u8)(rgb >> 16);
    pal[i * 3 + 1] = (u8)(rgb >> 8);
    pal[i * 3 + 2] = (u8)rgb;
}

static void load_palette(void)
{
    for (int i = 0; i < 768; i++) pal[i] = 0;
    set_col(P_BLACK, 0x000000);  set_col(P_WHITE, 0xFFFFFF);
    set_col(P_SHADOW, 0xC3D2E4); set_col(P_WALL, 0xEAF3FF);
    set_col(P_ACC, 0x1E90F0);    set_col(P_ACCD, 0x0F6FC8);
    set_col(P_GREEN, 0x2EBD6B);  set_col(P_RED, 0xE5484D);
    set_col(P_ORANGE, 0xF59E2B); set_col(P_PURPLE, 0x9B7BF0);
    set_col(P_CYAN, 0x2DC7D6);
    set_col(P_TASK, 0x10294A);   set_col(P_TASKH, 0x23497A);
    set_col(P_TASKB, 0x1B3A63);
    set_col(P_WIN, 0xFFFFFF);    set_col(P_BODY, 0xF5F9FE);
    set_col(P_TITLE, 0xDCEAFB);  set_col(P_TITLEIN, 0xF0F4F9);
    set_col(P_ALT, 0xEBF1F9);    set_col(P_HOVER, 0xD7E7FB);
    set_col(P_BORDER, 0xB3C5DB); set_col(P_TEXT, 0x14304F);
    set_col(P_MUTED, 0x58708C);  set_col(P_DIM, 0x8DA1B9);
    set_col(P_ONDARK, 0xF4F8FD); set_col(P_ACCS, 0xBFDDFB);
    set_col(P_ORANGEL, 0xFFC95E);
    for (int i = 0; i < 16; i++) {                      /* background -> blue */
        u32 a = 0xEAF3FF, b = 0x1298FD;
        u32 r = (((a >> 16) & 255) * (15 - i) + ((b >> 16) & 255) * i) / 15;
        u32 g = (((a >> 8) & 255) * (15 - i) + ((b >> 8) & 255) * i) / 15;
        u32 bl = ((a & 255) * (15 - i) + (b & 255) * i) / 15;
        set_col(P_RAMP + i, (r << 16) | (g << 8) | bl);
    }
    api->setpal(pal);
}

static int build_wallpaper(void)
{
    int n = WP_W * WP_H;
    u8 *lv = (u8 *)api->malloc((size_t)n);
    int area = H - TB_H;
    int gw = W * 46 / 100;
    int gh = gw * WP_H / WP_W;
    if (gh > area * 82 / 100) { gh = area * 82 / 100; gw = gh * WP_W / WP_H; }
    int *xt = (int *)api->malloc((size_t)gw * sizeof(int));
    for (int i = 0; i < W * H; i++) wp[i] = P_WALL;
    if (!lv || !xt) { if (lv) api->free(lv); if (xt) api->free(xt); return 0; }

    int k = 0;
    for (u32 i = 0; i + 1 < sizeof wp_rle; i += 2) {
        u8 v = wp_rle[i], c = wp_rle[i + 1];
        while (c-- && k < n) lv[k++] = v;
    }
    for (int x = 0; x < gw; x++) xt[x] = (int)(((long)x * (WP_W - 1) * 256) / gw);

    int gx = (W - gw) / 2, gy = (area - gh) / 2;
    for (int y = 0; y < gh; y++) {
        int fy = (int)(((long)y * (WP_H - 1) * 256) / gh);
        int y0 = fy >> 8, wy = fy & 255;
        int y1 = y0 + 1 < WP_H ? y0 + 1 : y0;
        u8 *dst = wp + (gy + y) * W + gx;
        for (int x = 0; x < gw; x++) {
            int x0 = xt[x] >> 8, wx = xt[x] & 255;
            int x1 = x0 + 1 < WP_W ? x0 + 1 : x0;
            int a = lv[y0 * WP_W + x0], b = lv[y0 * WP_W + x1];
            int c = lv[y1 * WP_W + x0], d = lv[y1 * WP_W + x1];
            int top = a * (256 - wx) + b * wx;
            int bot = c * (256 - wx) + d * wx;
            int v = (top * (256 - wy) + bot * wy) >> 12;      /* 0..240 */
            int l = (v + 8) >> 4;
            if (l > 15) l = 15;
            dst[x] = (u8)(P_RAMP + l);
        }
    }
    api->free(lv);
    api->free(xt);
    return 1;
}

/* ---- actions ---- */

static void reenter_graphics(void)
{
    api->gfx_mode(1);
    api->gfx_size(&W, &H);
    load_palette();
    damage_all();
    old_mb = 1;                      /* ignore the click that is still held */
}

static void run_fullscreen(const char *name)
{
    start_open = power_open = 0;
    api->cursor_at(0, 0, 0);
    api->gfx_mode(0);
    api->clear();
    api->setcolor(15, 1);
    api->puts("Starting "); api->puts(name); api->puts("...\n");
    api->run(name, 0, 0);
    reenter_graphics();
}

static void open_terminal(void)
{
    start_open = power_open = 0;
    api->cursor_at(0, 0, 0);
    api->gfx_mode(0);
    api->shell();
    reenter_graphics();
}

static void open_start(void)
{
    start_open = !start_open;
    power_open = 0;
    qlen = 0; query[0] = 0;
    start_sel = 0;
    if (start_open) { load_programs(); filter_programs(); }
    damage_start();
    damage(0, tb_y(), 214, TB_H);
}

static void launch_shown(int k)
{
    if (k < 0 || k >= nshown) return;
    char name[20];
    scpy(name, prog[shown[k]]);
    if (contains(name, "calculator")) { win_open(W_CALC, 300, 350, 0); return; }
    if (contains(name, "taskmgr")) { win_open(W_TASK, 520, 390, 0); return; }
    run_fullscreen(name);
}


static void icon_defaults(void)
{
    for (int i = 0; i < NICON; i++) {
        icon_x[i] = 22;
        icon_y[i] = 14 + i * 88;
    }
}

static void apply_wall_theme(int theme)
{
    wall_theme = theme;
    /* A lightweight wallpaper switch: keep the bundled star wallpaper for
       theme 0, and recolour its pixels for the other built-in themes. */
    build_wallpaper();
    if (theme == 1) {
        for (int y = 0; y < H - TB_H; y++)
            for (int x = 0; x < W; x++) {
                u8 *p = &wp[y * W + x];
                if (*p >= P_RAMP && *p < P_RAMP + 16) {
                    int v = *p - P_RAMP;
                    *p = (u8)(P_RAMP + (15 - v));
                }
            }
    } else if (theme == 2) {
        for (int y = 0; y < H - TB_H; y++)
            for (int x = 0; x < W; x++) {
                u8 *p = &wp[y * W + x];
                if (*p >= P_RAMP && *p < P_RAMP + 16) {
                    int v = *p - P_RAMP;
                    *p = (u8)(P_RAMP + (v / 2));
                } else if (*p == P_WALL) *p = P_TASK;
            }
    }
    damage_all();
}

static void paint_context(void)
{
    if (!context_open) return;
    int w = 190, h = 5 * 30 + 10;
    int x = context_x, y = context_y;
    if (x + w > W) x = W - w - 4;
    if (y + h > H - TB_H) y = H - TB_H - h - 4;
    R(x + 4, y + 4, w, h, P_SHADOW);
    card_frame(x, y, w, h, P_WIN, P_BORDER);
    static const char *items[5] = {
        "New text file", "Change wallpaper", "Arrange icons",
        "Refresh desktop", "Desktop settings"
    };
    for (int i = 0; i < 5; i++) {
        int yy = y + 5 + i * 30;
        int hv = hover == 1200 + i;
        if (hv) card(x + 5, yy, w - 10, 28, P_HOVER);
        T(x + 16, yy + 6, items[i], hv ? P_ACC : P_TEXT);
    }
}

static void context_open_at(int x, int y)
{
    context_open = 1;
    context_x = x; context_y = y;
    start_open = power_open = 0;
    damage_all();
}

static void context_click(int x, int y)
{
    int w = 190, h = 160;
    int px = context_x, py = context_y;
    if (px + w > W) px = W - w - 4;
    if (py + h > H - TB_H) py = H - TB_H - h - 4;
    for (int i = 0; i < 5; i++) {
        if (!inside(x, y, px + 5, py + 5 + i * 30, w - 10, 28)) continue;
        if (i == 0) {
            api->fs_write("New Text File.txt", "PicoOS Pro 2.1\n", 15);
        } else if (i == 1) {
            wall_theme = (wall_theme + 1) % 3;
            apply_wall_theme(wall_theme);
        } else if (i == 2) {
            icon_defaults();
            damage_all();
        } else if (i == 3) {
            damage_all();
        } else if (i == 4) {
            /* Keep this deliberately lightweight: the settings entry is
               the discoverable place for desktop preferences. */
            win_open(W_SETTINGS, 430, 330, 0);
        }
        context_open = 0;
        damage_all();
        return;
    }
    context_open = 0;
    damage_all();
}

static void desk_open(int i)
{
    switch (i) {
    case 0: win_open(W_SYS, 440, 400, 0); break;
    case 1: win_open(W_FILES, 480, 380, 0); break;
    case 2: win_open(W_USB, 520, 360, 0); break;
    case 3: open_terminal(); break;
    case 4: open_start(); break;
    }
}

static void power_action(int i)
{
    if (i == 0) api->power(0);
    else if (i == 1) api->power(1);
    else open_terminal();
}

/* ---- hit testing ---- */

static int hit_hover(int x, int y)
{
    if (context_open) {
        int px = context_x, py = context_y, w = 190;
        if (px + w > W) px = W - w - 4;
        if (py + 160 > H - TB_H) py = H - TB_H - 160 - 4;
        for (int i = 0; i < 5; i++)
            if (inside(x, y, px + 5, py + 5 + i * 30, w - 10, 28)) return 1200 + i;
        return 1199;
    }
    if (start_open) {
        int sx = sm_x(), sy = sm_y(), h = sm_h();
        int by = sy + h - 56;
        if (power_open) {
            int pw = 170, ph = 3 * 36 + 12;
            int px = sx + SM_W - pw - 8, py = by - ph - 6;
            for (int i = 0; i < 3; i++)
                if (inside(x, y, px + 6, py + 6 + i * 36, pw - 12, 32)) return 410 + i;
        }
        if (inside(x, y, sx + SM_W - 54, by + 8, 40, 38)) return 400;
        if (inside(x, y, sx + 8, by + 4, 200, 46)) return 420;
        int rows = sm_rows(), first = sm_first();
        for (int r = 0; r < rows && first + r < nshown; r++)
            if (inside(x, y, sx + 8, sy + 78 + r * SM_ROW, SM_W - 16, SM_ROW - 2))
                return 300 + first + r;
        if (inside(x, y, sx, sy, SM_W, h)) return 399;
    }
    if (y >= tb_y()) {
        if (x < 52) return 100;
        if (x >= 60 && x < 210) return 110;
        for (int i = 0; i < nord; i++) {
            int bw = task_btn_w();
            int bx = BTN_X0 + i * (bw + 6);
            if (inside(x, y, bx, tb_y() + 5, bw, TB_H - 10)) return 200 + order[i];
        }
        return 150;
    }
    for (int i = nord - 1; i >= 0; i--) {
        win_t *w = &wins[order[i]];
        if (!w->open || w->min) continue;
        if (inside(x, y, w->x, w->y, w->w, w->h)) {
            if (inside(x, y, w->x + w->w - 30, w->y, 30, TITLE_H)) return 1000 + order[i];
            if (inside(x, y, w->x + w->w - 70, w->y, 30, TITLE_H)) return 1100 + order[i];
            return 600 + order[i];
        }
    }
    for (int i = 0; i < NICON; i++) {
        int ix, iy;
        icon_pos(i, &ix, &iy);
        if (inside(x, y, ix - 6, iy - 6, 76, 80)) return 500 + i;
    }
    return -1;
}

static void select_icon(int i)
{
    if (desk_sel == i) return;
    if (desk_sel >= 0) damage_hover(500 + desk_sel);
    desk_sel = i;
    if (i >= 0) damage_hover(500 + i);
}

static void click(int x, int y)
{
    if (context_open) { context_click(x, y); return; }
    int hv = hit_hover(x, y);

    if (start_open) {
        if (hv == 400) { power_open = !power_open; damage_start(); return; }
        if (hv == 420) { win_open(W_ABOUT, 360, 250, 0); return; }
        if (hv >= 410 && hv <= 412) { power_action(hv - 410); return; }
        if (hv >= 300 && hv < 399) { launch_shown(hv - 300); return; }
        if (hv == 399) { if (power_open) { power_open = 0; damage_start(); } return; }
        if (hv == 100 || hv == 110) { open_start(); return; }
        start_open = power_open = 0;                 /* a click anywhere else */
        damage_start();
        damage(0, tb_y(), 214, TB_H);
    }

    if (hv == 100 || hv == 110) { open_start(); return; }
    if (hv >= 200 && hv < 300) {
        int id = hv - 200;
        if (nord && order[nord - 1] == id && !wins[id].min) wins[id].min = 1;
        else { wins[id].min = 0; bring_front(id); }
        damage_win(id);
        damage_task();
        return;
    }
    if (hv >= 1000 && hv < 1100) { win_close(hv - 1000); return; }
    if (hv >= 1100 && hv < 1200) { int id = hv - 1100; wins[id].min = 1; damage_win(id); damage_task(); return; }
    if (hv >= 600 && hv < 700) {
        int id = hv - 600;
        win_t *w = &wins[id];
        if (order[nord - 1] != id) { bring_front(id); damage_win(id); damage_task(); }
        if (y < w->y + TITLE_H) {
            if (x >= w->x + w->w - 70 && x < w->x + w->w - 40) {
                w->min = 1; damage_win(id); damage_task(); return;
            }
            drag = id; drag_dx = x - w->x; drag_dy = y - w->y;
            return;
        }
        if (w->type == W_CALC) {
            int idb = calc_hit(x, y, w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1);
            if (idb >= 0) { calc_button(idb); damage_win(id); }
            return;
        }
        if (w->type == W_FILES) {
            int row = (y - (w->y + TITLE_H + 26)) / 24;
            if (y >= w->y + TITLE_H + 26 && row >= 0) {
                int idx = w->scroll + row;
                char n[20]; u32 s;
                if (api->files(idx, n, &s)) {
                    u32 now = api->ticks();
                    if (idx == w->sel && last_click_id == 7000 + id && now - last_click_t < 45)
                        win_open(W_NOTE, 520, 360, n);
                    if (idx != w->sel) { w->sel = idx; damage_win(id); }
                    last_click_id = 7000 + id;
                    last_click_t = now;
                }
            }
        }
        return;
    }
    if (hv >= 500 && hv < 500 + NICON) {
        int i = hv - 500;
        u32 now = api->ticks();
        if (desk_sel == i && last_click_id == hv && now - last_click_t < 45) desk_open(i);
        int ix, iy; icon_pos(i, &ix, &iy);
        icon_drag = i; icon_dx = x - ix; icon_dy = y - iy;
        select_icon(i);
        last_click_id = hv;
        last_click_t = now;
        return;
    }
    select_icon(-1);
}

/* ---- keyboard ---- */

static void key(int k)
{
    if (context_open) {
        if (k == 27) { context_open = 0; damage_all(); }
        return;
    }
    if (k == KEY_SUPER || k == KEY_F1) { open_start(); return; }

    if (start_open) {
        if (k == 27) {
            if (power_open) { power_open = 0; damage_start(); }
            else open_start();
        } else if (k == KEY_UP)   { if (start_sel > 0) { start_sel--; damage_start(); } }
        else if (k == KEY_DOWN)   { if (start_sel < nshown - 1) { start_sel++; damage_start(); } }
        else if (k == '\n' || k == '\r') launch_shown(start_sel);
        else if (k == '\b') { if (qlen) query[--qlen] = 0; filter_programs(); damage_start(); }
        else if (k >= 32 && k < 127 && qlen < 20) {
            query[qlen++] = (char)k; query[qlen] = 0;
            filter_programs();
            damage_start();
        }
        return;
    }

    if ((k == 27 || k == KEY_F4) && nord) { win_close(order[nord - 1]); return; }
    if (nord) {
        int id = order[nord - 1];
        win_t *w = &wins[id];
        if (w->min) return;
        if (k == KEY_UP) {
            if (w->type == W_FILES) { if (w->sel > 0) w->sel--; if (w->sel < w->scroll) w->scroll = w->sel; }
            else if (w->scroll > 0) w->scroll--;
            damage_win(id);
        }
        if (k == KEY_DOWN) {
            if (w->type == W_FILES) {
                w->sel++;
                int rows = (w->h - TITLE_H - 27) / 24;
                if (w->sel >= w->scroll + rows) w->scroll = w->sel - rows + 1;
            } else w->scroll++;
            damage_win(id);
        }
        if (k == KEY_PGUP) { w->scroll -= 8; damage_win(id); }
        if (k == KEY_PGDN) { w->scroll += 8; damage_win(id); }
        if (w->type == W_CALC) {
            if (k >= '0' && k <= '9') { calc_push_digit(k - '0'); damage_win(id); }
            else if (k == '+') { calc_operator(1); damage_win(id); }
            else if (k == '-') { calc_operator(2); damage_win(id); }
            else if (k == '*') { calc_operator(3); damage_win(id); }
            else if (k == '/') { calc_operator(4); damage_win(id); }
            else if (k == '\n' || k == '\r' || k == '=') { calc_button(24); damage_win(id); }
            else if (k == 'c' || k == 'C') { calc_button(10); damage_win(id); }
            else if (k == '\b') { calc_button(11); damage_win(id); }
        }
        if (w->type == W_FILES && (k == '\n' || k == '\r')) {
            char n[20]; u32 s;
            if (api->files(w->sel, n, &s)) win_open(W_NOTE, 520, 360, n);
        }
    }
}

/* ---- main ---- */

int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!api->gfx_mode(1)) {
        api->puts("PicoOS desktop needs a VGA or firmware framebuffer.\n");
        return 1;
    }
    api->gfx_size(&W, &H);
    if (!api->gfx256() || W < 640 || H < 400) {
        /* 16-colour fallback: the classic menu works on anything */
        api->gfx_mode(0);
        return api->run("classic.pico", 0, 0);
    }
    bb = (u8 *)api->malloc((size_t)W * (size_t)H);
    wp = (u8 *)api->malloc((size_t)W * (size_t)H);
    if (!bb || !wp) {
        api->gfx_mode(0);
        return api->run("classic.pico", 0, 0);
    }
    api->cursor(0);
    load_palette();
    build_wallpaper();
    load_programs();
    icon_defaults();

    mx = W / 2; my = H / 2;
    damage_all();
    flush();

    int tick = 0;
    while (running) {
        int b = 0, px = mx, py = my;
        have_mouse = api->mouse_px(&px, &py, &b);
        if (have_mouse) {
            int moved = (px != mx || py != my);
            mx = px; my = py; mb = b;
            int hv = hit_hover(mx, my);
            if (hv != hover) {
                int old = hover;
                hover = hv;
                damage_hover(old);        /* only the two buttons that change */
                damage_hover(hv);
            }
            if ((mb & 2) && !(old_mb & 2) && !start_open && my < tb_y() && hit_hover(mx, my) == -1)
                context_open_at(mx, my);
            if ((mb & 1) && !(old_mb & 1)) click(mx, my);
            if (!(mb & 1)) { drag = -1; icon_drag = -1; }
            if (drag >= 0 && moved) {
                win_t *w = &wins[drag];
                damage_win(drag);
                w->x = mx - drag_dx;
                w->y = my - drag_dy;
                if (w->y < 0) w->y = 0;
                if (w->y > H - TB_H - TITLE_H) w->y = H - TB_H - TITLE_H;
                if (w->x < -w->w + 80) w->x = -w->w + 80;
                if (w->x > W - 80) w->x = W - 80;
                damage_win(drag);
            } else if (icon_drag >= 0 && moved && !start_open && !nord) {
                int ox = icon_x[icon_drag], oy = icon_y[icon_drag];
                icon_x[icon_drag] = mx - icon_dx;
                icon_y[icon_drag] = my - icon_dy;
                if (icon_x[icon_drag] < 8) icon_x[icon_drag] = 8;
                if (icon_y[icon_drag] < 8) icon_y[icon_drag] = 8;
                if (icon_x[icon_drag] > W - 82) icon_x[icon_drag] = W - 82;
                if (icon_y[icon_drag] > H - TB_H - 82) icon_y[icon_drag] = H - TB_H - 82;
                damage(ox - 8, oy - 8, 84, 84);
                damage(icon_x[icon_drag] - 8, icon_y[icon_drag] - 8, 84, 84);
            }
            int wh = api->mouse_wheel();
            if (wh) {
                int target = (hover >= 600 && hover < 700) ? hover - 600 : -1;
                if (start_open && hover >= 300 && hover < 400) {
                    start_sel -= wh;
                    if (start_sel < 0) start_sel = 0;
                    if (start_sel > nshown - 1) start_sel = nshown - 1;
                    damage_start();
                } else if (target >= 0) {
                    wins[target].scroll -= wh * 3;
                    damage_win(target);
                }
            }
            old_mb = mb;
        }

        {
            int hh, mm, ss;
            api->rtc(&hh, &mm, &ss);
            if (mm != last_min) {                 /* the clocks tick once a minute */
                last_min = mm;
                damage(W - 70, tb_y(), 70, TB_H);
                damage(W - 190, 16, 180, 104);
            }
            tick++;
        }
        if ((tick % 10) == 0) { int tm=win_find(W_TASK); if(tm>=0 && !wins[tm].min) { task_hist_update(); damage_win(tm); } }

        /* the system and USB windows show live numbers: refresh them now and then */
        if ((tick % 100) == 0) {
            int s = win_find(W_SYS), u = win_find(W_USB);
            if (s >= 0 && !wins[s].min) damage_win(s);
            if (u >= 0 && !wins[u].min) damage_win(u);
        }

        flush();

        {
            int k, guard = 0;
            while ((k = api->poll()) != 0 && guard++ < 16) {
                key(k);
                if (!running) break;
            }
        }
        api->sleep(10);
    }
    api->cursor_at(0, 0, 0);
    api->cursor(1);
    api->gfx_mode(0);
    return 0;
}
