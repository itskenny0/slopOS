#include "pico.h"

/* ---------------- GDT (our own, the bootloader's one is temporary) ------- */
struct gdt_entry {
    u16 limit_low, base_low;
    u8  base_mid, access, gran, base_high;
} __attribute__((packed));

struct gdt_ptr { u16 limit; u32 base; } __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtp;

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[i].base_low   = (u16)(base & 0xFFFF);
    gdt[i].base_mid   = (u8)((base >> 16) & 0xFF);
    gdt[i].base_high  = (u8)((base >> 24) & 0xFF);
    gdt[i].limit_low  = (u16)(limit & 0xFFFF);
    gdt[i].gran       = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access     = access;
}

static void gdt_init(void)
{
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (u32)&gdt;
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0x000FFFFF, 0x9A, 0xC0);   /* code */
    gdt_set(2, 0, 0x000FFFFF, 0x92, 0xC0);   /* data */
    __asm__ volatile(
        "lgdt %0\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :: "m"(gdtp) : "eax", "memory");
}

/* ---------------- IDT ---------------- */
struct idt_entry {
    u16 base_low;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_high;
} __attribute__((packed));

struct idt_ptr { u16 limit; u32 base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;
static isr_handler_t    handlers[256];

extern void idt_flush(u32);

/* put the kernel IDT back after a trip to real mode (see vgaintr.S) */
void idt_load(void);
void idt_load(void) { idt_flush((u32)&idtp); }

/* stubs from isr.S */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static void idt_set(int n, u32 base, u16 sel, u8 flags)
{
    idt[n].base_low  = (u16)(base & 0xFFFF);
    idt[n].base_high = (u16)((base >> 16) & 0xFFFF);
    idt[n].sel   = sel;
    idt[n].zero  = 0;
    idt[n].flags = flags;
}

static void pic_remap(void)
{
    outb(0x20, 0x11); io_wait();      /* start init, cascade */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();      /* master vectors -> 0x20 */
    outb(0xA1, 0x28); io_wait();      /* slave  vectors -> 0x28 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();      /* 8086 mode */
    outb(0xA1, 0x01); io_wait();

    /* Every interrupt off, except the three this kernel actually answers:
     * the timer, the keyboard, and the cascade to the second controller.
     * Anything else is unmasked only by the driver that claims it -- the
     * mouse opens IRQ12 when it is switched on, and nothing else ever
     * asks.
     *
     * Leaving the firmware's mask in place, which is what this did until
     * now, costs nothing in an emulator and everything on a real machine.
     * A controller nobody is driving -- a USB one with a mouse just
     * plugged into it, say -- asserts an interrupt, and an interrupt no
     * handler claims is an interrupt the hardware keeps holding up. It is
     * acknowledged and immediately re-delivered, for as long as the
     * device feels like it, and the machine stops doing anything else.
     * The keyboard, having a lower interrupt number, is not even the last
     * thing to go, but it is the one you notice: it is dead, and so is
     * everything around it, and nothing was printed anywhere.
     *
     * No driver here needs the mask the firmware left: the network cards
     * are polled, the clock is read from the CMOS, and shutting down is a
     * register write. */
    outb(0x21, 0xF8);                 /* master: IRQ 0, 1 and 2 open   */
    outb(0xA1, 0xFF);                 /* slave:  all shut              */
}

static const char *exc_name[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization exception",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor injection", "VMM communication",
    "Security exception", "Reserved"
};

void int_dispatch(regs_t *r)
{
    if (r->int_no < 32) {
        if (handlers[r->int_no]) { handlers[r->int_no](r); return; }

        /* If a .pico program caused this, report it and hand control back
         * to the shell rather than taking the whole machine down. */
        if (pico_running()) {
            con_setcolor(YELLOW, BLACK);
            kprintf("\n  %s faulted: %s at eip=%p (err=%p)\n",
                    pico_running(), exc_name[r->int_no], r->eip, r->err_code);
            con_setcolor(LGREY, BLACK);
            pico_fault();               /* does not return */
        }

        con_setcolor(WHITE, RED);
        kprintf("\n*** CPU EXCEPTION %d: %s ***\n",
                r->int_no, exc_name[r->int_no]);
        kprintf("eip=%p  err=%p  eax=%p  ebx=%p\n",
                r->eip, r->err_code, r->eax, r->ebx);
        kprintf("ecx=%p  edx=%p  esi=%p  edi=%p\n",
                r->ecx, r->edx, r->esi, r->edi);
        kprintf("System halted.\n");
        for (;;) { cli(); hlt(); }
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = (int)r->int_no - 32;
        if (handlers[r->int_no]) handlers[r->int_no](r);
        if (irq >= 8) outb(0xA0, 0x20);   /* EOI to slave */
        outb(0x20, 0x20);                 /* EOI to master */

        /* Only now, with the interrupt controller satisfied, is it safe to
         * stand on a different task's stack. Doing it any earlier would
         * carry the un-acknowledged interrupt into the next task and wedge
         * the timer for good. */
        task_resched();
    }
}

void isr_install(int n, isr_handler_t h) { handlers[n] = h; }
void irq_install(int irq, isr_handler_t h) { handlers[32 + irq] = h; }

void idt_init(void)
{
    gdt_init();
    pic_remap();

    for (int i = 0; i < 256; i++) { idt_set(i, 0, 0, 0); handlers[i] = NULL; }

    void *stubs[48] = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (int i = 0; i < 48; i++)
        idt_set(i, (u32)stubs[i], 0x08, 0x8E);   /* present, ring0, 32-bit gate */

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (u32)&idt;
    idt_flush((u32)&idtp);
}
