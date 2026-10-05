#include "pico.h"

#define BUFSZ 128
static volatile unsigned char buf[BUFSZ];
static volatile int  head = 0, tail = 0;
static int shift = 0, caps = 0, ctrl = 0, ext = 0;

/* Which physical keys are held right now, by set-1 scancode. The queue
 * above only carries presses; a game also needs to know that a key is
 * *still* down. E0-prefixed keys (arrows, right ctrl/alt, keypad enter)
 * live in the second array. */
static u8 kstate[128];
static u8 kstate_e0[128];

int kbd_down(int code)
{
    if (!task_owns_console()) return 0;
    if (code >= 0x100 && code < 0x180) return kstate_e0[code & 0x7F];
    if (code >= 0 && code < 128) return kstate[code];
    return 0;
}

/* scancode set 1 -> ASCII */
static const char map_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ', 0
};
static const char map_upper[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0, ' ', 0
};

static void push(unsigned char c)
{
    int n = (head + 1) % BUFSZ;
    if (n != tail) { buf[head] = c; head = n; }
}

/* scancode set 1, after an E0 prefix: the keys that carry no ASCII */
static unsigned char ext_key(u8 sc)
{
    switch (sc) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x5B: case 0x5C: return KEY_SUPER;     /* the Windows / Super key */
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x53: return KEY_DEL;
    case 0x52: return KEY_INS;
    case 0x1C: return '\n';               /* keypad enter */
    case 0x35: return '/';
    }
    return 0;
}

/* One byte of the scancode-set-1 stream. The PS/2 interrupt feeds it from
 * port 0x60; the USB keyboard driver synthesises the same stream, so shift,
 * caps, ctrl-c and the held-key table DOOM reads all work for both. */
void kbd_scancode(u8 sc)
{

    if (sc == 0xE0) { ext = 1; return; }   /* prefix: the next byte is special */

    if (ext) {
        ext = 0;
        if (sc & 0x7F) kstate_e0[sc & 0x7F] = (sc & 0x80) ? 0 : 1;
        if (!(sc & 0x80)) {
            unsigned char c = ext_key(sc);
            if (c) push(c);
        } else {
            u8 code = sc & 0x7F;           /* the right-hand ctrl releases too */
            if (code == 0x1D) ctrl = 0;
        }
        return;
    }

    if (sc & 0x80) {                       /* key release */
        u8 code = sc & 0x7F;
        kstate[code] = 0;
        if (code == 0x2A || code == 0x36) shift = 0;
        if (code == 0x1D) ctrl = 0;
        return;
    }

    if (sc < 128) kstate[sc] = 1;          /* key press */

    switch (sc) {
    case 0x2A: case 0x36: shift = 1; return;
    case 0x1D: ctrl = 1;  return;
    case 0x3A: caps = !caps; return;
    }

    if (sc >= 0x3B && sc <= 0x44) { push((unsigned char)(KEY_F1 + (sc - 0x3B))); return; }
    if (sc >= 128) return;
    char c = (shift ^ caps) ? map_upper[sc] : map_lower[sc];
    /* caps lock only affects letters */
    if (caps && !shift) {
        char l = map_lower[sc];
        if (l >= 'a' && l <= 'z') c = (char)(l - 32);
        else c = l;
    }
    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);   /* ^A..^Z */

    /* ctrl-c is not a character when a program is running: it is a request
     * to stop it. The scheduler carries it out at the next tick, so it
     * works even on a program stuck in a loop that never reads the
     * keyboard. */
    if (c == 3 && task_sigint()) return;
    if (c) push((unsigned char)c);
}

static void kbd_isr(regs_t *r)
{
    (void)r;
    kbd_scancode(inb(0x60));
}

void kbd_init(void)
{
    head = tail = 0;
    irq_install(1, kbd_isr);
    /* unmask IRQ0 (timer) and IRQ1 (keyboard) on the master PIC */
    u8 mask = inb(0x21);
    mask &= (u8)~0x03;
    outb(0x21, mask);
    while (inb(0x64) & 1) (void)inb(0x60);   /* drain leftovers */
}


/* Does the keyboard still answer?
 *
 * Ask it to set its LEDs and wait for the acknowledgement every keyboard
 * sends. A USB keyboard that the firmware is pretending is a PS/2 one
 * answers exactly like a real one -- right up to the moment the firmware
 * stops pretending, which is precisely what this is for. Taking a USB 2.0
 * controller away from the firmware kills that pretence, and on a machine
 * whose keyboard is USB that means killing the keyboard. So: ask first,
 * ask again afterwards, and if the answer changed, undo whatever we did.
 */
int kbd_answers(void)
{
    u32 f = irq_save();

    /* let anything already in flight go through first */
    for (int i = 0; i < 5000; i++) {
        if (!(inb(0x64) & 1)) break;
        (void)inb(0x60);
        io_wait();
    }

    int ok = 1;
    for (int byte = 0; byte < 2 && ok; byte++) {
        outb(0x60, byte ? 0x00 : 0xED);      /* set LEDs, then all off   */
        int got = 0;
        for (int i = 0; i < 30000; i++) {
            if (inb(0x64) & 1) {
                u8 r = inb(0x60);
                if (r == 0xFA) { got = 1; break; }          /* acked    */
                if (r == 0xFE) outb(0x60, byte ? 0x00 : 0xED);  /* again */
            }
            io_wait();
        }
        if (!got) ok = 0;
    }
    irq_restore(f);
    return ok;
}

int kbd_poll(void)
{
    if (!task_owns_console()) return 0;   /* background tasks get no keys */
    if (head == tail) return 0;
    char c = buf[tail];
    tail = (tail + 1) % BUFSZ;
    return (int)(u8)c;
}

int kbd_getchar(void)
{
    int c;
    while (!(c = kbd_poll())) task_wait();
    return c;
}

/* PICO-FIX-kbdflush: drop every queued key and forget held keys.
 * Without this, keys pressed inside a program replay into whatever
 * runs next: quitting DOOM with arrows+enter queued once walked the
 * menu sidebar down to POWER and switched the machine off. pico_exec
 * calls this before the child starts (no stale parent keys leak in)
 * and after it ends (no stale child keys leak back out). Caps lock
 * is a toggle, not a held key, so it is deliberately preserved. */
void kbd_flush(void)
{
    u32 f = irq_save();
    head = tail = 0;
    ext = 0;
    shift = ctrl = 0;
    for (int i = 0; i < 128; i++) kstate[i] = kstate_e0[i] = 0;
    irq_restore(f);
    while (inb(0x64) & 1) (void)inb(0x60);
}

void kbd_readline(char *out, int max)
{
    int len = 0;
    for (;;) {
        int c = kbd_getchar();
        if (c == '\n' || c == '\r') { putc('\n'); break; }
        if (c == '\b') {
            if (len > 0) { len--; putc('\b'); }
            continue;
        }
        if (c == 3) {                       /* ctrl-c */
            puts("^C\n");
            len = 0;
            break;
        }
        if (c == 27) continue;              /* ignore ESC */
        if (len < max - 1 && c >= 32 && c < 127) {
            out[len++] = (char)c;
            putc((char)c);
        }
    }
    out[len] = 0;
}
