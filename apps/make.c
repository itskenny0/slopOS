/* make.pico -- write and run your own games on the machine itself
 *
 * A C compiler that runs inside an operating system small enough to boot
 * on a 486 with a few hundred kilobytes is not a thing that exists, and
 * pretending otherwise would waste your time. What does fit is this: a
 * small line-numbered language, an editor to write it in, and an
 * interpreter that runs it -- all inside one .pico program, so the kernel
 * pays nothing for it until you ask for it.
 *
 * Games are text files on the RAM disk. F2 saves the one you are editing,
 * F5 runs it, escape stops it. You can get back to it later, list it with
 * `cat game.bas` in the shell, and change it by hand if you want to.
 *
 * The language, in one page:
 *
 *   LET A = 10            variables are single letters, numbers only
 *   PRINT "SCORE "; S     prints, then moves down a row
 *   PLOT 10,5,"#"         draws a string at column 10, row 5
 *   COLOR 2               sets the drawing colour
 *   CLS                   clears the screen
 *   WAIT 100              pauses for a hundredth of a second
 *   GOTO 40  GOSUB 200  RETURN
 *   IF A > 3 THEN 70
 *   FOR I = 1 TO 10 ... NEXT I        (STEP works too)
 *   KEY                   the key being held, or 0
 *   RND 6                 a random number from 0 to 5
 *   REM anything at all
 *
 * Numbers are integers. That is not a limitation you will notice writing
 * a snake or a breakout, and it is the reason the whole interpreter is a
 * few hundred lines instead of a few thousand.
 */

#include "picoapp.h"

#define KEY_ESC  27
#define KEY_BS   8
#define KEY_INS  0x8A
#define KEY_DEL  0x89

#define MAXLINES 120
#define LINELEN  56

typedef struct { int num; char src[LINELEN]; } line_t;

static int  W, H;
static line_t *prog;
static int  nlines;
static int  top;                /* first line shown in the listing */
static char in[LINELEN + 8];    /* what you are typing */
static int  inlen;
static const char *note;        /* one line of feedback */
static u32  note_until;

/* ------------------------------------------------------------------ *
 *  little helpers, because there is no libc in here                    *
 * ------------------------------------------------------------------ */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int eqi(const char *a, const char *b)
{
    while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; }
    return *a == *b;
}

static int digits(const char *s)
{
    int n = 0;
    while (s[n] >= '0' && s[n] <= '9') n++;
    return n;
}

/* Match a keyword at the parse position and step over it. Case does not
 * matter: nobody should lose a game to a lowercase `print`. */
static int kw(const char **pp, const char *word)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    const char *w = word;
    while (*w && up(*p) == *w) { p++; w++; }
    if (*w) return 0;
    *pp = p;
    return 1;
}

/* ------------------------------------------------------------------ *
 *  the program                                                        *
 * ------------------------------------------------------------------ */

static int find_line(int num)
{
    for (int i = 0; i < nlines; i++) if (prog[i].num == num) return i;
    return -1;
}

/* Insert or replace. Returns 0 when the table is full. */
static int store_line(int num, const char *src)
{
    int i = find_line(num);
    if (i >= 0) {
        int n = 0;
        while (src[n] && n < LINELEN - 1) { prog[i].src[n] = src[n]; n++; }
        prog[i].src[n] = 0;
        return 1;
    }
    if (nlines >= MAXLINES) return 0;

    int at = nlines;
    for (i = 0; i < nlines; i++) if (prog[i].num > num) { at = i; break; }

    for (i = nlines; i > at; i--) prog[i] = prog[i - 1];

    prog[at].num = num;
    int n = 0;
    while (src[n] && n < LINELEN - 1) { prog[at].src[n] = src[n]; n++; }
    prog[at].src[n] = 0;
    nlines++;
    return 1;
}

static void drop_line(int num)
{
    int i = find_line(num);
    if (i < 0) return;
    for (; i < nlines - 1; i++) prog[i] = prog[i + 1];
    nlines--;
}

/* ------------------------------------------------------------------ *
 *  the interpreter                                                    *
 * ------------------------------------------------------------------ */

static int  vars[26];
static int  pc;                     /* index of the line being run */
static int  colour = WHITE;
static int  tx, ty;                 /* where PRINT writes next */
static int  rows;                   /* the game can use rows 0..rows-1 */
static int  stop;                   /* escape was pressed */
static int  err_line;
static int  lastkey;                /* what KEY hands to the program  */

static struct { int v, to, step, pc; } forstk[8];
static int nfor;
static int gstack[8];
static int ngosub;

static const char *p;               /* parse cursor inside one line */

static void skip(void) { while (*p == ' ' || *p == '\t') p++; }

static int number(void)
{
    int v = 0;
    skip();
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}

static int expr(void);

/* one thing: a number, a variable, a bracketed sum, or a built-in */
static int primary(void)
{
    skip();
    if (*p == '(') {
        p++;
        int v = expr();
        skip();
        if (*p == ')') p++;
        return v;
    }
    if (*p == '-') { p++; return -primary(); }
    if (kw(&p, "RND"))  { int n = primary(); return n > 0 ? (int)(api->rand() % (u32)n) : 0; }
    if (kw(&p, "KEY"))  { int k = lastkey; lastkey = 0; return k; }
    if (kw(&p, "TICKS"))return (int)api->ticks();
    /* the arrow keys, by name: no game should have to know that left
     * is a hundred and twenty nine */
    if (kw(&p, "KUP"))    return 0x81;
    if (kw(&p, "KDOWN"))  return 0x82;
    if (kw(&p, "KLEFT"))  return 0x83;
    if (kw(&p, "KRIGHT")) return 0x84;
    if (kw(&p, "KSPACE")) return 32;
    if (kw(&p, "ABS"))  { int v = primary(); return v < 0 ? -v : v; }

    if (*p >= '0' && *p <= '9') return number();

    char c = up(*p);
    if (c >= 'A' && c <= 'Z') { p++; return vars[c - 'A']; }

    return 0;
}

static int term(void)
{
    int v = primary();
    for (;;) {
        skip();
        if (*p == '*') { p++; v *= primary(); }
        else if (*p == '/') { p++; int d = primary(); v = d ? v / d : 0; }
        else if (*p == '%') { p++; int d = primary(); v = d ? v % d : 0; }
        else return v;
    }
}

static int expr(void)
{
    int v = term();
    for (;;) {
        skip();
        if (*p == '+') { p++; v += term(); }
        else if (*p == '-') { p++; v -= term(); }
        else return v;
    }
}

/* PLOT and PRINT need a string literal in the same place a number can go */
static int strlit(char *out, int max)
{
    int n = 0;
    skip();
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && n < max - 1) out[n++] = *p++;
    if (*p == '"') p++;
    out[n] = 0;
    return 1;
}

static void plot(int x, int y, const char *s)
{
    if (y < 0 || y >= rows || x < 0 || x >= W) return;
    api->gotoxy(x, y);
    api->setcolor((u8)colour, BLACK);
    while (*s && x < W) {
        if (y == H - 1 && x == W - 1) break;    /* that cell scrolls */
        api->putc(*s++);
        x++;
    }
}

/* Print a run of things -- numbers and strings mixed, separated by
 * semicolons -- starting at one place. PRINT and PLOT are the same
 * operation with a different answer to "where". */
static void items(int x, int y, int *endx)
{
    for (;;) {
        char buf[LINELEN];
        skip();
        if (!*p) break;
        if (strlit(buf, sizeof buf)) { plot(x, y, buf); x += slen(buf); }
        else {
            int v = expr();
            char n[12];
            app_itoa(v, n);
            plot(x, y, n);
            x += slen(n);
        }
        skip();
        if (*p == ';') { p++; continue; }
        break;
    }
    if (endx) *endx = x;
}

/* Jump to a line number. Returns 0 if there is no such line. */
static int jump(int num)
{
    int i = find_line(num);
    if (i < 0) return 0;
    pc = i;
    return 1;
}

/* Run one line. `pc` may end up anywhere when this returns. */
static void exec(void)
{
    const char *s = prog[pc].src;
    p = s;
    skip();
    if (!*p) { pc++; return; }

    if (kw(&p, "REM"))   { pc++; return; }
    if (kw(&p, "LET"))   {
        skip();
        char c = up(*p);
        if (c >= 'A' && c <= 'Z') {
            p++;
            skip();
            if (*p == '=') { p++; vars[c - 'A'] = expr(); }
        }
        pc++; return;
    }
    if (kw(&p, "PRINT")) {
        items(tx, ty, 0);
        tx = 0;
        if (ty < rows - 1) ty++;
        pc++; return;
    }
    if (kw(&p, "PLOT"))  {
        int x = expr();
        skip(); if (*p == ',') p++;
        int y = expr();
        skip(); if (*p == ',') p++;
        items(x, y, 0);
        pc++; return;
    }
    if (kw(&p, "COLOR")) { colour = expr() & 15; pc++; return; }
    if (kw(&p, "CLS"))   {
        api->setcolor((u8)colour, BLACK);
        api->clear();
        tx = ty = 0;
        pc++; return;
    }
    if (kw(&p, "WAIT"))  { api->sleep((u32)expr() * 10u); pc++; return; }
    if (kw(&p, "BEEP"))  {
        int hz = expr();
        skip(); if (*p == ',') p++;
        int ms = expr();
        api->beep((u32)(hz ? hz : 440), (u32)(ms ? ms : 50));
        pc++; return;
    }
    if (kw(&p, "GOSUB")) {
        int n = expr();
        if (ngosub < 8 && jump(n)) { gstack[ngosub++] = pc + 0; return; }
        pc++; return;
    }
    if (kw(&p, "RETURN")) {
        if (ngosub > 0) pc = gstack[--ngosub];
        else pc++;
        return;
    }
    if (kw(&p, "GOTO"))  { int n = expr(); if (!jump(n)) pc++; return; }
    if (kw(&p, "IF"))    {
        int a = expr();
        const char *op = p;
        skip();
        int cmp = 0;
        if (*p == '=') { p++; cmp = 1; }
        else if (*p == '<' && p[1] == '>') { p += 2; cmp = 2; }
        else if (*p == '<') { p++; cmp = 3; }
        else if (*p == '>') { p++; cmp = 4; }
        (void)op;
        int b = expr();
        int yes = (cmp == 1) ? (a == b) : (cmp == 2) ? (a != b)
                : (cmp == 3) ? (a < b)  : (cmp == 4) ? (a > b) : 0;
        if (yes && kw(&p, "THEN")) { int n = expr(); if (!jump(n)) pc++; }
        else pc++;
        return;
    }
    if (kw(&p, "FOR"))   {
        skip();
        char c = up(*p);
        if (c >= 'A' && c <= 'Z' && nfor < 8) {
            p++;
            skip();
            if (*p == '=') p++;
            int from = expr();
            int to = 0, step = 1;
            if (kw(&p, "TO")) to = expr();
            if (kw(&p, "STEP")) step = expr();
            forstk[nfor].v = c - 'A';
            forstk[nfor].to = to;
            forstk[nfor].step = step ? step : 1;
            forstk[nfor].pc = pc + 1;
            vars[c - 'A'] = from;
            nfor++;
        }
        pc++; return;
    }
    if (kw(&p, "NEXT"))  {
        skip();
        char c = up(*p);
        if (nfor > 0) {
            int i = nfor - 1;
            vars[forstk[i].v] += forstk[i].step;
            int v = vars[forstk[i].v];
            int done = forstk[i].step > 0 ? (v > forstk[i].to) : (v < forstk[i].to);
            if (!done) { pc = forstk[i].pc; return; }
            nfor--;
        }
        (void)c;
        pc++; return;
    }
    if (kw(&p, "END"))   { stop = 1; return; }

    /* nothing matched: say so instead of silently doing nothing */
    err_line = prog[pc].num;
    stop = 1;
}

static void run_program(void)
{
    stop = 0; err_line = 0;
    nfor = ngosub = 0;
    pc = 0; tx = ty = 0;
    colour = WHITE;

    api->setcolor(LGREY, BLACK);
    api->clear();
    api->setcolor(DGREY, BLACK);
    api->gotoxy(0, H - 1);
    api->puts("escape stops");
    api->setcolor(LGREY, BLACK);

    u32 guard = api->ticks() + 100;         /* never block the machine */

    while (!stop && pc >= 0 && pc < nlines) {
        int k = api->poll();
        if (k == KEY_ESC) break;
        if (k) lastkey = k;
        exec();
        if (api->ticks() > guard) {         /* let other things breathe */
            api->sleep(1);
            guard = api->ticks() + 100;
        }
    }

    api->setcolor(LGREY, BLACK);
    api->gotoxy(0, H - 1);
    if (err_line) api->printf("? error in line %d", err_line);
    else          api->puts("ready. any key goes back to the editor");
    while (!api->poll()) api->sleep(20);
    api->poll();
}

/* ------------------------------------------------------------------ *
 *  files                                                              *
 * ------------------------------------------------------------------ */

static void say(const char *s)
{
    note = s;
    note_until = api->ticks() + 250;
}

static char fname[16] = "game.bas";

static void save_named(const char *name)
{
    char *buf = (char *)api->malloc(4200);
    if (!buf) { say("no room to save"); return; }
    int n = 0;
    for (int i = 0; i < nlines && n < 4100; i++) {
        char t[12];
        app_itoa(prog[i].num, t);
        for (int j = 0; t[j]; j++) buf[n++] = t[j];
        buf[n++] = ' ';
        for (int j = 0; prog[i].src[j] && n < 4100; j++) buf[n++] = prog[i].src[j];
        buf[n++] = '\n';
    }
    buf[n] = 0;
    api->fs_write(name, buf, (u32)n);
    api->free(buf);
    say("saved");
}

static void save_prog(void) { save_named(fname); }

static int load_named(const char *name)
{
    char *buf = (char *)api->malloc(4200);
    if (!buf) return 0;
    int n = api->fs_read(name, buf, 4199);
    if (n <= 0) { api->free(buf); return 0; }
    buf[n] = 0;

    nlines = 0;
    int i = 0;
    while (i < n) {
        int d = digits(buf + i);
        if (!d) { while (i < n && buf[i] != '\n') i++; if (i < n) i++; continue; }
        int num = 0;
        for (int j = 0; j < d; j++) num = num * 10 + (buf[i + j] - '0');
        i += d;
        while (buf[i] == ' ') i++;

        char src[LINELEN];
        int k = 0;
        while (i < n && buf[i] != '\n' && k < LINELEN - 1) src[k++] = buf[i++];
        src[k] = 0;
        if (i < n && buf[i] == '\n') i++;
        if (!store_line(num, src)) break;
    }
    api->free(buf);
    top = 0;
    for (int j = 0; name[j] && j < 15; j++) { fname[j] = name[j]; fname[j + 1] = 0; }
    return 1;
}

static void load_prog(void)
{
    if (load_named(fname)) say("loaded");
    else say("no saved game yet");
}

/* ------------------------------------------------------------------ *
 *  the editor                                                         *
 * ------------------------------------------------------------------ */

static void draw_screen(void)
{
    api->setcolor(LGREY, BLACK);
    api->clear();

    api->setcolor(BLACK, LGREY);
    api->gotoxy(0, 0);
    for (int i = 0; i < W - 1; i++) api->putc(' ');
    api->gotoxy(1, 0);
    api->puts("PICO MAKE");
    api->setcolor(DGREY, LGREY);
    api->gotoxy(12, 0);
    api->printf("%d line%s", nlines, nlines == 1 ? "" : "s");

    int vis = H - 5;
    for (int i = 0; i < vis; i++) {
        int idx = top + i;
        int y = 1 + i;
        api->gotoxy(0, y);
        if (idx >= nlines) { api->setcolor(LGREY, BLACK); continue; }
        api->setcolor(YELLOW, BLACK);
        api->printf("%4d ", prog[idx].num);
        api->setcolor(LGREY, BLACK);
        api->puts(prog[idx].src);
    }

    api->setcolor(BLACK, LGREY);
    api->gotoxy(0, H - 3);
    for (int i = 0; i < W - 1; i++) api->putc(' ');
    api->gotoxy(1, H - 3);
    api->printf("> %s", in);

    api->setcolor(DGREY, BLACK);
    api->gotoxy(0, H - 1);
    for (int i = 0; i < W - 1; i++) api->putc(' ');
    api->gotoxy(1, H - 1);
    if (note && *note && api->ticks() < note_until) api->puts(note);
    else api->puts("f5 run   f2 save   f3 load   esc quit   type 10 PRINT \"HI\"");
}

static void commit(void)
{
    int i = 0;
    while (in[i] == ' ') i++;
    if (!in[i]) return;

    int d = digits(in + i);
    if (d) {
        int num = 0;
        for (int j = 0; j < d; j++) num = num * 10 + (in[i + j] - '0');
        i += d;
        while (in[i] == ' ') i++;
        if (!in[i]) {
            drop_line(num);
            say("line gone");
        } else if (!store_line(num, in + i)) {
            say("the program is full");
        } else {
            say("ok");
        }
    } else if (eqi(in, "RUN"))   { inlen = 0; in[0] = 0; run_program(); return; }
    else if (eqi(in, "LIST"))    { top = 0; say("listing"); }
    else if (eqi(in, "SAVE"))    { save_prog(); }
    else if (eqi(in, "LOAD"))    { load_prog(); }
    else if (eqi(in, "NEW"))     { nlines = 0; top = 0; say("new program"); }
    else if (eqi(in, "QUIT"))    { inlen = 0; in[0] = 0; return; }
    else say("? type a number first, or RUN");

    inlen = 0;
    in[0] = 0;
}

int app_main(int argc, char **argv)
{
    (void)argc;
    W = api->width();
    H = api->height();
    rows = H - 1;

    prog = (line_t *)api->malloc(sizeof(line_t) * MAXLINES);
    if (!prog) return 1;
    for (int i = 0; i < MAXLINES; i++) { prog[i].num = 0; prog[i].src[0] = 0; }

    /* `make run game.bas` -- how the shell starts a game of yours */
    if (argc > 2 && argv[1] && argv[1][0] == 'r') {
        load_prog();
        run_program();
        api->free(prog);
        api->setcolor(LGREY, BLACK);
        api->clear();
        return 0;
    }

    api->cursor(1);
    draw_screen();

    /* A machine with nothing on it should still have something to play
     * with, so an empty editor starts with a game already in it. */
    if (load_named("game.bas")) say("game.bas loaded");
    if (nlines == 0) {
        store_line(10, "REM WALK -- arrow keys move the @");
        store_line(20, "CLS");
        store_line(30, "LET X = 10");
        store_line(40, "LET Y = 5");
        store_line(50, "LET S = 0");
        store_line(60, "PLOT X,Y, \" \"");
        store_line(70, "LET K = KEY");
        store_line(80, "IF K = KLEFT THEN 130");
        store_line(90, "IF K = KRIGHT THEN 150");
        store_line(100, "IF K = KUP THEN 170");
        store_line(110, "IF K = KDOWN THEN 190");
        store_line(120, "GOTO 210");
        store_line(130, "LET X = X - 1");
        store_line(140, "GOTO 210");
        store_line(150, "LET X = X + 1");
        store_line(160, "GOTO 210");
        store_line(170, "LET Y = Y - 1");
        store_line(180, "GOTO 210");
        store_line(190, "LET Y = Y + 1");
        store_line(200, "GOTO 210");
        store_line(210, "IF X < 0 THEN 230");
        store_line(220, "IF X < 70 THEN 250");
        store_line(230, "LET X = 0");
        store_line(240, "GOTO 250");
        store_line(250, "IF Y < 2 THEN 270");
        store_line(260, "IF Y < 21 THEN 290");
        store_line(270, "LET Y = 2");
        store_line(280, "GOTO 290");
        store_line(290, "LET S = S + 1");
        store_line(300, "COLOR 3");
        store_line(310, "PLOT X,Y,\"@\"");
        store_line(320, "PLOT 0,0,\"steps \"; S");
        store_line(330, "PLOT 0,1,\"escape stops\"");
        store_line(340, "WAIT 2");
        store_line(350, "GOTO 60");
        say("a game to start from: press f5");
    }
    draw_screen();

    for (;;) {
        int k = api->poll();
        if (k) {
            if (k == KEY_ESC) break;
            else if (k == KEY_F5)      { run_program(); draw_screen(); }
            else if (k == KEY_F2)      { save_prog(); }
            else if (k == KEY_F3)      { load_prog(); }
            else if (k == KEY_UP)      { if (top > 0) top--; }
            else if (k == KEY_DOWN)    { if (top < nlines - 1) top++; }
            else if (k == KEY_PGUP)    { top -= 8; if (top < 0) top = 0; }
            else if (k == KEY_PGDN)    { top += 8;
                                         if (top > nlines - 1) top = nlines - 1; }
            else if (k == KEY_BS)      { if (inlen) in[--inlen] = 0; }
            else if (k == '\n' || k == '\r') commit();
            else if (k >= 32 && k < 127) {
                if (inlen < LINELEN + 4) { in[inlen++] = (char)k; in[inlen] = 0; }
            }
            draw_screen();
        }
        api->sleep(15);
    }

    api->free(prog);
    api->cursor(1);
    api->setcolor(LGREY, BLACK);
    api->clear();
    return 0;
}
