/* hello.pico -- the smallest useful .pico program.
 * Shows the three things every program gets: the api table, arguments,
 * and an exit code. */
#include "picoapp.h"

int app_main(int argc, char **argv)
{
    api->setcolor(LGREEN, BLACK);
    api->puts("\n  hello from a .pico program!\n\n");
    api->setcolor(LGREY, BLACK);

    api->printf("  I am code the kernel had never seen until you typed\n");
    api->printf("  'start hello'. Nothing is reserved for me in advance:\n");
    api->printf("  the kernel dropped me wherever there was room and fixed\n");
    api->printf("  up every address inside me. I talk to PicoOS\n");
    api->printf("  through an api table with %d entries.\n\n", (int)(api->size / 4));

    api->printf("  console  : %dx%d\n", api->width(), api->height());
    api->printf("  uptime   : %d ms\n", (int)api->ms());
    api->printf("  api ver  : %d\n", (int)api->version);

    if (argc > 1) {
        api->printf("  arguments:");
        for (int i = 1; i < argc; i++) api->printf(" %s", argv[i]);
        api->putc('\n');
    }
    api->putc('\n');
    return 0;
}
