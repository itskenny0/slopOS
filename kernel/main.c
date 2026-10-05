#include "pico.h"
#include "net.h"
#include "usb.h"

bootinfo_t *bootinfo(void)
{
    if (!boot_info_ptr) return NULL;
    bootinfo_t *bi = (bootinfo_t *)boot_info_ptr;
    return bi->magic == BOOTINFO_MAGIC ? bi : NULL;
}

void reboot(void)
{
    /* pulse the reset line via the 8042 keyboard controller */
    u8 tmp;
    cli();
    do {
        tmp = inb(0x64);
        if (tmp & 1) (void)inb(0x60);
    } while (tmp & 2);
    outb(0x64, 0xFE);
    /* fall back to a triple fault */
    __asm__ volatile("lidt %0; int $3" :: "m"(*(char *)0));
    for (;;) hlt();
}

void shutdown(void)
{
    /* The real thing first. On a machine built after about 1999 this is the
     * only one of the four that does anything, and it is the only one that
     * actually cuts the power rather than parking the processor. */
    acpi_poweroff();

    /* Then the ports the emulators watch. QEMU, Bochs and VirtualBox each
     * picked a different one, and none of them exist on real hardware. */
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    /* Nothing worked: say so rather than leaving a dead-looking screen, and
     * park the processor cold so the fans at least wind down. */
    con_setcolor(YELLOW, BLACK);
    puts("\n  This machine will not power off under software control.\n");
    con_setcolor(LGREY, BLACK);
    puts("  It is safe to switch it off now.\n");
    cli();
    for (;;) hlt();
}

void panic(const char *msg)
{
    con_setcolor(WHITE, RED);
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    cli();
    for (;;) hlt();
}

static void boot_line(const char *what, const char *detail)
{
    con_setcolor(LGREY, BLACK);
    puts("  [");
    con_setcolor(LGREEN, BLACK);
    puts("ok");
    con_setcolor(LGREY, BLACK);
    puts("] ");
    puts(what);
    for (size_t i = strlen(what); i < 22; i++) putc(' ');
    con_setcolor(DGREY, BLACK);
    puts(detail);
    putc('\n');
    con_setcolor(LGREY, BLACK);
}

void kmain(void)
{
    serial_init();
    con_init();


    bootinfo_t *bi = bootinfo();
    int uefi = (bi && bi->firmware == FW_UEFI);

    con_setcolor(LGREEN, BLACK);
    puts("\n"
    "   ___  _         ___  ___ \n"
    "  | _ \\(_) __ ___ / _ \\/ __|\n"
    "  |  _/| |/ _/ _ \\ (_) \\__ \\\n"
    "  |_|  |_|\\__\\___/\\___/|___/   v2.1\n\n");
    con_setcolor(LGREY, BLACK);

    {
        char b[64], t[16];
        strcpy(b, uefi ? "UEFI, " : "BIOS, ");
        strcpy(b + 6, con_driver());
        size_t l = strlen(b);
        strcpy(b + l, " ");
        itoa(con_width(), t, 10);  strcpy(b + l + 1, t);
        l = strlen(b); strcpy(b + l, "x");
        itoa(con_height(), t, 10); strcpy(b + l + 1, t);
        boot_line("firmware", b);
    }

    idt_init();    boot_line("interrupts",  "GDT + IDT + PIC remapped");
    cpu_init();    boot_line("processor",  cpu_name());
    mem_init();

    char d[48], num[16];
    utoa(mem_total() / 1024, num, 10);
    strcpy(d, num); strcpy(d + strlen(d), " KB heap");
    boot_line("memory", d);

    timer_init(100);
    boot_line("timer", "PIT @ 100 Hz");

    kbd_init();
    boot_line("keyboard", "PS/2, scancode set 1");

    con_shadow_init();      /* the heap exists now: stop reading video memory */

    acpi_init();
    boot_line("power", acpi_available() ? "ACPI S5 soft-off"
                                        : "no ACPI, halt only");

    fs_init();
    store_init();
    {
        const char *t =
            "Welcome to PicoOS!\n"
            "This whole OS is a bootloader in assembly plus a C kernel.\n"
            "Type 'help' to see what it can do.\n";
        fs_write("readme.txt", t, strlen(t));
        const char *m = "PicoOS " PICOOS_VERSION
                          " - built from scratch, no Linux inside.\n";
        fs_write("motd.txt", m, strlen(m));
        {
            char cpuinfo[192];
            cpu_report(cpuinfo, sizeof(cpuinfo));
            fs_write("cpuinfo.txt", cpuinfo, strlen(cpuinfo));
        }
        const char *demo =
            "/* cc demo -- press F5 to compile and run */\n"
            "int i;\n"
            "int total;\n"
            "\n"
            "int twice(int n)\n"
            "{\n"
            "    return n + n;\n"
            "}\n"
            "\n"
            "void main(void)\n"
            "{\n"
            "    puts(\"the machine compiling its own C\\n\");\n"
            "    for (i = 1; i <= 5; i++) {\n"
            "        total += twice(i);\n"
            "        puts(\"added \");\n"
            "        putc(48 + twice(i));\n"
            "        putc(10);\n"
            "    }\n"
            "    beep(440, 120);\n"
            "    puts(\"total: \");\n"
            "    putc(48 + total / 10);\n"
            "    putc(48 + total % 10);\n"
            "    putc(10);\n"
            "}\n";
        fs_write("CC.C", demo, strlen(demo));
    }
    apps_install();               /* into the store, packed, no heap */
    papp_install();               /* and anything added after the build */
    {
        char msg[48], num[12];
        itoa(fs_count(), num, 10);
        strcpy(msg, num);
        strcpy(msg + strlen(msg), " files");
        boot_line("ramdisk", msg);

        u32 packed = store_packed_bytes(), orig = store_orig_bytes();
        itoa(store_count(), num, 10);
        strcpy(msg, num);
        strcpy(msg + strlen(msg), " programs, ");
        itoa((int)packed, num, 10);
        strcpy(msg + strlen(msg), num);
        strcpy(msg + strlen(msg), " B packed (was ");
        itoa((int)orig, num, 10);
        strcpy(msg + strlen(msg), num);
        strcpy(msg + strlen(msg), ")");
        boot_line("store", msg);

        if (papp_size()) {
            char m2[40];
            itoa(papp_count(), num, 10);
            strcpy(m2, num);
            strcpy(m2 + strlen(m2), " added, ");
            itoa((int)((papp_size() + 1023) / 1024), num, 10);
            strcpy(m2 + strlen(m2), num);
            strcpy(m2 + strlen(m2), " KB behind the kernel");
            boot_line("app archive", m2);
        }
    }

    /* The cards have to be found with interrupts on: a chip that is
     * settling after a reset is waited for, and waiting is an interrupt. */
    sti();

    usb_init();
    {
        char msg[72];
        if (usb_controller_count() == 0) strcpy(msg, "no USB host controller found");
        else usb_summary(msg, sizeof msg);
        boot_line("usb", msg);
    }

    net_init();
    {
        char msg[64], num[12];
        if (net_count() == 0) {
            strcpy(msg, "no network card found");
        } else {
            char mbuf[18];
            net_fmt_mac(net_at(0)->mac, mbuf);
            strcpy(msg, net_at(0)->name);
            strcpy(msg + strlen(msg), " ");
            strcpy(msg + strlen(msg), mbuf);
            if (net_count() > 1) {
                itoa(net_count(), num, 10);
                strcpy(msg + strlen(msg), " (+");
                strcpy(msg + strlen(msg), num);
                strcpy(msg + strlen(msg), " more)");
            }
        }
        boot_line("network", msg);
    }

    task_init();
    boot_line("scheduler", "pre-emptive, round-robin, 100 Hz");

    putc('\n');

    /* Boot straight into the graphical Menu. There is no USB setup wizard:
     * input is PS/2 (or USB devices emulated as PS/2 by the firmware),
     * which works immediately with no countdown and no waiting. */
    if (!fs_find("menu.pico")) store_extract("menu.pico");
    pico_exec("menu.pico", 0, 0);
    con_setcolor(WHITE, BLACK);
    puts("  Graphical menu ended. Type 'help' for commands.\n\n");
    con_setcolor(LGREY, BLACK);

    shell_run();

    panic("shell returned");
}
