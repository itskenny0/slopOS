/* pong.pico -- you against the machine. Any key serves, esc quits. */
#include "picoapp.h"
#define KEY_ESC 27
int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int W = api->width(), H = api->height();
    int py = H / 2, ey = H / 2, bx = W / 2, by = H / 2;
    int vx = 1, vy = 1, ps = 0, es = 0;
    char line[64];
    api->cursor(0);
    for (;;) {
        int k = api->poll();
        if (k == KEY_ESC) break;
        if (k == 0x81 || k == 'w') py--;
        if (k == 0x82 || k == 's') py++;
        if (py < 1) py = 1;
        if (py > H - 2) py = H - 2;
        bx += vx; by += vy;
        if (by < 1) { by = 1; vy = -vy; }
        if (by > H - 2) { by = H - 2; vy = -vy; }
        if (bx == 3 && by >= py - 2 && by <= py + 2 && vx < 0) { vx = -vx; api->beep(880, 20); }
        if (bx == W - 4 && by >= ey - 2 && by <= ey + 2 && vx > 0) { vx = -vx; api->beep(660, 20); }
        if (bx == 0) { es++; bx = W / 2; by = H / 2; vx = 1; vy = 1; api->beep(220, 60); }
        if (bx == W - 1) { ps++; bx = W / 2; by = H / 2; vx = -1; vy = 1; api->beep(440, 60); }
        if (by > ey + (bx < W / 2 ? 1 : 0) && bx > W / 2) ey++;
        if (by < ey - (bx < W / 2 ? 1 : 0) && bx > W / 2) ey--;
        if (ey < 1) ey = 1;
        if (ey > H - 2) ey = H - 2;
        api->clear();
        api->gotoxy(W / 2, 0);
        api->setcolor(0, 3);
        api->puts(" ");
        api->putc((char)(ps / 10 ? '0' + ps / 10 : ' '));
        api->putc((char)('0' + ps % 10));
        api->putc('-');
        api->putc((char)(es / 10 ? '0' + es / 10 : ' '));
        api->putc((char)('0' + es % 10));
        api->puts("  pong -- w/s or arrows, esc  ");
        api->setcolor(7, 0);
        api->gotoxy(W / 2, 2);
        for (int y = 1; y < H - 1; y += 2) { api->gotoxy(W / 2, y); api->putc('.'); }
        api->gotoxy(2, py); api->putc('|');
        api->gotoxy(W - 3, ey); api->putc('|');
        api->gotoxy(bx, by); api->putc('O');
        api->sleep(60);
    }
    (void)line;
    api->cursor(1);
    api->clear();
    return 0;
}
