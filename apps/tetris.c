/* tetris.pico -- Tetris for PicoOS
 *
 * Written against nothing but the kernel api table. No libc, no floats, no
 * allocations after startup.
 *
 * The interesting constraint is drawing. On a 486 with VGA text memory a
 * full redraw is cheap, but under UEFI the console renders every character
 * as an 8x16 glyph into a framebuffer, and repainting 200 cells sixty times
 * a second would crawl. So the board keeps a shadow copy of what is
 * currently on screen and only touches the cells that actually changed.
 * Typically that is eight cells per frame: the four the piece left and the
 * four it moved into.
 */
#include "picoapp.h"

#define BW 10           /* board width in cells  */
#define BH 20           /* board height in cells */

/* Where the board sits on screen. Worked out at startup so the playfield
 * ends up centred whether the console is 80x25 text or 128x48 pixels. */
static int OX = 3, OY = 1;

/* Each tetromino is four rotations of a 4x4 bitmap. Bit r*4+c is set when
 * that square of the box is filled. */
static const u16 SHAPES[7][4] = {
    { 0x00F0, 0x4444, 0x00F0, 0x4444 },   /* I */
    { 0x0660, 0x0660, 0x0660, 0x0660 },   /* O */
    { 0x0072, 0x0262, 0x0270, 0x0232 },   /* T */
    { 0x0036, 0x0462, 0x0036, 0x0462 },   /* S */
    { 0x0063, 0x0264, 0x0063, 0x0264 },   /* Z */
    { 0x0071, 0x0226, 0x0470, 0x0322 },   /* J */
    { 0x0074, 0x0622, 0x0170, 0x0223 }    /* L */
};
static const u8 COLORS[7] = { LCYAN, YELLOW, LMAGENTA, LGREEN, LRED, LBLUE, BROWN };

/* 0 = empty, otherwise colour+1 */
static u8 board[BH][BW];
static u8 shown[BH][BW];        /* what the screen currently shows */

static int cur, rot, px, py;    /* the falling piece */
static int nxt;
static int score, lines, level;
static int paused, gameover;
static u32 hiscore;

/* ------------------------------------------------------------ helpers -- */

static int cell(int piece, int r, int x, int y)
{
    if (x < 0 || y < 0 || x > 3 || y > 3) return 0;
    return (SHAPES[piece][r & 3] >> (y * 4 + x)) & 1;
}

/* does the piece fit with its box corner at (bx,by)? */
static int fits(int piece, int r, int bx, int by)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell(piece, r, x, y)) continue;
            int cx = bx + x, cy = by + y;
            if (cx < 0 || cx >= BW || cy >= BH) return 0;
            if (cy >= 0 && board[cy][cx]) return 0;
        }
    return 1;
}

static void draw_cell(int x, int y, u8 v)
{
    api->gotoxy(OX + x * 2, OY + y);
    if (v) {
        api->setcolor(BLACK, (u8)(v - 1));
        api->puts("  ");
    } else {
        api->setcolor(DGREY, BLACK);
        api->puts(" .");
    }
}

/* paint only what changed since the last frame */
static void flush_board(void)
{
    u8 tmp[BH][BW];
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            tmp[y][x] = board[y][x];

    /* overlay the falling piece */
    if (!gameover) {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                if (!cell(cur, rot, x, y)) continue;
                int cx = px + x, cy = py + y;
                if (cx >= 0 && cx < BW && cy >= 0 && cy < BH)
                    tmp[cy][cx] = (u8)(COLORS[cur] + 1);
            }
    }

    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            if (tmp[y][x] != shown[y][x]) {
                draw_cell(x, y, tmp[y][x]);
                shown[y][x] = tmp[y][x];
            }
}

static void draw_frame(void)
{
    api->setcolor(DGREY, BLACK);
    for (int y = 0; y < BH; y++) {
        api->gotoxy(OX - 1, OY + y);       api->puts("|");
        api->gotoxy(OX + BW * 2, OY + y);  api->puts("|");
    }
    api->gotoxy(OX - 1, OY + BH);
    api->puts("+--------------------+");
}

static void draw_next(void)
{
    int nx = OX + BW * 2 + 6, ny = OY + 2;
    for (int y = 0; y < 4; y++) {
        api->gotoxy(nx, ny + y);
        for (int x = 0; x < 4; x++) {
            if (cell(nxt, 0, x, y)) {
                api->setcolor(BLACK, COLORS[nxt]);
                api->puts("  ");
            } else {
                api->setcolor(BLACK, BLACK);
                api->puts("  ");
            }
        }
    }
}

static void draw_stats(void)
{
    int sx = OX + BW * 2 + 5;
    char buf[16];

    api->setcolor(WHITE, BLACK);
    api->gotoxy(sx, OY);      api->puts("NEXT");

    api->gotoxy(sx, OY + 8);  api->puts("SCORE");
    api->setcolor(LGREEN, BLACK);
    app_itoa(score, buf);
    api->gotoxy(sx, OY + 9);  api->puts("        ");
    api->gotoxy(sx, OY + 9);  api->puts(buf);

    api->setcolor(WHITE, BLACK);
    api->gotoxy(sx, OY + 11); api->puts("LINES");
    api->setcolor(LCYAN, BLACK);
    app_itoa(lines, buf);
    api->gotoxy(sx, OY + 12); api->puts("        ");
    api->gotoxy(sx, OY + 12); api->puts(buf);

    api->setcolor(WHITE, BLACK);
    api->gotoxy(sx, OY + 14); api->puts("LEVEL");
    api->setcolor(YELLOW, BLACK);
    app_itoa(level, buf);
    api->gotoxy(sx, OY + 15); api->puts("        ");
    api->gotoxy(sx, OY + 15); api->puts(buf);

    api->setcolor(WHITE, BLACK);
    api->gotoxy(sx, OY + 17); api->puts("BEST");
    api->setcolor(LMAGENTA, BLACK);
    app_itoa((int)hiscore, buf);
    api->gotoxy(sx, OY + 18); api->puts("        ");
    api->gotoxy(sx, OY + 18); api->puts(buf);
}

static void draw_keys(void)
{
    api->setcolor(DGREY, BLACK);
    api->gotoxy(OX - 1, OY + BH + 2);
    api->puts("left/right move   up rotate   down soft drop");
    api->gotoxy(OX - 1, OY + BH + 3);
    api->puts("space hard drop   p pause     q quit");
}

/* ------------------------------------------------------------- rules --- */

static void spawn(void)
{
    cur = nxt;
    nxt = (int)(api->rand() % 7);
    rot = 0;
    px  = BW / 2 - 2;
    py  = -1;
    if (!fits(cur, rot, px, py)) gameover = 1;
    draw_next();
}

static void lock_piece(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell(cur, rot, x, y)) continue;
            int cx = px + x, cy = py + y;
            if (cy >= 0 && cy < BH && cx >= 0 && cx < BW)
                board[cy][cx] = (u8)(COLORS[cur] + 1);
        }
}

static int clear_lines(void)
{
    int cleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BW; x++) if (!board[y][x]) { full = 0; break; }
        if (!full) continue;

        cleared++;
        for (int yy = y; yy > 0; yy--)
            for (int x = 0; x < BW; x++) board[yy][x] = board[yy - 1][x];
        for (int x = 0; x < BW; x++) board[0][x] = 0;
        y++;                                  /* recheck the row that fell */
    }
    return cleared;
}

/* classic scoring: one line is cheap, four at once is worth eight singles */
static const int LINE_SCORE[5] = { 0, 100, 300, 500, 800 };

/* how many 100ths of a second between drops, by level */
static int fall_delay(void)
{
    static const int tbl[11] = { 50, 43, 37, 31, 26, 21, 17, 13, 10, 8, 6 };
    return level < 10 ? tbl[level] : 4;
}

static void rotate_piece(int dir)
{
    int nr = (rot + (dir > 0 ? 1 : 3)) & 3;
    /* wall kicks: straight, then nudged one or two cells sideways */
    static const int kick[5] = { 0, -1, 1, -2, 2 };
    for (int i = 0; i < 5; i++)
        if (fits(cur, nr, px + kick[i], py)) {
            rot = nr;
            px += kick[i];
            return;
        }
}

static void settle(void)
{
    lock_piece();
    int n = clear_lines();
    if (n) {
        lines += n;
        score += LINE_SCORE[n] * (level + 1);
        level = lines / 10;
        if (level > 20) level = 20;
        api->beep(n == 4 ? 880 : 440, 40);
        /* a cleared row changes the whole board, so forget the shadow */
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) shown[y][x] = 0xFF;
        draw_stats();
    }
    spawn();
}

static void load_hiscore(void)
{
    char buf[16];
    int n = api->fs_read("tetris.hi", buf, sizeof buf - 1);
    hiscore = 0;
    if (n > 0) {
        buf[n] = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] < '0' || buf[i] > '9') break;
            hiscore = hiscore * 10 + (u32)(buf[i] - '0');
        }
    }
}

static void save_hiscore(void)
{
    if ((u32)score <= hiscore) return;
    hiscore = (u32)score;
    char buf[16];
    app_itoa(score, buf);
    api->fs_write("tetris.hi", buf, (u32)app_strlen(buf));
}

/* --------------------------------------------------------------- main -- */

int app_main(int argc, char **argv)
{
    if (api->width() < 40 || api->height() < 25) {
        api->puts("tetris: needs a console of at least 40x25\n");
        return 1;
    }

    /* the whole layout is the board (20 cols) plus a 14-column sidebar */
    int need_w = BW * 2 + 16, need_h = BH + 5;
    OX = (api->width()  - need_w) / 2;
    OY = (api->height() - need_h) / 2;
    if (OX < 3) OX = 3;
    if (OY < 1) OY = 1;

    load_hiscore();

    api->setcolor(LGREY, BLACK);
    api->clear();
    draw_frame();
    draw_keys();

    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) { board[y][x] = 0; shown[y][x] = 0xFF; }

    score = lines = level = 0;
    paused = gameover = 0;

    /* `start tetris demo` seeds two almost-complete rows with a square hole
     * right under the spawn point, and makes the first piece the O. Hit
     * space and you get a double line clear -- handy for seeing that the
     * scoring works without playing for two minutes. */
    int demo = (argc > 1 && argv[1][0] == 'd');
    if (demo) {
        for (int y = BH - 2; y < BH; y++)
            for (int x = 0; x < BW; x++)
                if (x != 4 && x != 5) board[y][x] = (u8)(COLORS[x % 7] + 1);
    }
    nxt = demo ? 1 : (int)(api->rand() % 7);   /* 1 = the O piece */
    spawn();
    if (demo) { cur = 1; rot = 0; px = BW / 2 - 2; }
    draw_stats();

    u32 last = api->ticks();

    while (!gameover) {
        int k = api->poll();

        if (k == 'q' || k == 'Q' || k == 27) break;

        if (k == 'p' || k == 'P') {
            paused = !paused;
            api->setcolor(YELLOW, BLACK);
            api->gotoxy(OX + 3, OY + BH / 2);
            api->puts(paused ? " PAUSED " : "        ");
            if (!paused)
                for (int y = 0; y < BH; y++)
                    for (int x = 0; x < BW; x++) shown[y][x] = 0xFF;
            last = api->ticks();
        }

        if (!paused && k) {
            if (k == KEY_LEFT  || k == 'a' || k == 'A') {
                if (fits(cur, rot, px - 1, py)) px--;
            } else if (k == KEY_RIGHT || k == 'd' || k == 'D') {
                if (fits(cur, rot, px + 1, py)) px++;
            } else if (k == KEY_UP    || k == 'w' || k == 'W') {
                rotate_piece(1);
            } else if (k == KEY_DOWN  || k == 's' || k == 'S') {
                if (fits(cur, rot, px, py + 1)) { py++; score++; last = api->ticks(); }
            } else if (k == ' ') {
                while (fits(cur, rot, px, py + 1)) { py++; score += 2; }
                flush_board();
                settle();
                draw_stats();
                last = api->ticks();
            }
        }

        if (!paused) {
            u32 now = api->ticks();
            if ((int)(now - last) >= fall_delay()) {
                last = now;
                if (fits(cur, rot, px, py + 1)) py++;
                else { settle(); draw_stats(); }
            }
            flush_board();
        }

        api->sleep(10);              /* one poll per tick is plenty */
    }

    save_hiscore();

    api->setcolor(gameover ? LRED : WHITE, BLACK);
    api->gotoxy(OX + 1, OY + BH / 2);
    api->puts(gameover ? "  GAME OVER  " : "   BYE   ");
    api->setcolor(LGREY, BLACK);
    api->gotoxy(0, OY + BH + 5);
    api->printf("  score %d   lines %d   best %d\n", score, lines, (int)hiscore);
    if (gameover) {
        api->puts("  press a key");
        api->getkey();
    }
    api->setcolor(LGREY, BLACK);
    api->clear();
    return 0;
}
