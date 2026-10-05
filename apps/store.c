/* store.pico -- the program store, one network hop away.
 *
 * A short list of programs this machine does not have yet, each with
 * the address it lives at. Pick one, press enter, and the kernel's
 * http/https client pulls the bytes in, checks they really are a .pico
 * program, and writes them into the ramdisk as a file -- which is what
 * installing is, on a machine with no installer. It shows in Programs
 * the moment the store closes.
 *
 * The kernel fetches an address by itself if it has none yet, so on a
 * cable the store just works. Wireless has to be joined first, from
 * the shell: wifi <name> <password> -- the store says so when it has
 * no network, instead of failing quietly.
 */

#include "picoapp.h"

#define KEY_ESC 27

#define NITEMS 2
#define DLMAX  49152            /* the biggest thing the store fetches */

static const struct {
    const char *name;
    const char *desc;
    const char *url;
} items[NITEMS] = {
    { "PONG.PIC",  "pong, you against the machine", "https://x0.at/YYwi.pico" },
    { "STARS.PIC", "a starfield you fly through",   "https://x0.at/vTCd.pico" },
};

static char *buf;               /* the download lands here             */

static int installed(const char *name)
{
    char n[20];
    u32 sz;
    for (int i = 0; ; i++) {
        if (!api->files(i, n, &sz)) return 0;
        for (int k = 0; k < 20; k++) {
            if (n[k] != name[k]) break;
            if (!n[k]) return 1;
        }
    }
}

static void draw(int sel, const char *msg)
{
    api->clear();
    api->setcolor(15, 1);
    api->gotoxy(0, 0);
    api->puts("  the PicoOS store  --  enter downloads, q quits  ");
    api->setcolor(7, 0);
    api->gotoxy(2, 2);
    api->puts("program       what it is                 here yet?");
    api->gotoxy(2, 3);
    api->puts("---------------------------------------------");

    for (int i = 0; i < NITEMS; i++) {
        int y = 5 + i * 2;
        api->gotoxy(2, y);
        if (i == sel) {
            api->setcolor(15, 1);
            api->puts(" > ");
        } else {
            api->setcolor(7, 0);
            api->puts("   ");
        }
        api->puts(items[i].name);
        api->gotoxy(14, y);
        api->puts(items[i].desc);
        api->gotoxy(43, y);
        api->puts(installed(items[i].name) ? "yes" : "--");
        api->setcolor(7, 0);
    }

    api->gotoxy(2, 5 + NITEMS * 2 + 1);
    api->puts("on a cable this works at once; wireless");
    api->gotoxy(2, 5 + NITEMS * 2 + 2);
    api->puts("needs wifi <name> <password> in the shell");

    if (msg && msg[0]) {
        api->gotoxy(0, api->height() - 2);
        api->setcolor(0, 3);
        api->puts(" ");
        api->puts(msg);
        api->puts(" ");
        api->setcolor(7, 0);
    }
}

static void fetch(int sel)
{
    static u32 last;
    char status[64];
    u32 got = 0;
    int r;

    /* a held enter repeats: one download per half second, not a storm */
    if (api->ticks() - last < 50) return;
    last = api->ticks();

    draw(sel, "fetching...");
    if (api->net_up() < 0) {
        draw(sel, "no network: plug in a cable and retry, or");
        return;
    }

    r = api->net_get(items[sel].url, buf, DLMAX, &got, status,
                     (int)sizeof status);
    if (r != 0 || got < 8) {
        draw(sel, status[0] ? status : "the download did not arrive");
        return;
    }
    if (buf[0] != 'P' || buf[1] != 'I' || buf[2] != 'C' || buf[3] != 'O') {
        draw(sel, "that was not a .pico program");
        return;
    }
    if (api->fs_write(items[sel].name, buf, got) >= 0)
        draw(sel, "installed -- it is in Programs now");
    else
        draw(sel, "the ramdisk would not take it");
}

int app_main(int argc, char **argv)
{
    int sel = 0;

    buf = (char *)api->malloc(DLMAX);
    if (!buf) {
        api->puts("store: not enough memory to download into\n");
        return 1;
    }

    /* `store <url>` fetches straight away: the store is also a
     * one-line installer for anything you have an address for. */
    if (argc > 1 && argv[1][0]) {
        api->puts("store: fetching ");
        api->puts(argv[1]);
        api->puts("\n");
        {
            char status[64];
            u32 got = 0;
            int r = api->net_get(argv[1], buf, DLMAX, &got, status,
                                 (int)sizeof status);
            if (r != 0 || got < 8) {
                api->puts(status[0] ? status : "the download did not arrive");
                api->puts("\n");
            } else if (buf[0] != 'P' || buf[1] != 'I' ||
                       buf[2] != 'C' || buf[3] != 'O') {
                api->puts("that was not a .pico program\n");
            } else {
                if (api->fs_write("DOWNLOAD.PIC", buf, got) >= 0)
                    api->puts("saved as DOWNLOAD.PIC\n");
                else
                    api->puts("the ramdisk would not take it\n");
            }
        }
        api->free(buf);
        return 0;
    }

    api->cursor(0);
    for (;;) {
        draw(sel, "");
        int k = api->getkey();
        if (k == KEY_ESC || k == 'q' || k == 'Q' || k == KEY_F10) break;
        if (k == KEY_UP)   { sel = sel ? sel - 1 : NITEMS - 1; }
        if (k == KEY_DOWN) { sel = (sel + 1) % NITEMS; }
        if (k == '1') sel = 0;
        if (k == '2') sel = 1;
        if (k == '\n' || k == '\r' || k == 'd' || k == 'D') fetch(sel);
    }

    api->cursor(1);
    api->clear();
    api->free(buf);
    return 0;
}
