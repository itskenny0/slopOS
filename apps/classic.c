/* menu.c -- PicoOS menu desktop
 *
 * A dark dashboard with a sidebar, cards, an application grid, a files
 * page and a system page. Model-driven: every screen redraws from state.
 *
 * Two things this version does differently from the first one:
 *
 *   1. Region repaints. Moving the cursor used to repaint the whole
 *      screen -- a full clear plus thousands of per-pixel calls -- which
 *      is the flicker you see on a VGA machine. Now only the changed
 *      region repaints: a sidebar move redraws two sidebar rows, a grid
 *      move redraws two cards, and the clock redraws the top bar alone.
 *
 *   2. Keyboard focus. Tab (or Left/Right at the edges) moves between the
 *      sidebar and the main area. Arrows move inside the focused area and
 *      Enter activates: sidebar items switch pages, app icons launch, and
 *      files open. The old code sent every Enter to the sidebar, so an
 *      app could never be started from the keyboard, and the selection
 *      highlight was painted over and invisible.
 */
#include "picoapp.h"
#include "menu_icons.h"

#define KEY_ESC 27
#define TOP_H 42
#define SIDE_W 154
#define FOOT_H 24

/* These are the hand-tuned palette entries in console.c. Low-nibble VGA
 * colours remain readable when the machine can only provide 16 colours. */
#define BG       33
#define SIDE     17
#define CARD     39
#define CARD2    33
#define ACCENT   49
#define N_CYAN   51
#define N_GREEN  34
#define ORANGE   44
#define TEXT     63
#define MUTED    55
#define BLACK    0

#define PAGE_HOME 0
#define PAGE_APPS 1
#define PAGE_FILES 2
#define PAGE_SYSTEM 3

#define FOCUS_SIDE 0
#define FOCUS_MAIN 1

/* repaint regions, or-ed into `dirty` */
#define D_TOP  0x01
#define D_SIDE 0x02
#define D_MAIN 0x04
#define D_FOOT 0x08
#define D_ALL  (D_TOP | D_SIDE | D_MAIN | D_FOOT)

static int W, H;
static int page = PAGE_HOME;
static int focus = FOCUS_SIDE;
static int side_cur;
static int app_sel, file_sel, quick_sel;
static int dirty = D_ALL;
static int running = 1;
static int mouse_x, mouse_y, mouse_btn, old_btn;
static int have_mouse;
static int last_sec = -1;
static int has_doom;               /* doom.pico installed? (adds a card) */

static const char *side_labels[6] = {
    "Dashboard", "Applications", "Files", "System", "Terminal", "Power"
};
static const u8 *side_icons[6] = {
    menu_icon_computer, menu_icon_folder, menu_icon_file,
    menu_icon_settings, menu_icon_window, menu_icon_power
};

static const u8 glyph[41][3] = {
    {0x1e,0x05,0x1e},{0x1f,0x15,0x0a},{0x0e,0x11,0x11},{0x1f,0x11,0x0e},
    {0x1f,0x15,0x11},{0x1f,0x05,0x01},{0x0e,0x11,0x1d},{0x1f,0x04,0x1f},
    {0x11,0x1f,0x11},{0x08,0x10,0x0f},{0x1f,0x04,0x1b},{0x1f,0x10,0x10},
    {0x1f,0x02,0x1f},{0x1f,0x01,0x1e},{0x0e,0x11,0x0e},{0x1f,0x05,0x02},
    {0x06,0x09,0x16},{0x1f,0x05,0x1a},{0x12,0x15,0x09},{0x01,0x1f,0x01},
    {0x1f,0x10,0x1f},{0x0f,0x10,0x0f},{0x1f,0x08,0x1f},{0x1b,0x04,0x1b},
    {0x03,0x1c,0x03},{0x19,0x15,0x13},{0x1f,0x11,0x1f},{0x12,0x1f,0x10},
    {0x19,0x15,0x12},{0x11,0x15,0x1f},{0x07,0x04,0x1f},{0x17,0x15,0x09},
    {0x1e,0x15,0x1d},{0x01,0x19,0x07},{0x1f,0x15,0x1f},{0x17,0x15,0x0f},
    {0x00,0x00,0x00},{0x04,0x04,0x04},{0x00,0x0a,0x00},{0x10,0x08,0x00},
    {0x00,0x10,0x00}
};

static void fill(int x, int y, int w, int h, int colour)
{
    if (w > 0 && h > 0) api->fill_rect(x, y, w, h, colour);
}

static void glyph_put(int x, int y, char ch, int colour, int scale)
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
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (glyph[g][c] & (1 << r))
                for (int yy = 0; yy < scale; yy++)
                    for (int xx = 0; xx < scale; xx++)
                        api->pixel(x + c * scale + xx,
                                   y + r * scale + yy, colour);
}

static void text(int x, int y, const char *s, int colour, int scale)
{
    while (*s) { glyph_put(x, y, *s++, colour, scale); x += 4 * scale; }
}

static void icon(int x, int y, const u8 *art)
{
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 24; c++) {
            u8 v = art[r * 24 + c];
            if (v != 255) api->pixel(x + c, y + r, v);
        }
}

static void rule(int x, int y, int w, int colour)
{
    fill(x, y, w, 1, colour);
}

static void card(int x, int y, int w, int h)
{
    fill(x + 3, y + 3, w, h, BLACK);
    fill(x, y, w, h, CARD);
    fill(x, y, w, 2, ACCENT);
}

/* a 2-pixel outline: the selection marker on cards */
static void frame(int x, int y, int w, int h, int colour)
{
    fill(x, y, w, 2, colour);
    fill(x, y + h - 2, w, 2, colour);
    fill(x, y, 2, h, colour);
    fill(x + w - 2, y, 2, h, colour);
}

static void number_text(int x, int y, int value, int colour, int scale)
{
    char b[12]; int n = 0;
    if (!value) b[n++] = '0';
    while (value && n < 11) { b[n++] = (char)('0' + value % 10); value /= 10; }
    for (int i = n - 1; i >= 0; i--) {
        glyph_put(x, y, b[i], colour, scale);
        x += 4 * scale;
    }
}

static int same(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The desktop itself is infrastructure, not an app button. Hide menu.pico
 * from the visible list so clicking Applications can never recursively
 * launch another copy of the desktop. */
static int visible_program(int wanted, char *out, u32 *size)
{
    char name[20]; u32 bytes; int raw = 0, visible = 0;
    while (api->programs(raw, name, &bytes)) {
        if (!same(name, "menu.pico") && !same(name, "classic.pico")) {
            if (visible == wanted) {
                int i = 0;
                while (name[i] && i < 19) { out[i] = name[i]; i++; }
                out[i] = 0;
                if (size) *size = bytes;
                return 1;
            }
            visible++;
        }
        raw++;
    }
    return 0;
}

static int count_programs(void)
{
    char name[20]; u32 size; int i = 0;
    while (visible_program(i, name, &size)) i++;
    return i;
}

static int count_files(void)
{
    char name[20]; u32 size; int i = 0;
    while (api->files(i, name, &size)) i++;
    return i;
}

static void run_program_index(int index)
{
    char name[20]; u32 size;
    if (!visible_program(index, name, &size)) return;
    api->gfx_mode(0);
    api->clear();
    api->setcolor(TEXT & 15, 1);
    api->puts("Starting "); api->puts(name); api->puts("...\n");
    api->run(name, 0, 0);
    api->gfx_mode(1);
    dirty = D_ALL;
}

static void run_named(const char *wanted)
{
    char name[20]; u32 size; int i = 0;
    while (visible_program(i, name, &size)) {
        if (same(name, wanted)) { run_program_index(i); return; }
        i++;
    }
}

static int have_program(const char *wanted)
{
    char name[20]; u32 size; int i = 0;
    while (visible_program(i, name, &size)) {
        if (same(name, wanted)) return 1;
        i++;
    }
    return 0;
}

/* ------------------------------ sections ------------------------------ */

static void topbar(void)
{
    int hh, mm, ss;
    char clock[9];
    fill(0, 0, W, TOP_H, SIDE);
    fill(SIDE_W, 0, 2, H, ACCENT);
    text(22, 13, "PicoOS", TEXT, 2);
    text(96, 15, "MENU", N_CYAN, 1);
    rule(SIDE_W + 24, TOP_H - 1, W - SIDE_W - 48, CARD);
    api->rtc(&hh, &mm, &ss);
    last_sec = ss;
    clock[0] = (char)('0' + hh / 10); clock[1] = (char)('0' + hh % 10);
    clock[2] = ':'; clock[3] = (char)('0' + mm / 10);
    clock[4] = (char)('0' + mm % 10); clock[5] = ':';
    clock[6] = (char)('0' + ss / 10); clock[7] = (char)('0' + ss % 10);
    clock[8] = 0;
    text(W - 84, 15, clock, MUTED, 1);
}

/* one sidebar row: the cursor box when focused here, otherwise a quiet
 * marker on the active page so you never lose where you are */
static void side_item(int i)
{
    int y = 82 + i * 45;
    int hot = (i == side_cur && focus == FOCUS_SIDE);
    int here = (i == page);
    fill(10, y - 5, SIDE_W - 20, 36, hot ? CARD : SIDE);
    fill(10, y - 5, 3, 36, hot ? N_CYAN : (here ? ACCENT : SIDE));
    icon(22, y, side_icons[i]);
    text(55, y + 8, side_labels[i],
         (hot || here) ? TEXT : MUTED, 1);
}

static void sidebar(void)
{
    fill(0, TOP_H, SIDE_W, H - TOP_H, SIDE);
    text(20, 63, "WORKSPACE", MUTED, 1);
    for (int i = 0; i < 6; i++) side_item(i);
    rule(20, H - 48, SIDE_W - 40, CARD);
    text(20, H - 34, "PicoOS v2", N_CYAN, 1);
}

static void footer(void)
{
    fill(SIDE_W + 2, H - FOOT_H, W - SIDE_W - 2, FOOT_H, SIDE);
    if (have_mouse)
        text(SIDE_W + 24, H - 16, "MOUSE READY", N_GREEN, 1);
    else
        text(SIDE_W + 24, H - 16, "KEYBOARD READY", N_GREEN, 1);
    text(W - 190, H - 16, "TAB SWITCH ESC BACK", MUTED, 1);
}

/* main-area geometry shared by painters and hit tests */
static int main_x0(void) { return SIDE_W + 2; }
static int main_y0(void) { return TOP_H; }
static int main_w(void)  { return W - SIDE_W - 2; }
static int main_h(void)  { return H - TOP_H - FOOT_H; }

static void main_bg(void)
{
    fill(main_x0(), main_y0(), main_w(), main_h(), BG);
}

static void metric_card(int x, int y, int w, const char *label,
                       const char *value, int colour, const u8 *art)
{
    card(x, y, w, 74);
    icon(x + 14, y + 12, art);
    text(x + 50, y + 16, label, MUTED, 1);
    text(x + 50, y + 39, value, colour, 1);
}

/* quick cards: Draw, Apps, and DOOM when it is installed */
static int quick_count(void) { return 2 + (has_doom ? 1 : 0); }

static void quick_geom(int i, int *x, int *y, int *w)
{
    int nq = quick_count();
    int mx = SIDE_W + 28;
    int total = W - mx - 28;
    int gap = 10;
    int cw = (total - (nq - 1) * gap) / nq;
    *x = mx + i * (cw + gap);
    *y = 362;
    *w = cw;
}

static void quick_card(int i)
{
    int x, y, w;
    quick_geom(i, &x, &y, &w);
    /* clear the slot first: the frame of a deselected card must go too */
    fill(x - 2, y - 2, w + 8, 68, BG);
    card(x, y, w, 62);
    if (i == 0) {
        icon(x + 12, y + 19, menu_icon_image);
        text(x + 45, y + 22, "Draw", TEXT, 2);
    } else if (i == 1) {
        icon(x + 12, y + 19, menu_icon_folder);
        text(x + 45, y + 22, "Apps", TEXT, 2);
    } else {
        icon(x + 12, y + 19, menu_icon_globe);
        text(x + 45, y + 22, "DOOM", LRED, 2);
    }
    if (i == quick_sel)
        frame(x, y, w, 62, focus == FOCUS_MAIN ? N_CYAN : MUTED);
}

static void quick_activate(int i)
{
    if (i == 0) run_named("draw.pico");
    else if (i == 1) { page = PAGE_APPS; app_sel = 0; dirty |= D_SIDE | D_MAIN; }
    else run_named("doom.pico");
}

static void dashboard(void)
{
    char cpu[64], net[40];
    int got, nprog;
    int mx = SIDE_W + 28;
    main_bg();
    text(mx, 72, "Good day, Pico user", TEXT, 2);
    text(mx, 96, "Your system is ready.", MUTED, 1);
    nprog = count_programs();
    number_text(mx, 128, nprog, N_CYAN, 2);
    text(mx + 28, 132, "programs installed", MUTED, 1);

    got = api->fs_read("cpuinfo.txt", cpu, sizeof(cpu) - 1);
    if (got > 0) {
        cpu[got] = 0;
        for (int i = 0; i < got; i++) if (cpu[i] == '\n') { cpu[i] = 0; break; }
    } else {
        cpu[0] = 'C'; cpu[1] = 'P'; cpu[2] = 'U'; cpu[3] = 0;
    }
    api->net_status(net, sizeof(net));
    metric_card(mx, 164, 190, "PROCESSOR", cpu, N_CYAN, menu_icon_computer);
    metric_card(mx + 200, 164, 190, "MEMORY", "READY", N_GREEN, menu_icon_settings);
    metric_card(mx, 248, 390, "NETWORK", net[0] ? "ONLINE" : "OFFLINE",
                net[0] ? N_GREEN : ORANGE, menu_icon_network);

    text(mx, 342, "QUICK ACCESS", MUTED, 1);
    for (int i = 0; i < quick_count(); i++) quick_card(i);
}

/* ------------------------------ apps grid ------------------------------ */

static int app_cols(void) { return W >= 800 ? 3 : 2; }

static void app_geom(int n, int *x, int *y, int *w)
{
    int cols = app_cols();
    int gap = 14;
    int cw = (W - SIDE_W - 56 - (cols - 1) * gap) / cols;
    *x = SIDE_W + 28 + (n % cols) * (cw + gap);
    *y = 132 + (n / cols) * 56;
    *w = cw;
}

static void app_card(int n)
{
    char name[20]; u32 size;
    int x, y, w;
    if (!visible_program(n, name, &size)) return;
    app_geom(n, &x, &y, &w);
    fill(x - 2, y - 2, w + 8, 54, BG);
    card(x, y, w, 48);
    {
        const u8 *art = menu_icon_folder;
        int sub = MUTED;
        const char *subtxt = "launch";
        if (same(name, "doom.pico")) {
            art = menu_icon_image; sub = LRED; subtxt = "shareware E1";
        } else if (same(name, "draw.pico")) {
            art = menu_icon_image;
        } else if (same(name, "web.pico")) {
            art = menu_icon_globe;
        }
        icon(x + 10, y + 10, art);
        text(x + 44, y + 13, name, TEXT, 1);
        text(x + 44, y + 31, subtxt, sub, 1);
    }
    if (n == app_sel)
        frame(x, y, w, 48, focus == FOCUS_MAIN ? N_CYAN : MUTED);
}

static void applications(void)
{
    int n = count_programs();
    main_bg();
    text(SIDE_W + 28, 72, "Applications", TEXT, 2);
    text(SIDE_W + 28, 98, "Programs available on this system", MUTED, 1);
    for (int i = 0; i < n; i++) app_card(i);
    if (!n) text(SIDE_W + 28, 140, "No applications found", ORANGE, 1);
}

/* ------------------------------ files ------------------------------ */

static int file_shown(void)
{
    int n = count_files();
    return n > 10 ? 10 : n;
}

static void file_row(int n)
{
    char name[20]; u32 size;
    int fw = W - SIDE_W - 56;
    int y = 166 + n * 25;
    if (!api->files(n, name, &size)) return;
    if (n == file_sel && focus == FOCUS_MAIN)
        fill(SIDE_W + 28, y, fw, 22, ACCENT);
    else
        fill(SIDE_W + 28, y, fw, 22, n & 1 ? CARD2 : CARD);
    if (n == file_sel && focus == FOCUS_SIDE)
        fill(SIDE_W + 28, y, 3, 22, MUTED);
    icon(SIDE_W + 42, y - 1, menu_icon_file);
    text(SIDE_W + 76, y + 6, name, TEXT, 1);
    number_text(SIDE_W + fw - 68, y + 6, (int)size, MUTED, 1);
}

static void files_page(void)
{
    int fw = W - SIDE_W - 56;
    main_bg();
    text(SIDE_W + 28, 72, "Files", TEXT, 2);
    text(SIDE_W + 28, 98, "Temporary RAM drive", MUTED, 1);
    card(SIDE_W + 28, 126, fw, 32);
    text(SIDE_W + 44, 137, "NAME", MUTED, 1);
    text(SIDE_W + fw - 80, 137, "SIZE", MUTED, 1);
    for (int i = 0; i < file_shown(); i++) file_row(i);
    if (!file_shown()) text(SIDE_W + 44, 180, "No files", MUTED, 1);
}

static int is_pico(const char *name)
{
    int n = 0;
    while (name[n]) n++;
    return n > 5 && name[n - 5] == '.' && name[n - 4] == 'p' &&
           name[n - 3] == 'i' && name[n - 2] == 'c' && name[n - 1] == 'o';
}

/* Enter on a file: programs run, text shows */
static void file_activate(int n)
{
    char name[20]; u32 size;
    if (!api->files(n, name, &size)) return;
    if (is_pico(name)) {
        run_named(name);
        return;
    }
    api->gfx_mode(0);
    api->clear();
    api->setcolor(WHITE, BLACK);
    api->puts(name);
    api->puts("\n\n");
    api->setcolor(LGREY, BLACK);
    {
        /* stream it out in chunks: files can exceed one buffer */
        static char buf[2048];
        u32 off = 0;
        /* fs_read has no offset, so read whole small files, and the head
         * of big ones -- a viewer, not an editor */
        int got = api->fs_read(name, buf, sizeof(buf) - 1);
        if (got > 0) {
            buf[got] = 0;
            api->puts(buf);
            if ((u32)got == sizeof(buf) - 1) api->puts("\n[...]\n");
        } else {
            api->puts("(empty or unreadable)\n");
        }
        (void)off;
    }
    api->puts("\nPress a key.\n");
    api->getkey();
    api->gfx_mode(1);
    dirty = D_ALL;
}

/* ------------------------------ system ------------------------------ */

static void system_page(void)
{
    char info[192];
    int got = api->fs_read("cpuinfo.txt", info, sizeof(info) - 1);
    int fw = W - SIDE_W - 56;
    main_bg();
    text(SIDE_W + 28, 72, "System", TEXT, 2);
    text(SIDE_W + 28, 98, "Hardware and operating system", MUTED, 1);
    card(SIDE_W + 28, 132, fw, 150);
    icon(SIDE_W + 52, 158, menu_icon_computer);
    text(SIDE_W + 92, 164, "CPU DRIVER", N_CYAN, 1);
    if (got > 0) {
        char *p = info; int line = 0;
        info[got] = 0;
        while (*p && line < 4) {
            char *q = p;
            while (*q && *q != '\n') q++;
            if (*q) *q++ = 0;
            text(SIDE_W + 52, 210 + line * 17, p, TEXT, 1);
            p = q; line++;
        }
    }
    card(SIDE_W + 28, 300, fw, 128);
    icon(SIDE_W + 52, 320, menu_icon_settings);
    text(SIDE_W + 92, 326, "DRIVERS", N_GREEN, 1);
    text(SIDE_W + 52, 366, "PS2 keyboard active", TEXT, 1);
    text(SIDE_W + 220, 366, have_mouse ? "PS2 pointer active" : "No pointer", TEXT, 1);
    text(SIDE_W + 52, 390, has_doom ? "DOOM installed" : "DOOM not installed",
         has_doom ? N_GREEN : ORANGE, 1);
    text(SIDE_W + 220, 390, "BIOS UEFI compatible", TEXT, 1);
}

/* ------------------------------ paint ------------------------------ */

static void paint_main(void)
{
    if (page == PAGE_HOME) dashboard();
    else if (page == PAGE_APPS) applications();
    else if (page == PAGE_FILES) files_page();
    else system_page();
}

static void paint(void)
{
    if (dirty & D_ALL) {
        if (dirty == D_ALL) fill(0, 0, W, H, BG);
        if (dirty & D_TOP) topbar();
        if (dirty & D_SIDE) sidebar();
        if (dirty & D_MAIN) paint_main();
        if (dirty & D_FOOT) footer();
    }
    dirty = 0;
}

/* redraw one main-area selection (old + new), nothing else */
static void resel_main(int old)
{
    if (page == PAGE_HOME) { quick_card(old); quick_card(quick_sel); }
    else if (page == PAGE_APPS) {
        char name[20]; u32 s;
        if (visible_program(old, name, &s)) app_card(old);
        app_card(app_sel);
    } else if (page == PAGE_FILES) { file_row(old); file_row(file_sel); }
}

static void set_page(int p)
{
    page = p;
    app_sel = file_sel = quick_sel = 0;
    dirty |= D_SIDE | D_MAIN;
}

static void set_focus(int f)
{
    if (focus == f) return;
    focus = f;
    dirty |= D_SIDE | D_MAIN;
}

static void open_shell(void)
{
    api->gfx_mode(0);
    api->shell();
    api->gfx_mode(1);
    dirty = D_ALL;
}

/* ------------------------------ input ------------------------------ */

static void side_activate(int i)
{
    if (i < 4) { set_page(i); side_cur = i; set_focus(FOCUS_MAIN); }
    else if (i == 4) open_shell();
    else api->power(0);
}

static void click(int x, int y)
{
    /* sidebar rows live at y = 77 + i*45 */
    if (x < SIDE_W && y >= 77 && y < 77 + 6 * 45) {
        int i = (y - 77) / 45;
        side_cur = i;
        dirty |= D_SIDE;
        side_activate(i);
        return;
    }
    if (page == PAGE_HOME) {
        for (int i = 0; i < quick_count(); i++) {
            int qx, qy, qw;
            quick_geom(i, &qx, &qy, &qw);
            if (x >= qx && x < qx + qw && y >= qy && y < qy + 62) {
                quick_sel = i;
                set_focus(FOCUS_MAIN);
                quick_activate(i);
                return;
            }
        }
    } else if (page == PAGE_APPS && x >= SIDE_W + 28 && y >= 132) {
        int cols = app_cols();
        int gap = 14;
        int cw = (W - SIDE_W - 56 - (cols - 1) * gap) / cols;
        int col = (x - SIDE_W - 28) / (cw + gap);
        int row = (y - 132) / 56;
        int ox = (x - SIDE_W - 28) % (cw + gap);
        int oy = (y - 132) % 56;
        if (col >= 0 && col < cols && row >= 0 && ox < cw && oy < 48) {
            int idx = row * cols + col;
            char name[20]; u32 s;
            if (visible_program(idx, name, &s)) {
                app_sel = idx;
                set_focus(FOCUS_MAIN);
                run_program_index(idx);
                return;
            }
        }
    } else if (page == PAGE_FILES && x >= SIDE_W + 28 && y >= 166) {
        int row = (y - 166) / 25;
        int oy = (y - 166) % 25;
        if (row >= 0 && row < file_shown() && oy < 22) {
            if (row == file_sel && focus == FOCUS_MAIN) file_activate(row);
            else { file_sel = row; set_focus(FOCUS_MAIN); dirty |= D_MAIN; }
            return;
        }
    }
}

/* arrows inside the main area; returns 1 when the key was used */
static int main_key(int k)
{
    int n, cols, old;
    if (page == PAGE_HOME) {
        n = quick_count();
        if (k == KEY_LEFT) {
            if (quick_sel > 0) { old = quick_sel--; resel_main(old); }
            else set_focus(FOCUS_SIDE);
            return 1;
        }
        if (k == KEY_RIGHT) {
            if (quick_sel + 1 < n) { old = quick_sel++; resel_main(old); }
            return 1;
        }
        if (k == KEY_UP || k == KEY_DOWN) { set_focus(FOCUS_SIDE); return 1; }
        if (k == '\n' || k == '\r') { quick_activate(quick_sel); return 1; }
        return 0;
    }
    if (page == PAGE_APPS) {
        n = count_programs();
        cols = app_cols();
        if (!n) return 0;
        if (k == KEY_LEFT) {
            if (app_sel % cols == 0) set_focus(FOCUS_SIDE);
            else { old = app_sel--; resel_main(old); }
            return 1;
        }
        if (k == KEY_RIGHT) {
            if (app_sel + 1 < n) { old = app_sel++; resel_main(old); }
            return 1;
        }
        if (k == KEY_UP) {
            if (app_sel >= cols) { old = app_sel; app_sel -= cols; resel_main(old); }
            return 1;
        }
        if (k == KEY_DOWN) {
            if (app_sel + cols < n) { old = app_sel; app_sel += cols; resel_main(old); }
            return 1;
        }
        if (k == KEY_HOME) { old = app_sel; app_sel = 0; resel_main(old); return 1; }
        if (k == KEY_END) { old = app_sel; app_sel = n - 1; resel_main(old); return 1; }
        if (k == '\n' || k == '\r') { run_program_index(app_sel); return 1; }
        return 0;
    }
    if (page == PAGE_FILES) {
        n = file_shown();
        if (!n) return 0;
        if (k == KEY_UP) {
            if (file_sel > 0) { old = file_sel--; resel_main(old); }
            return 1;
        }
        if (k == KEY_DOWN) {
            if (file_sel + 1 < n) { old = file_sel++; resel_main(old); }
            return 1;
        }
        if (k == KEY_LEFT) { set_focus(FOCUS_SIDE); return 1; }
        if (k == '\n' || k == '\r') { file_activate(file_sel); return 1; }
        return 0;
    }
    /* system page: nothing to move on, arrows hand back to the sidebar */
    if (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT) {
        set_focus(FOCUS_SIDE);
        return 1;
    }
    return 0;
}

static void key(int k)
{
    int old;
    if (k == KEY_ESC) {
        if (focus == FOCUS_MAIN) { set_focus(FOCUS_SIDE); return; }
        running = 0;
        return;
    }
    if (k == '\t') { set_focus(focus ^ 1); return; }
    if (k >= '1' && k <= '4') {
        side_cur = k - '1';
        if (page != side_cur) set_page(side_cur);
        else dirty |= D_SIDE | D_MAIN;
        return;
    }
    if (focus == FOCUS_SIDE) {
        if (k == KEY_UP && side_cur > 0) {
            old = side_cur--;
            side_item(old); side_item(side_cur);
            return;
        }
        if (k == KEY_DOWN && side_cur < 5) {
            old = side_cur++;
            side_item(old); side_item(side_cur);
            return;
        }
        if (k == KEY_RIGHT) {
            set_focus(FOCUS_MAIN);
            return;
        }
        if (k == '\n' || k == '\r') { side_activate(side_cur); return; }
        return;
    }
    main_key(k);
}

int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!api->gfx_mode(1)) {
        api->puts("PicoOS menu needs a VGA or firmware framebuffer.\n");
        return 1;
    }
    api->gfx_size(&W, &H);
    if (W < 640 || H < 360) {
        api->gfx_mode(0);
        api->puts("PicoOS menu needs at least 640x360 pixels.\n");
        return 1;
    }
    has_doom = have_program("doom.pico");
    api->cursor(0);
    while (running) {
        int buttons = 0;
        have_mouse = api->mouse_px(&mouse_x, &mouse_y, &buttons);
        mouse_btn = buttons;
        if (have_mouse && (mouse_btn & 1) && !(old_btn & 1))
            click(mouse_x, mouse_y);
        old_btn = mouse_btn;
        {
            /* the clock ticks without touching anything else */
            int hh, mm, ss;
            api->rtc(&hh, &mm, &ss);
            if (ss != last_sec) dirty |= D_TOP;
        }
        if (dirty) paint();
        {
            /* drain the queue: one frame handles every waiting key */
            int k, guard = 0;
            while ((k = api->poll()) != 0 && guard++ < 16) {
                key(k);
                if (!running) break;
                if (dirty) paint();
            }
        }
        api->sleep(25);
    }
    api->cursor(1);
    api->gfx_mode(0);
    return 0;
}
