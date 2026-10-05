/* snake.pico -- the other classic, in about 120 lines.
 * Same trick as tetris: only the head, the old tail and the food are
 * repainted each frame, so it stays smooth on a slow machine. */
#include "picoapp.h"

#define W 38
#define H 18
static int OX = 2, OY = 2;
#define MAXLEN (W * H)

static s16 sx[MAXLEN], sy[MAXLEN];
static int head, len;
static int dx, dy;
static int fx, fy;
static int score, dead;

static int hits_body(int x, int y)
{
    for (int i = 0; i < len; i++) {
        int j = (head - i + MAXLEN) % MAXLEN;
        if (sx[j] == x && sy[j] == y) return 1;
    }
    return 0;
}

static void put(int x, int y, const char *s, u8 fg)
{
    api->setcolor(fg, BLACK);
    api->gotoxy(OX + x, OY + y);
    api->puts(s);
}

static void place_food(void)
{
    do {
        fx = (int)(api->rand() % W);
        fy = (int)(api->rand() % H);
    } while (hits_body(fx, fy));
    put(fx, fy, "*", LRED);
}

int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;

    api->setcolor(LGREY, BLACK);
    api->clear();

    OX = (api->width()  - W) / 2;  if (OX < 2) OX = 2;
    OY = (api->height() - H) / 3;  if (OY < 2) OY = 2;

    api->setcolor(DGREY, BLACK);
    api->gotoxy(OX - 1, OY - 1);
    for (int i = 0; i < W + 2; i++) api->putc('-');
    api->gotoxy(OX - 1, OY + H);
    for (int i = 0; i < W + 2; i++) api->putc('-');
    for (int y = 0; y < H; y++) {
        api->gotoxy(OX - 1, OY + y);     api->putc('|');
        api->gotoxy(OX + W, OY + y);     api->putc('|');
    }

    head = 0; len = 4; dx = 1; dy = 0; score = 0; dead = 0;
    sx[0] = W / 2; sy[0] = H / 2;
    place_food();

    u32 last = api->ticks();
    int delay = 12;

    while (!dead) {
        int k = api->poll();
        if (k == 'q' || k == 'Q' || k == 27) break;
        if ((k == KEY_UP    || k == 'w') && dy == 0) { dx = 0; dy = -1; }
        if ((k == KEY_DOWN  || k == 's') && dy == 0) { dx = 0; dy =  1; }
        if ((k == KEY_LEFT  || k == 'a') && dx == 0) { dx = -1; dy = 0; }
        if ((k == KEY_RIGHT || k == 'd') && dx == 0) { dx =  1; dy = 0; }

        u32 now = api->ticks();
        if ((int)(now - last) < delay) { api->sleep(5); continue; }
        last = now;

        int nx = sx[head] + dx, ny = sy[head] + dy;
        if (nx < 0 || nx >= W || ny < 0 || ny >= H || hits_body(nx, ny)) {
            dead = 1;
            break;
        }

        int tail = (head - len + 1 + MAXLEN) % MAXLEN;
        int grew = (nx == fx && ny == fy);
        if (!grew) put(sx[tail], sy[tail], " ", BLACK);
        else {
            len++;
            score += 10;
            if (delay > 4) delay--;
            api->beep(700, 20);
            place_food();
        }

        put(sx[head], sy[head], "o", LGREEN);
        head = (head + 1) % MAXLEN;
        sx[head] = (s16)nx; sy[head] = (s16)ny;
        put(nx, ny, "@", WHITE);

        api->setcolor(WHITE, BLACK);
        api->gotoxy(OX, OY + H + 2);
        api->printf("score %d   length %d   q to quit   ", score, len);
    }

    api->setcolor(dead ? LRED : LGREY, BLACK);
    api->gotoxy(OX, OY + H + 4);
    api->printf(dead ? "game over -- score %d\n" : "bye -- score %d\n", score);
    api->setcolor(LGREY, BLACK);
    if (dead) { api->puts("  press a key"); api->getkey(); }
    api->clear();
    return 0;
}
