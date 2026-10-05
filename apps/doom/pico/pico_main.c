/* pico_main.c -- the .pico entry point for DOOM.
 *
 * crt0.c calls app_main with the kernel api table already in place.
 * From here on it is doomgeneric like on any other platform: create
 * the game, then tick it forever. Quitting (the in-game menu, or a
 * fatal error) goes through exit(), which drops back to text mode
 * and returns to whatever ran DOOM -- usually the graphical menu.
 *
 * This file is part of the PicoOS DOOM port, GPL-2.0-or-later.
 */

#include "picoapp.h"

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

int app_main(int argc, char **argv)
{
    api->puts("DOOM, the shareware episode, running on PicoOS.\n");

    doomgeneric_Create(argc, argv);

    for (;;)
        doomgeneric_Tick();

    return 0;
}
