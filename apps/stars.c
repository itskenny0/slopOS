/* stars.pico -- a starfield you fly through. Any key quits. */
#include "picoapp.h"
#define N 140
typedef struct { int x, y, s; } star_t;
static star_t st[N];
static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; }
static char shade(int s) { if (s > 8) return '.'; if (s > 6) return '+'; if (s > 4) return '*'; return '#'; }
int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int W = api->width(), H = api->height();
    for (int i = 0; i < N; i++) {
        st[i].x = (int)(rnd() % (u32)(W * 4)) - W * 2;
        st[i].y = (int)(rnd() % (u32)(H * 4)) - H * 2;
        st[i].s = (int)(rnd() % 10) + 1;
    }
    api->cursor(0);
    api->clear();
    for (int tick = 0;; tick++) {
        if (api->poll()) break;
        api->clear();
        for (int i = 0; i < N; i++) {
            st[i].x += st[i].x > 0 ? st[i].s : -st[i].s;
            st[i].y += st[i].y > 0 ? st[i].s / 2 : -(st[i].s / 2);
            int px = W / 2 + st[i].x / 8;
            int py = H / 2 + st[i].y / 4;
            if (px < 0 || px >= W || py < 0 || py >= H) {
                st[i].x = (int)(rnd() % 40u) - 20;
                st[i].y = (int)(rnd() % 40u) - 20;
                continue;
            }
            api->gotoxy(px, py);
            api->putc(shade(st[i].s));
        }
        api->gotoxy(W / 2, H / 2);
        api->setcolor(14, 0);
        api->puts("<*>");
        api->gotoxy(0, 0);
        api->setcolor(0, 3);
        api->puts(" stars -- any key quits ");
        api->setcolor(7, 0);
        api->sleep(80);
    }
    api->cursor(1);
    api->clear();
    return 0;
}
