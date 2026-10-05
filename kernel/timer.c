#include "pico.h"

static volatile u32 ticks = 0;
static u32 hz_rate = 100;

static void timer_isr(regs_t *r) { (void)r; ticks++; usb_tick(); task_tick(); }

void timer_init(u32 hz)
{
    hz_rate = hz;
    u32 div = 1193180 / hz;
    outb(0x43, 0x36);                       /* channel 0, lo/hi, mode 3 */
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
    irq_install(0, timer_isr);
}

u32 timer_ticks(void) { return ticks; }
u32 timer_ms(void)    { return (ticks * 1000) / hz_rate; }

/* Sleeping is not halting any more: we hand the CPU to whoever else wants
 * it, and only really halt when nobody does. */
void sleep_ms(u32 ms)
{
    u32 target = ticks + (ms * hz_rate) / 1000 + 1;
    while (ticks < target) task_wait();
}
