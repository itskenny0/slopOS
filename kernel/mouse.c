/* mouse.c -- the PS/2 mouse
 *
 * The mouse hangs off the same 8042 controller as the keyboard, on what the
 * chip calls the auxiliary port. That sharing is the whole difficulty. Both
 * devices deliver their bytes through port 0x60, and the only way to tell
 * whose byte you are holding is bit 5 of the status register at 0x64, which
 * you have to check *before* you read, because reading is what clears it.
 *
 * Which is why enabling the mouse is not free and not optional-in-name-only:
 * the moment the aux port starts reporting, mouse bytes begin arriving in
 * the same buffer the keyboard reads from. If nobody is expecting them, the
 * keyboard handler eats them and types garbage. So the driver is either off
 * and completely silent, or on and properly wired to IRQ12. There is no
 * halfway state, and nothing turns it on until something asks.
 *
 * That is also why it is lazy. A machine that never opens the menu never
 * enables the mouse, never allocates the packet state, and pays one byte of
 * kernel data for the flag that says so.
 *
 * Position is kept in character cells rather than pixels. PicoOS has no
 * graphics and a menu does not need any: what a click has to answer is
 * "which line of text", and that is what the console already speaks. The
 * hardware reports movement in mickeys, which are much finer than a
 * character cell, so the remainder is accumulated rather than thrown away
 * -- otherwise slow movement would never move the cursor at all.
 */

#include "pico.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUTPUT  0x01     /* there is a byte to read      */
#define ST_INPUT   0x02     /* the controller is busy       */
#define ST_AUX     0x20     /* that byte came from the mouse */

static int  present;        /* a mouse answered at boot-up time */
static int  enabled;
static u8   packet[3];
static int  phase;
static int  mx, my;         /* character cells */
static int  accx, accy;     /* leftover mickeys */
static int  buttons;
static u32  last_packet;
static int  step;           /* how far mouse_enable got, for the mouse command */

/* Pixel mode, new in 1.0: the drawing screen wants the pointer in
 * pixels, one per mickey, instead of in text cells. */
static int  pxw, pxh;       /* extent; 0 = text-cell mode */
static int  px, py;         /* pointer in pixels */

/* The controller is slow enough that we must wait for it, and broken
 * enough on some machines that we must not wait forever. */
static int wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & ST_INPUT)) return 1;
    return 0;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & ST_OUTPUT) return 1;
    return 0;
}

static void ctrl_cmd(u8 c)
{
    wait_write();
    outb(PS2_CMD, c);
}

/* Anything sent to the mouse rather than the controller needs the 0xD4
 * prefix, and answers with an acknowledgement byte we have to collect. */
static int mouse_cmd(u8 c)
{
    /* Real PS/2 devices may answer RESEND (0xFE) once while the 8042 is
     * changing the auxiliary line. Retry the complete command instead of
     * reporting a dead mouse at the reset step. */
    for (int retry = 0; retry < 3; retry++) {
        ctrl_cmd(0xD4);
        if (!wait_write()) continue;
        outb(PS2_DATA, c);
        if (!wait_read()) continue;
        u8 answer = inb(PS2_DATA);
        if (answer == 0xFA) return 1;
        if (answer != 0xFE) return 0;
    }
    return 0;
}

/* Three bytes per report: flags, then a signed X and Y delta whose sign
 * bits live back in the flags byte, because the protocol is from 1987. */
static void ps2_byte(u8 b);

/* The polled road to the same bytes -- see the note in mouse_enable(). */
static void ps2_poll(void)
{
    for (int i = 0; i < 8; i++) {
        u8 st = inb(PS2_STATUS);
        if (!(st & ST_OUTPUT) || !(st & ST_AUX)) return;
        ps2_byte(inb(PS2_DATA));
    }
}

static void ps2_byte(u8 b)
{
    /* Bit 3 of the first byte is always set. If it is not, we are out of
     * step -- so resynchronise rather than believing the packet. */
    if (phase == 0 && !(b & 0x08)) return;

    packet[phase++] = b;
    if (phase < 3) return;
    phase = 0;

    u8 flags = packet[0];
    if (flags & 0xC0) return;           /* overflow: the deltas are junk */

    int dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)packet[2] - ((flags & 0x20) ? 256 : 0);

    /* Roughly one character cell per eight mickeys sideways and per
     * sixteen vertically: cells are twice as tall as they are wide, so
     * equal hand movement should cover equal distance on screen. */
    accx += dx;
    accy += dy;

    mx += accx / 8;
    my -= accy / 16;            /* the mouse counts up, the screen counts down */
    accx %= 8;
    accy %= 16;

    int w = con_width(), h = con_height();
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= w) mx = w - 1;
    if (my >= h) my = h - 1;

    if (pxw) {                     /* the drawing screen is watching too */
        px += dx;
        py -= dy;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= pxw) px = pxw - 1;
        if (py >= pxh) py = pxh - 1;
    }

    buttons = flags & 0x07;

    last_packet = timer_ticks();
}


/* ---- USB mouse (kernel/usb.c delivers the reports here) -----------------
 *
 * A USB mouse reports relative motion in mickeys, just like the PS/2 one,
 * so it feeds the same pointer state. The difference is only where the
 * packets come from: usb.c polls the host controller and calls mouse_feed()
 * for every report. While at least one USB mouse is attached the PS/2 port
 * is left completely alone. */
static int  usb_mice;
static int  wheel_acc;

void mouse_usb_attach(int delta)
{
    u32 f = irq_save();
    usb_mice += delta;
    if (usb_mice < 0) usb_mice = 0;
    if (delta > 0) {
        /* start in the middle of whatever surface is current */
        if (pxw) { px = pxw / 2; py = pxh / 2; }
        mx = con_width() / 2; my = con_height() / 2;
        last_packet = timer_ticks() + 1;
    }
    irq_restore(f);
}

int mouse_usb_present(void) { return usb_mice > 0; }

/* Pointer acceleration: slow movement stays 1:1 for precision, fast
 * movement is amplified so a flick crosses a 1920-pixel screen. */
static int accel(int d)
{
    int a = d < 0 ? -d : d, r;
    if (a <= 2)      r = a;
    else if (a <= 6) r = a + (a - 2) / 2;
    else             r = a * 2 - 4;
    return d < 0 ? -r : r;
}

void mouse_feed(int dx, int dy, int wheel, int btn)
{
    int adx = accel(dx), ady = accel(dy);

    if (pxw) {
        px += adx;
        py += ady;                     /* USB reports y down = screen down */
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= pxw) px = pxw - 1;
        if (py >= pxh) py = pxh - 1;
    }
    accx += dx;
    accy += dy;
    mx += accx / 8;
    my += accy / 16;
    accx %= 8;
    accy %= 16;
    {
        int w = con_width(), h = con_height();
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= w) mx = w - 1;
        if (my >= h) my = h - 1;
    }
    wheel_acc += wheel;
    buttons = btn & 7;
    last_packet = timer_ticks() + 1;
}

int mouse_wheel_take(void)
{
    u32 f = irq_save();
    int w = wheel_acc;
    wheel_acc = 0;
    irq_restore(f);
    return w;
}

/* Give the keyboard its port back before admitting defeat: a failed mouse
 * probe must never leave the machine unable to type. */
static int bail(u32 iflag)
{
    ctrl_cmd(0xAE);
    irq_restore(iflag);
    return 0;
}

/* Turn it on. Safe to call twice; returns 0 if there is nothing there. */
int mouse_enable(void)
{
    if (usb_mice) return 1;           /* USB mouse: leave PS/2 alone */
    if (enabled) return 1;

    /* The whole handshake has to happen with interrupts off and the
     * keyboard port shut.
     *
     * Ask the controller for its configuration byte and it puts the answer
     * in the same output buffer the keyboard uses -- and raises IRQ1 doing
     * it. The keyboard handler then wakes up, reads port 0x60, and swallows
     * the answer. The polling loop here waits for a byte that a different
     * piece of the kernel already ate, times out, and reports "no mouse" on
     * a machine that has one. That is exactly what happened, and it is why
     * the menu said "keyboard only" under an emulator with a mouse plugged
     * in.
     *
     * So: interrupts off for the duration, keyboard port disabled with
     * 0xAD, and the buffer drained first in case something is left in it. */
    u32 iflag = irq_save();
    ctrl_cmd(0xAD);                     /* keyboard port off, briefly */
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & ST_OUTPUT); i++)
        (void)inb(PS2_DATA);            /* drain whatever is stale */

    step = 1;
    ctrl_cmd(0xA8);                     /* enable the auxiliary port */

    /* Set bit 1 of the configuration byte so IRQ12 actually fires, and
     * clear bit 5 so the aux clock is not held disabled. */
    ctrl_cmd(0x20);
    step = 2;
    if (!wait_read()) return bail(iflag);
    u8 cfg = inb(PS2_DATA);
    cfg |= (1 << 1);
    cfg &= (u8)~(1 << 5);
    ctrl_cmd(0x60);
    wait_write();
    outb(PS2_DATA, cfg);

    step = 3;
    if (!mouse_cmd(0xF6)) return bail(iflag);   /* restore defaults */
    step = 4;
    if (!mouse_cmd(0xF4)) return bail(iflag);   /* start reporting  */
    step = 5;

    ctrl_cmd(0xAE);                     /* keyboard port back on */

    /* No IRQ12: the mouse is polled (ps2_poll). An interrupt-driven
     * mouse proved to upset machines that had just switched video modes,
     * and a poll every frame is more than a pointer needs anyway.
     * The keyboard keeps its interrupt. */

    mx = con_width() / 2;
    my = con_height() / 2;
    accx = accy = 0;
    phase = 0;
    buttons = 0;

    present = enabled = 1;
    irq_restore(iflag);
    return 1;
}

void mouse_disable(void)
{
    if (!enabled) return;
    mouse_cmd(0xF5);                    /* stop reporting */
    outb(0xA1, (u8)(inb(0xA1) | (1 << 4)));
    enabled = 0;
}

int mouse_present(void) { return present; }
int mouse_step(void)    { return step; }

/* Pixels or cells: the drawing screen calls this when it takes over. */
void mouse_pixel_mode(int w, int h)
{
    pxw = w > 0 ? w : 0;
    pxh = h > 0 ? h : 0;
    px  = pxw / 2;
    py  = pxh / 2;
    wheel_acc = 0;
}

int mouse_get_px(int *x, int *y, int *btn)
{
    if (!pxw) return 0;
    if (usb_mice) {
        usb_service();
        u32 f = irq_save();
        if (x)   *x = px;
        if (y)   *y = py;
        if (btn) *btn = buttons;
        irq_restore(f);
        return 1;
    }
    usb_service();                       /* maybe one was just plugged in */
    if (!enabled) return 0;
    ps2_poll();
    u32 f = irq_save();
    if (x)   *x = px;
    if (y)   *y = py;
    if (btn) *btn = buttons;
    irq_restore(f);
    return 1;
}

/* Returns 1 if there is a mouse, and fills in wherever it is now. */
int mouse_get(int *x, int *y, int *btn)
{
    if (usb_mice) {
        usb_service();
        u32 f = irq_save();
        if (x)   *x = mx;
        if (y)   *y = my;
        if (btn) *btn = buttons;
        irq_restore(f);
        return 1;
    }
    usb_service();
    if (!enabled) return 0;
    ps2_poll();               /* no interrupt: we ask, it answers */
    u32 f = irq_save();
    if (x)   *x = mx;
    if (y)   *y = my;
    if (btn) *btn = buttons;
    irq_restore(f);
    return 1;
}

/* Has it produced anything at all since being switched on? A mouse that is
 * enabled but silent is a machine with no mouse plugged in, and the menu
 * should not draw a pointer that can never move. */
int mouse_alive(void)
{
    if (usb_mice) return 1;
    usb_service();
    if (usb_mice) return 1;
    if (enabled) ps2_poll();
    return enabled && last_packet != 0;
}

/* Did the PS/2 auxiliary port itself answer the handshake? This is the
 * only mouse PicoOS drives: a USB mouse works through the firmware's
 * PS/2 emulation, which needs no driver at all. */
int mouse_ps2(void)
{
    return enabled;
}
