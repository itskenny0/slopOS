/* doomgeneric_pico.c -- doomgeneric's platform half on PicoOS.
 *
 * Graphics: the game draws a 320x200 indexed frame into DG_ScreenBuffer
 * and calls DG_DrawFrame, which hands it to the kernel's blit() for
 * integer-scaled display. The palette travels through setpal() only on
 * frames where the game actually changed it.
 *
 * Input: PicoOS reports which physical keys are held (PS/2 set-1
 * scancodes, exactly what a 1993 PC keyboard sent). This file polls the
 * interesting ones every tick, turns make/break edges into press/release
 * events, and translates them to the key codes i_input.c wants.
 *
 * This file is part of the PicoOS DOOM port, GPL-2.0-or-later.
 */

#include "picoapp.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "i_video.h"

/* E0-prefixed scancodes (arrows, right ctrl/alt, keypad enter) arrive
 * at api->keydown with bit 8 set; see the v13 api note in picoapp.h */
#define E0 0x100

static int gfx_on;
static int first_frame = 1;

/* back to text mode; i_system.c calls this before printing an error so
 * the message lands on a screen the user can read */
void DG_EnterText(void)
{
    if (gfx_on) {
        api->gfx_mode(0);
        gfx_on = 0;
    }
}

void DG_Init(void)
{
    /* Prove the screen does 256 colours before D_DoomMain prints its
     * first line, then drop back to text so the startup banner stays
     * visible. The first frame switches to graphics for good. */
    if (!api->gfx_mode(1) || !api->gfx256()) {
        api->gfx_mode(0);
        I_Error("DOOM needs a 256-colour screen (VBE or UEFI).");
    }
    api->gfx_mode(0);
}

void DG_DrawFrame(void)
{
    static unsigned char pal[768];
    int i;

    if (first_frame) {
        api->gfx_mode(1);
        gfx_on = 1;
        first_frame = 0;
    }
    if (palette_changed) {
        for (i = 0; i < 256; i++) {
            pal[i * 3] = colors[i].r;
            pal[i * 3 + 1] = colors[i].g;
            pal[i * 3 + 2] = colors[i].b;
        }
        api->setpal(pal);
        palette_changed = false;
    }
    if (!api->blit(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY))
        I_Error("DOOM lost its 256-colour screen.");
}

void DG_SleepMs(unsigned int ms)
{
    api->sleep(ms);
}

unsigned int DG_GetTicksMs(void)
{
    return api->ms();
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;                 /* PicoOS has no windows to title */
}

/* scancode to DOOM key code. Letters are lowercase on purpose: i_input
 * applies the shift translation itself while shift is held. */
static const struct {
    int sc;
    unsigned char dk;
} keymap[] = {
    { 0x01, KEY_ESCAPE },
    { 0x02, '1' }, { 0x03, '2' }, { 0x04, '3' }, { 0x05, '4' },
    { 0x06, '5' }, { 0x07, '6' }, { 0x08, '7' }, { 0x09, '8' },
    { 0x0A, '9' }, { 0x0B, '0' },
    { 0x0C, KEY_MINUS }, { 0x0D, KEY_EQUALS },
    { 0x0E, KEY_BACKSPACE }, { 0x0F, KEY_TAB },
    { 0x10, 'q' }, { 0x11, 'w' }, { 0x12, 'e' }, { 0x13, 'r' },
    { 0x14, 't' }, { 0x15, 'y' }, { 0x16, 'u' }, { 0x17, 'i' },
    { 0x18, 'o' }, { 0x19, 'p' },
    { 0x1A, '[' }, { 0x1B, ']' },
    { 0x1C, KEY_ENTER }, { 0x1D, KEY_FIRE },
    { 0x1E, 'a' }, { 0x1F, 's' }, { 0x20, 'd' }, { 0x21, 'f' },
    { 0x22, 'g' }, { 0x23, 'h' }, { 0x24, 'j' }, { 0x25, 'k' },
    { 0x26, 'l' },
    { 0x27, ';' }, { 0x28, '\'' }, { 0x29, '`' },
    { 0x2A, KEY_RSHIFT }, { 0x2B, '\\' },
    { 0x2C, 'z' }, { 0x2D, 'x' }, { 0x2E, 'c' }, { 0x2F, 'v' },
    { 0x30, 'b' }, { 0x31, 'n' }, { 0x32, 'm' },
    { 0x33, ',' }, { 0x34, '.' }, { 0x35, '/' },
    { 0x36, KEY_RSHIFT },
    { 0x37, KEYP_MULTIPLY },
    { 0x38, KEY_RALT }, { 0x39, KEY_USE },
    { 0x3A, KEY_CAPSLOCK },
    { 0x3B, KEY_F1 }, { 0x3C, KEY_F2 }, { 0x3D, KEY_F3 },
    { 0x3E, KEY_F4 }, { 0x3F, KEY_F5 }, { 0x40, KEY_F6 },
    { 0x41, KEY_F7 }, { 0x42, KEY_F8 }, { 0x43, KEY_F9 },
    { 0x44, KEY_F10 },
    { 0x45, KEY_NUMLOCK }, { 0x46, KEY_SCRLCK },
    { 0x47, KEYP_7 }, { 0x48, KEYP_8 }, { 0x49, KEYP_9 },
    { 0x4A, KEYP_MINUS },
    { 0x4B, KEYP_4 }, { 0x4C, KEYP_5 }, { 0x4D, KEYP_6 },
    { 0x4E, KEYP_PLUS },
    { 0x4F, KEYP_1 }, { 0x50, KEYP_2 }, { 0x51, KEYP_3 },
    { 0x52, '0' }, { 0x53, '.' },
    { 0x57, KEY_F11 }, { 0x58, KEY_F12 },
    /* E0-prefixed keys */
    { E0 | 0x1C, KEY_ENTER }, { E0 | 0x1D, KEY_FIRE },
    { E0 | 0x35, KEYP_DIVIDE }, { E0 | 0x37, KEY_PRTSCR },
    { E0 | 0x38, KEY_RALT }, { E0 | 0x46, KEY_PAUSE },
    { E0 | 0x47, KEY_HOME }, { E0 | 0x48, KEY_UPARROW },
    { E0 | 0x49, KEY_PGUP }, { E0 | 0x4B, KEY_LEFTARROW },
    { E0 | 0x4D, KEY_RIGHTARROW }, { E0 | 0x4F, KEY_END },
    { E0 | 0x50, KEY_DOWNARROW }, { E0 | 0x51, KEY_PGDN },
    { E0 | 0x52, KEY_INS }, { E0 | 0x53, KEY_DEL },
};

#define KEYMAPN (sizeof(keymap) / sizeof(keymap[0]))
#define KEYQ 64

static unsigned char prev_down[KEYMAPN];
static unsigned char q_press[KEYQ];
static unsigned char q_key[KEYQ];
static int qh, qt;

static void q_push(int pressed, unsigned char key)
{
    q_press[qh] = pressed ? 1 : 0;
    q_key[qh] = key;
    qh = (qh + 1) % KEYQ;
    if (qh == qt)                /* overrun: drop the oldest event */
        qt = (qt + 1) % KEYQ;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    unsigned i;

    /* poll every mapped key and queue the edges */
    for (i = 0; i < KEYMAPN; i++) {
        int cur = api->keydown(keymap[i].sc) ? 1 : 0;
        if (cur && !prev_down[i])
            q_push(1, keymap[i].dk);
        else if (!cur && prev_down[i])
            q_push(0, keymap[i].dk);
        prev_down[i] = (unsigned char)cur;
    }

    if (qt == qh)
        return 0;
    *pressed = q_press[qt];
    *key = q_key[qt];
    qt = (qt + 1) % KEYQ;
    return 1;
}
