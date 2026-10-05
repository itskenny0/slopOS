#include "pico.h"
#include "usb.h"
#include "net.h"

#define MAXARGS 12
#define LINESZ  200

/* A shell opened from the graphical menu returns here on `exit`, rather
 * than terminating the menu's task. The boot shell still behaves normally. */
static int shell_leave;
static int shell_nested;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAXARGS) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    return argc;
}

/* rest of the line starting at argument i, spaces preserved */
static const char *rest(char *line, char **argv, int argc, int i)
{
    (void)line; (void)argc;
    if (i >= argc) return "";
    char *s = argv[i];
    /* the splitter turned separators into NULs; stitch them back */
    for (int j = i; j < argc - 1; j++) {
        char *e = argv[j] + strlen(argv[j]);
        while (e < argv[j + 1]) *e++ = ' ';
    }
    return s;
}

/* start <program> [args...] -- run a .pico executable
 *
 * ".pico" is optional, so `start tetris` and `start tetris.pico` are the
 * same thing. Everything after the name is handed to the program as argv,
 * exactly like a shell on a grown-up OS. */
/* `start x &` and `bg x` are the same thing; the second exists because &
 * is an awkward key on half the keyboards in the world. */
/* ------------------------------------------------------------------ *
 * A pager.
 *
 * `help` is longer than a 25-line screen and always has been, so the top
 * of it scrolled away before you could read it. Now anything that prints
 * more than a screenful stops and waits.
 *
 * The line count is reset by whoever starts the output, not by the pager,
 * because a command that has already printed a heading has already used a
 * line and the pager cannot know that.
 * ------------------------------------------------------------------ */
static int page_lines;

static void page_reset(void) { page_lines = 0; }

static void page_puts(const char *str)
{
    for (const char *p = str; *p; p++) {
        putc(*p);
        if (*p != '\n') continue;

        if (++page_lines < con_height() - 2) continue;

        con_setcolor(BLACK, LGREY);
        puts(" more -- space for the next page, q to stop ");
        con_setcolor(LGREY, BLACK);

        int k = kbd_getchar();
        putc('\r');
        for (int i = 0; i < 46; i++) putc(' ');
        putc('\r');

        page_lines = 0;
        if (k == 'q' || k == 'Q' || k == 3 || k == 27) return;
    }
}


/* usb: what the USB drivers found */
static void cmd_usb(void)
{
    char b[96];
    usb_summary(b, sizeof b);
    kprintf("  %s\n", b);
    if (usb_controller_count() == 0)
        kprintf("  no USB host controller found on the PCI bus\n");
    for (int i = 0; usb_devlog_line(i); i++)
        kprintf("    %s\n", usb_devlog_line(i));
    if (usb_mouse_count() == 0)
        kprintf("  no USB mouse attached (a PS/2 mouse still works)\n");
    kprintf("\n");
}

static void cmd_start(int argc, char **argv, int background)
{
    if (argc < 2) {
        puts("  usage: start <program> [arguments]   (add & to run it behind)\n");
        puts("  try:   start tetris\n");
        return;
    }

    /* a trailing & means: start it and give me my prompt back */
    if (!strcmp(argv[argc - 1], "&")) { background = 1; argc--; }
    else {
        char *last = argv[argc - 1];
        int  l = (int)strlen(last);
        if (l > 1 && last[l - 1] == '&') { background = 1; last[l - 1] = 0; }
    }
    if (argc < 2) { puts("  usage: start <program> [arguments] [&]\n"); return; }

    char name[FS_NAMELEN];
    strncpy(name, argv[1], FS_NAMELEN - 1);
    name[FS_NAMELEN - 1] = 0;

    /* A game you wrote is a .bas file, and `start game.bas` should run it
     * rather than complain that it is not a program. Hand it to the game
     * maker, which knows what to do with it. */
    {
        int l = (int)strlen(name);
        if (l > 4 && !strcmp(name + l - 4, ".bas")) {
            char *av[3];
            av[0] = (char *)"make";
            av[1] = (char *)"run";
            av[2] = name;
            if (!fs_find("make.pico")) store_extract("make.pico");
            int e = background ? pico_bg("make.pico", 3, av)
                               : pico_exec("make.pico", 3, av);
            if (e == PICO_ENOFILE) puts("  no game maker on this machine\n");
            return;
        }
    }

    /* add the extension when the user left it off */
    int n = (int)strlen(name);
    int has_ext = (n > 5 && !strcmp(name + n - 5, ".pico"));
    if (!has_ext && n < FS_NAMELEN - 6) {
        strcpy(name + n, ".pico");
        /* fall back to the bare name only if neither disk nor store has it */
        if (!fs_find(name) && !store_find(name)) name[n] = 0;
    }

    /* Not on the ramdisk but sitting packed in the store: unpack it and say
     * nothing. Someone typing `start tetris` wants tetris, not a lecture on
     * where it was being kept. */
    if (!fs_find(name) && store_find(name)) {
        int e = store_extract(name);
        if (e < 0) {
            con_setcolor(LRED, BLACK);
            kprintf("  %s: could not be unpacked (%d)\n", name, e);
            con_setcolor(LGREY, BLACK);
            return;
        }
    }

    if (background) {
        int pid = pico_bg(name, argc - 1, argv + 1);
        if (pid < 0) {
            con_setcolor(LRED, BLACK);
            kprintf("  %s: %s\n", name, pico_error(pid));
            con_setcolor(LGREY, BLACK);
        } else {
            kprintf("  [%d] %s running in the background\n", pid, name);
        }
        return;
    }

    /* packed in the store, never run before? unpack it and try again,
     * the same right api->run gives a program: naming it runs it */
    int r = pico_exec(name, argc - 1, argv + 1);
    if (r == PICO_ENOFILE && !fs_find(name) && store_extract(name) >= 0)
        r = pico_exec(name, argc - 1, argv + 1);

    if (r == -101) {
        con_setcolor(YELLOW, BLACK);
        kprintf("  %s stopped\n", name);
        con_setcolor(LGREY, BLACK);
    } else if (r == PICO_ENOFILE) {
        con_setcolor(LRED, BLACK);
        kprintf("  %s: no such program\n", name);
        con_setcolor(LGREY, BLACK);
    } else if (r == -100) {
        con_setcolor(LRED, BLACK);
        kprintf("  %s was killed after a fault\n", name);
        con_setcolor(LGREY, BLACK);
    } else if (r < 0 && r >= -20) {
        con_setcolor(LRED, BLACK);
        kprintf("  %s: %s\n", name, pico_error(r));
        con_setcolor(LGREY, BLACK);
    } else if (r > 0) {
        kprintf("  %s exited with code %d\n", name, r);
    }
}

/* right-align a number in a field, so columns line up */
static void pad_num(u32 v, int width)
{
    u32 t = v; int digits = 1;
    while (t >= 10) { t /= 10; digits++; }
    for (int i = digits; i < width; i++) putc(' ');
    kprintf("%u", v);
}

/* mouse -- switch the pointer on and say what the hardware did about it.
 * The mouse is the one piece of optional hardware PicoOS talks to, so when
 * a machine has one and the menu says "keyboard only", this is where you
 * find out which step of the 8042 handshake the controller refused.
 * USB mice work through the firmware's PS/2 emulation: no driver needed. */

/* ------------------------------------------------------------------ *
 * The network.
 *
 * Three commands that cover what a machine this size actually does
 * with a network: ask for an address, see if something is there, and
 * fetch something. `wget` writes straight onto the RAM disk, so a game
 * downloaded is a game you can start.
 * ------------------------------------------------------------------ */

static void cmd_net(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "ne2000")) {
        u32 io = argc > 2 ? (u32)atoi(argv[2]) : 0x300;
        int i = ne2000_init(io);
        if (i < 0) {
            kprintf("\n  nothing like an ne2000 answered at 0x%x\n", io);
            puts("  the usual addresses are 0x300, 0x320, 0x340, 0x360 and 0x280,\n"
                 "  set by jumpers on the card itself.\n\n");
        } else {
            char m[18];
            net_fmt_mac(net_at(i)->mac, m);
            kprintf("\n  ne2000 at 0x%x, %s\n\n", io, m);
        }
        return;
    }

    if (net_count() == 0) {
        puts("\n  no network card on the pci bus\n"
             "  on an isa machine try `net ne2000 0x300`\n"
             "  `net` again afterwards to see it\n\n");
        return;
    }

    if (argc > 1 && !strcmp(argv[1], "diag")) {
        putc('\n');
        for (int i = 0; i < net_count(); i++) {
            netif_t *n = net_at(i);
            char m[18];
            net_fmt_mac(n->mac, m);
            kprintf("  %s %s irq %d\n", n->name, m, n->irq);
            if (n->diag) n->diag(n);
        }
        putc('\n');
        return;
    }

    putc('\n');
    for (int i = 0; i < net_count(); i++) {
        netif_t *n = net_at(i);
        char m[18], a[16];
        net_fmt_mac(n->mac, m);
        kprintf("  [%d] %s  %s  %s", i + 1, n->name, m,
                n->link ? "link up" : "no link");
        if (n->speed_mbps) kprintf(", %u Mb/s", n->speed_mbps);
        putc('\n');

        if (!n->ip) {
            puts("      no address -- `dhcp` asks the router for one\n");
            continue;
        }
        net_fmt_ip(n->ip, a);   kprintf("      ip %s", a);
        net_fmt_ip(n->mask, a); kprintf("  mask %s", a);
        net_fmt_ip(n->gw, a);   kprintf("\n      gateway %s", a);
        net_fmt_ip(n->dns, a);  kprintf("  dns %s", a);
        kprintf("\n      %u packets in, %u out\n",
                n->rx_packets, n->tx_packets);
    }
    putc('\n');
}

static void cmd_dhcp(void)
{
    netif_t *n = net_active();
    if (!n || !n->present) { puts("\n  no network card\n\n"); return; }

    puts("\n  asking the router for an address...\n");
    if (net_dhcp(n) < 0) {
        con_setcolor(YELLOW, BLACK);
        puts("  nobody answered.\n");
        con_setcolor(LGREY, BLACK);
        kprintf("\n  the card tried %u packet%s, %u actually left, "
                "and %u came back.\n",
                n->tx_packets, n->tx_packets == 1 ? "" : "s",
                n->tx_ok, n->rx_packets);
        if (!n->tx_ok)
            puts("  nothing left the card at all -- check the cable, and\n"
                 "  the link light on the socket it sits in.\n");
        else if (!n->rx_packets)
            puts("  packets left but nothing came back -- the router replies\n"
                 "  to an address this card is not listening on, or the\n"
                 "  reply does not arrive. `net diag` prints the address.\n");
        else
            puts("  traffic both ways, but no address -- this router does\n"
                 "  not hand addresses to machines it does not know.\n"
                 "\n  no problem. type:\n\n     ip auto\n\n"
                 "  and I will read the network off the cable myself.\n");
        putc('\n');
        return;
    }
    char a[16];
    net_fmt_ip(n->ip, a);   kprintf("  ip      %s\n", a);
    net_fmt_ip(n->mask, a); kprintf("  netmask %s\n", a);
    net_fmt_ip(n->gw, a);   kprintf("  gateway %s\n", a);
    net_fmt_ip(n->dns, a);  kprintf("  dns     %s\n\n", a);
}

/* What is actually arriving? A card can receive perfectly well and still
 * be told it hears nothing, if what it hears is not what the program
 * asked for. This shows the frames themselves: how big they are, what
 * they say they are, who sent them, and -- the one that settles arguments
 * about DHCP -- which port they are aimed at. */
static int  sniff_wanted;

static void sniff_show(const u8 *pkt, u32 len)
{
    if (sniff_wanted > 0) sniff_wanted--;      /* one fewer to wait for */
    if (len < 14) {
        kprintf("  %u bytes  too short for a frame\n", len);
        return;
    }

    u16 type = (u16)((pkt[12] << 8) | pkt[13]);
    const char *name = type == 0x0800 ? "ip"
                     : type == 0x0806 ? "arp"
                     : type == 0x86DD ? "ipv6"
                     : type == 0x8100 ? "vlan"
                     :                  "?";

    kprintf("  %u bytes  %s  from %02x:%02x:%02x:%02x:%02x:%02x",
            len, name, pkt[6], pkt[7], pkt[8], pkt[9], pkt[10], pkt[11]);

    if (type == 0x0806 && len >= 42) {
        /* who is talking, and about whom: this is what the autonet code
         * reads the shape of the network off */
        kprintf("\n           %s %u.%u.%u.%u  ->  %u.%u.%u.%u",
                (pkt[21] == 2) ? "arp reply " : "arp ask   ",
                pkt[28], pkt[29], pkt[30], pkt[31],
                pkt[38], pkt[39], pkt[40], pkt[41]);
    }

    if (type == 0x0800 && len >= 34) {
        u8  proto = pkt[23];
        kprintf("\n           ip %u.%u.%u.%u -> %u.%u.%u.%u  proto %u",
                pkt[26], pkt[27], pkt[28], pkt[29],
                pkt[30], pkt[31], pkt[32], pkt[33], proto);
        if (proto == 17 && len >= 38) {
            u16 sp = (u16)((pkt[34] << 8) | pkt[35]);
            u16 dp = (u16)((pkt[36] << 8) | pkt[37]);
            kprintf("  udp %u -> %u", sp, dp);
            if (sp == 67 || dp == 67 || sp == 68 || dp == 68)
                kprintf("   <-- dhcp");
        }
        if (proto == 1) kprintf("  icmp");
        if (proto == 6) kprintf("  tcp");
    }
    putc('\n');
}

static void cmd_sniff(int argc, char **argv)
{
    netif_t *n = net_active();
    if (!n || !n->present) { puts("\n  no network card\n\n"); return; }

    int want = (argc > 1) ? atoi(argv[1]) : 8;
    if (want < 1)   want = 8;
    if (want > 40)  want = 40;

    kprintf("\n  listening for %d packet%s, thirty seconds at most\n\n",
            want, want == 1 ? "" : "s");

    sniff_wanted = want;
    net_tap(sniff_show, want);

    u32 t0 = timer_ms();
    while (sniff_wanted > 0 && (u32)(timer_ms() - t0) < 30000)
        net_pump(100);

    net_tap(0, 0);
    putc('\n');
}

/* Set the address by hand.
 *
 * Everything on this machine has assumed, up to now, that an address is
 * something a router gives you. When it does not -- and a router is
 * perfectly entitled not to -- there was no way at all to use the network,
 * because there was no way to say what address to use. So: say it. */
static void cmd_ip(int argc, char **argv)
{
    netif_t *n = net_active();
    if (!n || !n->present) { puts("\n  no network card\n\n"); return; }

    if (argc < 2) {
        char a[16], m[16], g[16], d[16];
        net_fmt_ip(n->ip, a);   net_fmt_ip(n->mask, m);
        net_fmt_ip(n->gw, g);   net_fmt_ip(n->dns, d);
        kprintf("\n  ip      %s\n  netmask %s\n  gateway %s\n  dns     %s\n\n",
                n->ip ? a : "(none)", m, g, d);
        return;
    }

    /* `ip auto`: read an address off the cable. No router needed. */
    if (!strcmp(argv[1], "auto")) {
        if (net_autonet(n) == 0) {
            char a[16], m[16], g[16], d[16];
            net_fmt_ip(n->ip, a);   net_fmt_ip(n->mask, m);
            net_fmt_ip(n->gw, g);   net_fmt_ip(n->dns, d);
            kprintf("\n  ip      %s\n  netmask %s\n  gateway %s\n"
                    "  dns     %s\n\n", a, m, g, d);
            puts("  try it:  web example.com\n\n");
        } else
            putc('\n');
        return;
    }

    u32 ip = net_ip(argv[1]);
    if (!ip) { puts("\n  that is not an address\n\n"); return; }

    u32 mask = (argc > 2) ? net_ip(argv[2]) : 0xFFFFFF00u;   /* 255.255.255.0 */
    u32 gw   = (argc > 3) ? net_ip(argv[3]) : ((ip & mask) | 1u);
    u32 dns  = (argc > 4) ? net_ip(argv[4]) : gw;

    n->ip   = ip;
    n->mask = mask;
    n->gw   = gw;
    n->dns  = dns;

    char a[16], m[16], g[16];
    net_fmt_ip(ip, a); net_fmt_ip(mask, m); net_fmt_ip(gw, g);
    kprintf("\n  set by hand:  %s  netmask %s  gateway %s\n", a, m, g);
    puts("  try the gateway:  ping <gateway>\n");
    puts("  to fetch anything you also need a dns address:\n"
         "     ip <address> <netmask> <gateway> <dns>\n\n");
}

static void cmd_ping(int argc, char **argv)
{
    if (argc < 2) { puts("\n  ping <address or name> [count]\n\n"); return; }
    if (net_need_ip() < 0) {
        puts("\n  no network card, or no address. run `ip auto` first\n\n");
        return;
    }

    u32 ip = net_ip(argv[1]);
    if (!ip) {
        kprintf("\n  looking up %s...\n", argv[1]);
        if (net_dns(argv[1], &ip, 4000) < 0) {
            puts("  no answer from the name server\n\n");
            return;
        }
        char a[16];
        net_fmt_ip(ip, a);
        kprintf("  %s is %s\n", argv[1], a);
    }

    int times = argc > 2 ? atoi(argv[2]) : 3;
    if (times < 1) times = 1;
    if (times > 10) times = 10;

    int answered = 0;
    for (int i = 0; i < times; i++) {
        int rtt = net_ping(ip, 2000);
        if (rtt > 0) {
            kprintf("  answer in %d ms\n", rtt);
            answered++;
        } else {
            puts("  no answer\n");
        }
        if (i + 1 < times) sleep_ms(300);
    }
    kprintf("  %d of %d answered\n\n", answered, times);
}

/* the file name when you do not give one: whatever is behind the last
 * slash, cut down to the fifteen characters the RAM disk allows */
static void wget_name(const char *url, char *out)
{
    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    const char *last = NULL;
    for (const char *q = p; *q; q++) if (*q == '/') last = q;

    const char *start = (last && last[1]) ? last + 1 : p;
    if (!*start || (last && !last[1])) start = "index.html";

    int i = 0;
    for (; start[i] && start[i] != '?' && start[i] != '#' && i < FS_NAMELEN - 1; i++)
        out[i] = start[i];
    out[i] = 0;
    if (!out[0]) strcpy(out, "index.html");
}

/* web -- the browser.
 *
 * It is a program like any other, which is the point: the kernel hands out
 * a socket and a screen and the browser does the rest, so a machine that
 * never opens it pays nothing for it. `web` is only here to save you
 * typing `start web.pico`. */
static void cmd_web(int argc, char **argv)
{
    char *av[3];
    av[0] = (char *)"web";
    av[1] = (char *)"web.pico";
    av[2] = (argc > 1) ? argv[1] : 0;
    cmd_start((argc > 1) ? 3 : 2, av, 0);
}

static void cmd_wget(int argc, char **argv)
{
    if (argc < 2) {
        puts("\n  wget <url> [filename] [max bytes]\n"
             "  wget http://example.com/tetris.pico\n\n");
        return;
    }
    if (net_need_ip() < 0) {
        puts("\n  no network card, or no address came back\n\n");
        return;
    }

    char name[FS_NAMELEN];
    if (argc > 2) strncpy(name, argv[2], FS_NAMELEN - 1);
    else          wget_name(argv[1], name);
    name[FS_NAMELEN - 1] = 0;

    u32 max = argc > 3 ? (u32)atoi(argv[3]) : 65536;

    puts("\n  fetching ");
    puts(argv[1]);
    puts("\n  ");
    /* the dot is the whole progress bar: this OS has one task and one
     * screen and no time to draw */
    for (int i = 0; i < 3; i++) { putc('.'); sleep_ms(1); }
    putc('\n');

    char status[80];
    u32  got = 0;
    int  r = net_http_get(argv[1], name, max, &got, status, sizeof status);

    if (r < 0) {
        const char *why =
            r == -2 ? "no answer from the name server"
          : r == -3 ? "could not connect"
          : r == -4 ? "sending the request failed"
          : r == -5 ? "not enough memory for that many bytes"
          : r == -6 ? "could not write the file"
          : r == -10 ? net_tls_error()
                    : "the fetch failed";
        kprintf("  %s (%d)\n\n", why, r);
        return;
    }

    if (status[0]) kprintf("  %s\n", status);
    kprintf("  %u bytes -> %s\n", got, name);
    if (got && strlen(name) >= 5 && !strncmp(name + strlen(name) - 5, ".pico", 5))
        kprintf("  it is a program: `start %s` runs it\n", name);
    putc('\n');
}

/* ---- wireless --------------------------------------------------------
 *
 * Two different things live behind this command.
 *
 * `wifi` on its own looks at what the machine has: a wireless chip on
 * the pci bus, if any, and an esp module on the serial port, if any. What
 * it says about the chip is the truth, which is usually "that one needs a
 * program from the chip maker and we are not allowed to hand you one".
 *
 * `wifi join` and `wifi get` are the other thing: a real wireless
 * connection, through a module that brings its own brains.
 * ------------------------------------------------------------------ */

static void cmd_wifi(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "join")) {
        if (argc < 3) {
            puts("\n  wifi join <network name> [password]\n\n");
            return;
        }
        puts("\n  trying to join ");
        puts(argv[2]);
        puts("...\n");
        if (wifi_ser_join(argv[2], argc > 3 ? argv[3] : "") < 0) {
            con_setcolor(YELLOW, BLACK);
            puts("  that did not work. no module on the serial port, or the\n"
                 "  name and password were not the ones the router wants.\n");
            con_setcolor(LGREY, BLACK);
        } else {
            con_setcolor(LGREEN, BLACK);
            puts("  joined. `wifi get <url>` fetches something now.\n");
            con_setcolor(LGREY, BLACK);
        }
        putc('\n');
        return;
    }

    if (argc > 1 && !strcmp(argv[1], "get")) {
        if (argc < 3) {
            puts("\n  wifi get <url> [filename]\n\n");
            return;
        }
        if (!wifi_ser_joined()) {
            puts("\n  not joined to anything yet: `wifi join <name> [password]`\n\n");
            return;
        }

        u8  *body = (u8 *)kmalloc(60000);
        char name[FS_NAMELEN];
        if (argc > 3) strncpy(name, argv[3], FS_NAMELEN - 1);
        else          wget_name(argv[2], name);
        name[FS_NAMELEN - 1] = 0;

        if (!body) { puts("\n  not enough memory\n\n"); return; }

        char status[80];
        u32  got = 0;
        int  r = wifi_ser_get(argv[2], body, 60000, &got, status, sizeof status);

        if (r < 0) {
            const char *why =
                r == -2 ? "not joined to a network"
              : r == -3 ? "that url makes no sense"
              : r == -4 ? "the module could not look the name up"
              : r == -5 ? "the connection was refused"
              : r == -6 ? "the module would not take the request"
                        : "no esp module answered on the serial port";
            kprintf("\n  %s (%d)\n\n", why, r);
        } else {
            if (status[0]) kprintf("\n  %s\n", status);
            if (fs_write(name, (const char *)body, got) < 0)
                puts("  could not write the file\n");
            else
                kprintf("  %u bytes -> %s\n", got, name);
            putc('\n');
        }
        kfree(body);
        return;
    }

    /* `wifi` on its own: what is here, and what it would take */
    puts("\n");
    int n = wifi_probe();
    if (!n) {
        puts("  no wireless controller on the pci bus\n");
    } else {
        for (int i = 0; i < n; i++)
            kprintf("  wireless chip: %s\n",
                    wifi_name(wifi_vendor(i), wifi_device(i)));
        putc('\n');
    }

    if (wifi_ser_probe()) {
        con_setcolor(LGREEN, BLACK);
        if (wifi_ser_joined()) {
            puts("  esp module on the serial port, joined to ");
            puts(wifi_ser_ssid());
            puts("\n");
        } else {
            puts("  esp module on the serial port, not joined to anything\n");
        }
        con_setcolor(LGREY, BLACK);
        puts("  wifi join <name> [password]     join a network\n"
             "  wifi get <url> [file]           fetch something over it\n");
    } else {
        puts("  no esp module answered on com1\n");
    }

    con_setcolor(YELLOW, BLACK);
    puts("\n  why not the wireless chip:\n");
    con_setcolor(LGREY, BLACK);
    puts("  every wireless chip except the Atheros ones is a radio with no\n"
         "  brain: the part that joins a network is a program the host has\n"
         "  to upload into it, made by the chip maker, hundreds of\n"
         "  kilobytes, fetched over the network you do not have yet.\n"
         "  A usb dongle is the same problem in a smaller box.\n\n"
         "  Two ways out. A cable, which works today. Or an esp8266 / esp32\n"
         "  module on the serial port: it keeps its firmware in rom, it\n"
         "  costs nothing, and PicoOS already talks to it -- that is what\n"
         "  `wifi join` and `wifi get` above are for.\n\n");
}

static void cmd_mouse(void)
{
    int ok = mouse_enable();
    int x = 0, y = 0, b = 0;

    if (ok) {
        for (int i = 0; i < 20 && !mouse_alive(); i++) task_wait();

        mouse_get(&x, &y, &b);
        kprintf("  ps/2 mouse enabled, pointer at %d,%d, buttons %d\n",
                x, y, b);
        puts(mouse_alive() ? "  it is reporting: move it and the menu will follow\n"
                           : "  enabled but silent so far -- move it and try again\n");
    } else {
        int s = mouse_step();
        if (s == 3)
            puts("  no PS2 mouse: reset/defaults got no ACK.\n"
                 "  The aux port and config byte worked; the device did not answer F6.\n");
        else if (s == 4)
            puts("  no PS2 mouse: reporting command got no ACK after reset.\n");
        else if (s == 2)
            puts("  no PS2 mouse: the 8042 returned no configuration byte.\n");
        else
            kprintf("  no PS2 mouse: controller stopped at step %d.\n", s);
        puts("  steps: aux port, config byte, reset/defaults, start reporting.\n"
             "  A USB mouse needs the firmware's legacy/PS/2 emulation on.\n");
    }
}

/* progs -- just the executables, with the header of each one decoded */
/* One table for every program on the system.
 *
 * There used to be a second command for the ones that were still
 * compressed, and a `get` to unpack one by hand. Both were ceremony:
 * `start` unpacks whatever it needs on its own, so the only thing the
 * separate view ever told you was a detail of storage. It belongs in the
 * one place you already look to see what you can run -- as a column. */
static void cmd_progs(void)
{
    int n = fs_count(), found = 0;
    puts("\n  NAME            CODE    BSS  RELOC  ON DISK\n");
    for (int i = 0; i < n; i++) {
        file_t *f = fs_at(i);
        u32 nl = strlen(f->name);
        if (nl <= 5 || strcmp(f->name + nl - 5, ".pico")) continue;
        if (f->size < sizeof(pico_hdr_t)) continue;

        pico_hdr_t h;
        memcpy(&h, f->data, sizeof h);
        if (h.magic != PICO_MAGIC) continue;

        found++;
        con_setcolor(LGREEN, BLACK);
        kprintf("  %s", f->name);
        con_setcolor(LGREY, BLACK);
        for (u32 k = nl; k < 16; k++) putc(' ');
        pad_num(h.image_size, 6);
        pad_num(h.bss_size, 7);
        pad_num(h.reloc_count, 7);

        const store_ent_t *e = store_find(f->name);
        if (e && e->packed < e->orig)
            kprintf("  %u packed\n", e->packed);
        else
            puts("  in memory\n");
    }
    /* The ones still compressed are just as runnable -- start unpacks them
     * on the way -- so they go in the same table, greyed out. */
    int packed = 0;
    for (int i = 0; i < store_count(); i++) {
        const store_ent_t *e = store_at(i);
        if (fs_find(e->name)) continue;
        packed++;

        con_setcolor(DGREY, BLACK);
        kprintf("  %s", e->name);
        for (u32 k = strlen(e->name); k < 16; k++) putc(' ');
        pad_num(e->orig, 6);
        puts("      -      -");
        kprintf("  %u packed\n", e->packed);
        con_setcolor(LGREY, BLACK);
    }

    if (!found && !packed) puts("  (none)\n");

    u32 sp = store_packed_bytes(), so = store_orig_bytes();
    if (so > sp)
        kprintf("\n  the packed ones cost %u bytes instead of %u, and unpack\n"
                "  themselves the first time you start them\n", sp, so);
    kprintf("\n  programs are relocatable, loaded from the heap, api v%d\n\n",
            PICOAPI_VERSION);
}

/* res -- exactly what this operating system costs to run.
 * Everything here is measured at runtime, nothing is a build-time claim. */
static void cmd_res(void)
{
    extern char _kernel_end[];
    u32 kstart  = 0x00010000;
    u32 kend    = (u32)_kernel_end;
    u32 ksize   = kend - kstart;
    u32 heapuse = mem_used();
    u32 stk     = stack_used();

    u32 papp = papp_size();

    u32 sp = store_packed_bytes(), so = store_orig_bytes();

    puts("\n  DISK\n");
    kprintf("    boot sector          512 bytes\n");
    kprintf("    kernel + programs    %u bytes\n", ksize);
    if (so)
        kprintf("    ...of which programs %u bytes packed, %u unpacked\n",
                sp, so);
    if (papp)
        kprintf("    added afterwards     %u bytes  (%d file(s))\n",
                papp, papp_count());
    kprintf("    total on disk        %u bytes  (%u KB)\n",
            ksize + 512 + papp, (ksize + 512 + papp + 1023) / 1024);

    puts("\n  RAM ACTUALLY IN USE\n");
    kprintf("    kernel image         %u bytes  at %p\n", ksize, kstart);
    kprintf("    kernel stack         %u of %u bytes used\n", stk, stack_size());
    kprintf("    heap handed out      %u bytes\n", heapuse);
    kprintf("    running total        %u bytes  (%u KB)\n",
            ksize + heapuse, (ksize + heapuse + 1023) / 1024);

    puts("\n  RAM THAT MUST EXIST\n");
    kprintf("    heap starts at       %p\n", mem_base());
    kprintf("    heap on this machine %u KB\n", mem_total() / 1024);

    /* Programs are relocatable and come out of the heap, so nothing is
     * reserved for them. The floor is the kernel plus enough heap to load
     * the largest program that ships with it. Measured from where the
     * kernel actually ends, not from this machine's heap, which sits high
     * up whenever there is extended memory to put it in. */
    u32 floor_bytes = (papp ? papp_base() + papp : kend) + 32 * 1024;
    kprintf("    smallest machine     %u KB of ram\n",
            (floor_bytes + 1023) / 1024);

    kprintf("    per extra task       %u bytes  (%u KB stack + bookkeeping)\n",
            (u32)task_stack_size() + 320u, task_stack_size() / 1024u);

    puts("\n  CPU\n");
    kprintf("    tasks running now    %d of %d\n", task_running_count(), TASK_MAX);
    puts("    any 80386 or better, no minimum clock speed\n\n");
}

static void beep(u32 freq, u32 ms)
{
    if (freq < 20) freq = 20;
    u32 div = 1193180 / freq;
    outb(0x43, 0xB6);
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)((div >> 8) & 0xFF));
    u8 t = inb(0x61);
    outb(0x61, t | 3);
    sleep_ms(ms);
    outb(0x61, inb(0x61) & 0xFC);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
static void head(const char *title)
{
    con_setcolor(LCYAN, BLACK);
    page_puts(title);
    con_setcolor(LGREY, BLACK);
}

static void cmd_help(void)
{
    page_reset();

    head("\nPicoOS commands\n");
    page_puts(
    "  help              this list\n"
    "  clear             clear the screen\n"
    "  echo <text>       print text\n"
    "  ver / about       version and credits\n"
    "  mem               heap statistics\n"
    "  usb               USB controllers and devices found\n"
    "  uptime            time since boot\n"
    "  mode [25|50]      lines of text on screen\n"
    "  time [HH:MM:SS]   show the clock, or set it\n"
    "  date [YYYY-MM-DD] show the date, or set it\n"
    "  bg <0-7>          desktop wallpaper colour\n"
    "  sound on|off      pc speaker on or off\n"
    "  cpu               cpu vendor and features\n");

    head("\nprograms\n");
    page_puts(
    "  mouse             test the PS/2 mouse path\n"
    "  progs             the programs on this system, packed or not\n"
    "  menu              open the graphical PicoOS menu\n"
    "  start <prog>      run it  (unpacks it first if it needs to)\n"
    "  run <prog>        same as start; e.g. run tetris.pico\n"
    "  start <prog> &    ...and give me my prompt back\n"
    "  exit              return to the graphical menu\n"
    "  bg <prog>         the same, for keyboards without an &\n"
    "  (add your own with tools/addapp.py -- no rebuild needed)\n");

    head("\ntasks\n");
    page_puts(
    "  ps / jobs         what is running right now\n"
    "  fg <pid>          bring a background program to the screen\n"
    "  kill <pid>        stop it (ctrl-c does the same up front)\n"
    "  res               what this OS costs: disk, ram, cpu\n");

    head("\nfiles (RAM disk)\n");
    page_puts(
    "  ls                list files\n"
    "  cat <f>           show a file\n"
    "  write <f> <text>  create/overwrite a file\n"
    "  app <f> <text>    append a line to a file\n"
    "  cp <a> <b>        copy a file\n"
    "  rm <f>            delete a file -- to the bin first\n"
    "  bin               what is in the bin\n"
    "  restore <nr>      fish one back out of the bin\n"
    "  empty             really delete them and free the space\n");

    head("\nnetwork\n");
    page_puts(
    "  net                the network cards and the address they have\n"
    "  net diag           what the card itself reports\n"
    "  net ne2000 0x300   look for an isa ne2000 at that address\n"
    "  dhcp               ask the router for an address\n"
    "  ip auto            find the network without a router\n"
    "  ip <a> <m> <g> <d> set the address by hand\n"
    "  ping <name/ip>     is it there?\n"
    "  wget <url> [file]  fetch it onto the RAM disk\n"
    "  web [url]          the browser: a page, in text, with numbered links\n"
    "  wifi               what wireless this machine has, and what it costs\n"
    "  wifi join <n> [p]  join a network through the esp module on com1\n"
    "  wifi get <url> [f] fetch over it\n");

    head("\nmachine\n");
    page_puts(
    "  calc <a> <op> <b> integer maths (+ - * / %)\n"
    "  peek <addr>       read a byte of physical memory\n"
    "  poke <addr> <val> write a byte of physical memory\n"
    "  hex <addr> [n]    hexdump memory\n"
    "  beep [hz] [ms]    pc speaker\n"
    "  sleep <ms>        wait\n"
    "  color <fg> <bg>   text colour 0-15\n"
    "  reboot            restart\n"
    "  halt / shutdown   power the machine off\n\n");
}

static void cmd_ver(void)
{
    con_setcolor(LGREEN, BLACK);
    puts("\n  PicoOS " PICOOS_VERSION "\n");
    con_setcolor(LGREY, BLACK);
    puts("  A 32-bit x86 operating system written from scratch.\n"
         "  No Linux, no BSD, no libc -- assembly + C, nothing else.\n"
         "  Programs are relocatable and loaded from the heap, so they\n"
         "  cost exactly their own size, and several can run at once:\n"
         "  pre-emptive round-robin, one task on screen, the rest behind.\n"
         "  The programs ship compressed and unpack only when you run one,\n"
         "  so a boot that never starts anything costs no memory for them.\n"
         "  Keyboards and mice work through PS/2 and the firmware's USB\n"
         "  emulation, with no driver to install and no setup to wait for.\n"
         "  The network is its own: ethernet, arp, ip, icmp, udp, tcp, dhcp,\n"
         "  dns and a little http, over five different network cards, plus\n"
         "  wireless through an esp module on the serial port.\n"
         "  There is a browser: 'web' reads a page, keeps the headings,\n"
         "  the paragraphs and the lists, numbers every link on it, and\n"
         "  lets you type a number to follow one. Plain http only -- it\n"
         "  says so when a page is encrypted rather than mangling it.\n"
         "  Write your own games with 'make', paint with 'draw'.\n"
         "  Any 386, no minimum clock. Type 'res' for the full breakdown,\n"
         "  measured on this machine.\n\n");
}

static void cmd_mem(void)
{
    u32 t = mem_total(), u = mem_used(), f = mem_free();
    kprintf("heap total : %u bytes (%u KB)\n", t, t / 1024);
    kprintf("heap used  : %u bytes\n", u);
    kprintf("heap free  : %u bytes (%u KB)\n", f, f / 1024);
    kprintf("blocks     : %d\n", mem_blocks());
    u32 pct = t ? (u * 100) / t : 0;
    puts("[");
    for (u32 i = 0; i < 40; i++) putc(i < (pct * 40) / 100 ? '#' : '.');
    kprintf("] %u%%\n", pct);
}

static void cmd_uptime(void)
{
    u32 ms = timer_ms();
    kprintf("up %u.%u seconds (%u ticks)\n",
            ms / 1000, (ms % 1000) / 100, timer_ticks());
}

/* ------------------------------------------------------------------ */
/* mode / background / sound -- new in 1.0                             */
/* ------------------------------------------------------------------ */

static void cmd_mode(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("\n  text is %dx%d (%s)\n", con_width(), con_height(),
                con_driver());
        puts("  mode 25     80x25 text -- the classic\n"
             "  mode 50     80x50 text -- smaller font, twice the lines\n"
             "  the drawing program switches to 640x480 graphics itself\n\n");
        return;
    }
    int want = atoi(argv[1]);
    if (want != 25 && want != 50) {
        puts("\n  only 25 and 50 are text modes here (draw does graphics)\n\n");
        return;
    }
    int got = gfx_lines(want);
    if (!got)
        puts("\n  this screen was set by the firmware loader and stays as it is\n\n");
    else
        kprintf("\n  ok, %dx%d\n\n", con_width(), con_height());
}

static void cmd_bg(int argc, char **argv)
{
    static const char *names[8] = {
        "black", "blue", "green", "cyan", "red", "magenta", "brown", "grey"
    };
    if (argc < 2) {
        puts("\n  bg <0-7>   the background colour of the screen:\n"
             "   0 black   1 blue    2 green   3 cyan\n"
             "   4 red     5 magenta 6 brown   7 grey\n\n");
        return;
    }
    int c = atoi(argv[1]) & 7;
    con_setcolor(LGREY, (u8)c);
    con_clear();
    kprintf("  background is %s. (bg 0 puts it back)\n", names[c]);
}

static void cmd_sound(int argc, char **argv)
{
    if (argc < 2) {
        puts("\n  sound on          the speaker works\n"
             "  sound off         silence, even from programs\n"
             "  sound test [hz] [ms]   prove it\n\n");
        return;
    }
    if (!strcmp(argv[1], "off")) {
        sound_set(0);
        puts("\n  sound off. programs that beep will not\n\n");
    } else if (!strcmp(argv[1], "on")) {
        sound_set(1);
        puts("\n  sound on\n\n");
    } else if (!strcmp(argv[1], "test")) {
        u32 hz = argc > 2 ? (u32)atoi(argv[2]) : 880;
        u32 ms = argc > 3 ? (u32)atoi(argv[3]) : 150;
        if (!sound_is_on()) { puts("\n  sound is off -- `sound on` first\n\n"); return; }
        beep(hz, ms);
        kprintf("\n  %u Hz for %u ms\n\n", hz, ms);
    } else {
        puts("\n  usage: sound on | off | test [hz] [ms]\n\n");
    }
}

/* ------------------------------------------------------------------ */
/* the clock, readable and settable -- new in 1.0                      */
/* ------------------------------------------------------------------ */

/* "12:34:56" -> 12, 34, 56. Accepts HH:MM too, and is picky on purpose:
 * a typo that sets the clock to 99:99 would be worse than an error. */
static int parse_hms(const char *s, int *h, int *m, int *sc)
{
    int v[3] = { -1, 0, 0 }, n = 0;
    for (int i = 0; s[i] && n < 3; i++) {
        if (s[i] == ':') { if (v[n] < 0) return 0; n++; continue; }
        if (s[i] < '0' || s[i] > '9') return 0;
        if (v[n] < 0) v[n] = 0;
        v[n] = v[n] * 10 + (s[i] - '0');
        if (v[n] > 99) return 0;
    }
    if (v[0] < 0) return 0;
    *h = v[0]; *m = v[1]; *sc = v[2];
    return *h <= 23 && *m <= 59 && *sc <= 59;
}

/* "2026-09-01" or "01-09-2026" style: three numbers with any separator
 * that is not a digit. Year must be the four-digit one. */
static int parse_date(const char *s, int *y, int *mo, int *d)
{
    int v[3] = { -1, -1, -1 }, n = 0;
    for (int i = 0; s[i] && n < 3; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (v[n] < 0) v[n] = 0;
            v[n] = v[n] * 10 + (s[i] - '0');
            if (v[n] > 9999) return 0;
        } else {
            if (v[n] < 0) return 0;
            n++;
        }
    }
    if (v[0] < 0 || v[1] < 0 || v[2] < 0) return 0;
    if (v[0] > 31) { *y = v[0]; *mo = v[1]; *d = v[2]; }    /* Y-M-D    */
    else           { *d = v[0]; *mo = v[1]; *y = v[2]; }    /* D-M-Y    */
    return *y >= 1970 && *y <= 2099 && *mo >= 1 && *mo <= 12 && *d >= 1 && *d <= 31;
}

static void cmd_time(int argc, char **argv)
{
    if (argc >= 2) {
        int h, m, s;
        if (!parse_hms(argv[1], &h, &m, &s)) {
            puts("\n  usage: time [HH:MM:SS]\n\n");
            return;
        }
        rtc_set_time(h, m, s);
        puts("\n  clock set\n");
    }
    int h, m, s;
    rtc_time(&h, &m, &s);
    kprintf("  %02d:%02d:%02d\n\n", h, m, s);
}

static void cmd_date(int argc, char **argv)
{
    if (argc >= 2) {
        int y, mo, d;
        if (!parse_date(argv[1], &y, &mo, &d)) {
            puts("\n  usage: date [YYYY-MM-DD]\n\n");
            return;
        }
        rtc_set_date(y, mo, d);
        puts("\n  date set\n");
    }
    int y, mo, d;
    rtc_date(&y, &mo, &d);
    kprintf("  %04d-%02d-%02d\n\n", y, mo, d);
}

/* ------------------------------------------------------------------ */
/* the recycle bin -- new in 1.0                                       */
/*                                                                     */
/* `rm` used to be final, which is an honest way to lose a file. Now   */
/* it goes to the bin first: the file is renamed b<N>~<name>, which    */
/* costs nothing but a directory slot and keeps the data where it      */
/* already was. `bin` lists them, `restore <nr>` brings one back,      */
/* `empty` really deletes them and hands the heap back.                */
/* ------------------------------------------------------------------ */

static int is_binned(const char *name)
{
    return name[0] == 'b' && name[1] >= '0' && name[1] <= '9' && name[2] == '~';
}

static int bin_count(void)
{
    int n = 0;
    for (int i = 0; i < fs_count(); i++) {
        file_t *f = fs_at(i);
        if (is_binned(f->name)) n++;
    }
    return n;
}

/* move a file into the bin; 1 = binned, -1 = hidden (built-in file), 0 = no */
static int to_bin(const char *name)
{
    file_t *f = fs_find(name);
    if (!f) return 0;

    /* files that are baked into the kernel image are not heap copies;
     * binning one would materialise a heap copy of something we already
     * have for free. Hide it instead -- restore is `reboot` for those. */
    if (f->cap == 0) { fs_delete(name); return -1; }

    for (int n = 1; n <= 99; n++) {
        char bin[FS_NAMELEN], num[8];
        utoa((u32)n, num, 10);
        strcpy(bin, "b");
        strncat(bin, num,  FS_NAMELEN - 1 - strlen(bin));
        strncat(bin, "~",  FS_NAMELEN - 1 - strlen(bin));
        strncat(bin, name, FS_NAMELEN - 1 - strlen(bin));
        if (fs_find(bin)) continue;                 /* slot taken       */

        file_t *nf = fs_create(bin);
        (void)nf;
        if (fs_write(bin, f->data ? f->data : "", f->size) < 0) {
            fs_delete(bin);
            return 0;
        }
        fs_delete(name);
        return 1;
    }
    return 0;
}

/* the n-th file in the bin, counting from 1 */
static file_t *bin_at(int want)
{
    int n = 0;
    for (int i = 0; i < fs_count(); i++) {
        file_t *f = fs_at(i);
        if (!is_binned(f->name)) continue;
        n++;
        if (n == want) return f;
    }
    return NULL;
}

static void cmd_bin(void)
{
    int n = bin_count();
    if (!n) { puts("\n  the bin is empty\n\n"); return; }

    puts("\n  the bin -- restore <nr> gets one back, empty clears all\n");
    int k = 0;
    for (int i = 0; i < fs_count(); i++) {
        file_t *f = fs_at(i);
        if (!is_binned(f->name)) continue;
        k++;
        kprintf("   %2d  %s", k, f->name + 3);
        for (int pad = (int)strlen(f->name + 3); pad < 14; pad++) putc(' ');
        kprintf("%u bytes\n", f->size);
    }
    putc('\n');
}

static void cmd_restore(int argc, char **argv)
{
    if (argc < 2) { puts("\n  usage: restore <nr>   (see `bin`)\n\n"); return; }

    file_t *f = bin_at(atoi(argv[1]));
    if (!f) { puts("\n  no such item in the bin\n\n"); return; }

    const char *orig = f->name + 3;
    if (!orig[0]) { puts("\n  this one lost its name; write it out first\n\n"); return; }
    if (fs_find(orig)) { puts("\n  a file with that name exists again already\n\n"); return; }

    if (fs_write(orig, f->data ? f->data : "", f->size) < 0) {
        puts("\n  could not write it back\n\n");
        return;
    }
    char keep[FS_NAMELEN];
    strncpy(keep, f->name, FS_NAMELEN - 1);
    keep[FS_NAMELEN - 1] = 0;
    fs_delete(keep);
    kprintf("\n  restored %s\n\n", orig);
}

static void cmd_empty(void)
{
    int n = 0;
    for (int i = 0; i < fs_count(); i++) {
        file_t *f = fs_at(i);
        if (is_binned(f->name)) { fs_delete(f->name); n++; i--; }
    }
    kprintf("\n  emptied the bin, %d file%s gone for real\n\n", n, n == 1 ? "" : "s");
}

static int have_cpuid(void)
{
    u32 before, after;
    __asm__ volatile(
        "pushfl\n"
        "pushfl\n"
        "popl %0\n"
        "movl %0, %1\n"
        "xorl $0x200000, %1\n"      /* flip the ID bit */
        "pushl %1\n"
        "popfl\n"
        "pushfl\n"
        "popl %1\n"
        "popfl\n"
        : "=r"(before), "=r"(after));
    return ((before ^ after) & 0x200000) != 0;
}

static void cmd_cpu(void)
{
    u32 a, b, c, d;
    if (!have_cpuid()) {
        puts("cpu has no CPUID instruction -- 386 or early 486.\n");
        return;
    }
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    *(u32 *)&vendor[0] = b;
    *(u32 *)&vendor[4] = d;
    *(u32 *)&vendor[8] = c;
    vendor[12] = 0;
    kprintf("vendor  : %s\n", vendor);
    kprintf("max leaf: %u\n", a);

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    kprintf("family %u model %u stepping %u\n",
            (a >> 8) & 0xF, (a >> 4) & 0xF, a & 0xF);
    puts("features:");
    if (d & (1u << 0))  puts(" fpu");
    if (d & (1u << 4))  puts(" tsc");
    if (d & (1u << 5))  puts(" msr");
    if (d & (1u << 6))  puts(" pae");
    if (d & (1u << 8))  puts(" cx8");
    if (d & (1u << 23)) puts(" mmx");
    if (d & (1u << 25)) puts(" sse");
    if (d & (1u << 26)) puts(" sse2");
    if (c & (1u << 0))  puts(" sse3");
    putc('\n');
}

static void ls_row(const char *name, u32 size, int exe)
{
    u32 nl = strlen(name);
    con_setcolor(exe ? LGREEN : LGREY, BLACK);
    kprintf("%s", name);
    con_setcolor(LGREY, BLACK);
    for (u32 k = nl; k < 16; k++) putc(' ');
    kprintf("%u", size);
    u32 digits = 1, v = size;
    while (v >= 10) { v /= 10; digits++; }
    for (u32 k = digits; k < 8; k++) putc(' ');
    puts(exe ? "program\n" : "text\n");
}

static void cmd_ls(void)
{
    /* `ls` shows both ordinary RAM-disk files and shipped .pico programs.
     * Programs stay compressed until first run, but they are still files a
     * user can discover and launch with `run tetris.pico`. */
    int n = fs_count();
    int shown = 0;
    u32 total = 0;

    puts("NAME            SIZE    TYPE\n");
    for (int i = 0; i < n; i++) {
        file_t *f = fs_at(i);
        u32 nl = strlen(f->name);
        int exe = (nl > 5 && !strcmp(f->name + nl - 5, ".pico"));
        ls_row(f->name, f->size, exe);
        total += f->size;
        shown++;
    }

    /* The program store is the read-only part of the directory. Do not
     * print an extracted program twice: fs_find() is the authoritative
     * check after a program has been run or deliberately unpacked. */
    for (int i = 0; i < store_count(); i++) {
        const store_ent_t *e = store_at(i);
        if (!e || fs_find(e->name)) continue;
        ls_row(e->name, e->orig, 1);
        total += e->orig;
        shown++;
    }

    if (!shown) { puts("(no files)\n"); return; }
    kprintf("%d file(s), %u bytes\n", shown, total);
}

static void cmd_calc(int argc, char **argv)
{
    if (argc < 4) { puts("usage: calc <a> <op> <b>\n"); return; }
    int a = atoi(argv[1]), b = atoi(argv[3]);
    char op = argv[2][0];
    int r = 0;
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case 'x': r = a * b; break;
    case '/': if (!b) { puts("division by zero\n"); return; } r = a / b; break;
    case '%': if (!b) { puts("division by zero\n"); return; } r = a % b; break;
    default:  puts("unknown operator\n"); return;
    }
    kprintf("%d %c %d = %d  (0x%x)\n", a, op, b, r, (u32)r);
}

static void cmd_hex(int argc, char **argv)
{
    if (argc < 2) { puts("usage: hex <addr> [count]\n"); return; }
    u32 addr = (u32)atoi(argv[1]);
    int n = argc > 2 ? atoi(argv[2]) : 64;
    if (n <= 0) n = 64;
    if (n > 512) n = 512;
    for (int i = 0; i < n; i += 16) {
        kprintf("%08x  ", addr + (u32)i);
        for (int j = 0; j < 16; j++) {
            u8 v = *(volatile u8 *)(addr + (u32)(i + j));
            char t[4];
            utoa(v, t, 16);
            if (v < 16) putc('0');
            puts(t); putc(' ');
        }
        puts(" |");
        for (int j = 0; j < 16; j++) {
            u8 v = *(volatile u8 *)(addr + (u32)(i + j));
            putc((v >= 32 && v < 127) ? (char)v : '.');
        }
        puts("|\n");
    }
}

static void cmd_banner(void)
{
    con_setcolor(LGREEN, BLACK);
    puts("\n"
    "   ___  _         ___  ___ \n"
    "  | _ \\(_) __ ___ / _ \\/ __|\n"
    "  |  _/| |/ _/ _ \\ (_) \\__ \\\n"
    "  |_|  |_|\\__\\___/\\___/|___/\n");
    con_setcolor(LGREY, BLACK);
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------- tasks -- */

static void cmd_ps(void)
{
    task_reap(0);
    task_list();
    con_setcolor(DGREY, BLACK);
    puts("  fore = owns the screen   back = running behind   idle = waiting\n");
    con_setcolor(LGREY, BLACK);
}

static void cmd_kill(int argc, char **argv)
{
    if (argc < 2) { puts("  usage: kill <pid>   (see 'ps')\n"); return; }
    int pid = atoi(argv[1]);
    int r = task_kill(pid);
    if (r == -2)      puts("  that one is the shell; it stays\n");
    else if (r < 0)   kprintf("  no task %d\n", pid);
    else              kprintf("  asked %d to stop\n", pid);
}

static void cmd_fg(int argc, char **argv)
{
    if (argc < 2) { puts("  usage: fg <pid>   (see 'ps')\n"); return; }
    int pid = atoi(argv[1]);
    if (pid == task_self()) { puts("  that is this shell\n"); return; }
    int r = pico_fg(pid);
    if (r == -1)       kprintf("  no task %d\n", pid);
    else if (r == -101) puts("  stopped\n");
}

void shell_run(void)
{
    char line[LINESZ];
    shell_leave = 0;
    char *argv[MAXARGS];

    for (; !shell_leave;) {
        task_reap(0);            /* tidy up anything that finished behind us */
        con_setcolor(LGREEN, BLACK);
        puts("pico");
        con_setcolor(WHITE, BLACK);
        puts("> ");
        con_setcolor(LGREY, BLACK);

        kbd_readline(line, LINESZ);
        if (!line[0]) continue;

        char raw[LINESZ];
        strcpy(raw, line);

        int argc = split(line, argv);
        if (!argc) continue;
        char *c = argv[0];

        if (!strcmp(c, "store")) {
            char *sv[2]; sv[0] = "start"; sv[1] = "store";
            cmd_start(2, sv, 0);
        }
        else if (!strcmp(c, "menu")) {
            char *mv[2]; mv[0] = "start"; mv[1] = "menu";
            cmd_start(2, mv, 0);
        }
        else if (!strcmp(c, "exit")) {
            if (shell_nested) shell_leave = 1;
            else puts("  the boot shell stays active; use menu to open the GUI.\n");
        }
        else if (!strcmp(c, "start") || !strcmp(c, "run")) cmd_start(argc, argv, 0);
        else if (!strcmp(c, "bg"))  cmd_start(argc, argv, 1);
        else if (!strcmp(c, "help") || !strcmp(c, "?"))   cmd_help();
        else if (!strcmp(c, "clear") || !strcmp(c, "cls")) con_clear();
        else if (!strcmp(c, "echo")) {
            puts(raw + (strlen("echo") < strlen(raw) ? 5 : 4));
            putc('\n');
        }
        else if (!strcmp(c, "ver") || !strcmp(c, "about")) cmd_ver();
        else if (!strcmp(c, "banner"))  cmd_banner();
        else if (!strcmp(c, "usb")) cmd_usb();
        else if (!strcmp(c, "mem") || !strcmp(c, "free")) cmd_mem();
        else if (!strcmp(c, "uptime"))  cmd_uptime();
        else if (!strcmp(c, "cpu"))     cmd_cpu();
        else if (!strcmp(c, "ls") || !strcmp(c, "dir"))   cmd_ls();
        else if (!strcmp(c, "progs"))   cmd_progs();
        else if (!strcmp(c, "mouse"))   cmd_mouse();
        else if (!strcmp(c, "net"))      cmd_net(argc, argv);
        else if (!strcmp(c, "dhcp"))     cmd_dhcp();
        else if (!strcmp(c, "ip"))       cmd_ip(argc, argv);
        else if (!strcmp(c, "sniff"))    cmd_sniff(argc, argv);
        else if (!strcmp(c, "ping"))     cmd_ping(argc, argv);
        else if (!strcmp(c, "wget"))     cmd_wget(argc, argv);
        else if (!strcmp(c, "web") || !strcmp(c, "browse"))
                                         cmd_web(argc, argv);
        else if (!strcmp(c, "wifi"))     cmd_wifi(argc, argv);
        else if (!strcmp(c, "res"))     cmd_res();
        else if (!strcmp(c, "ps") || !strcmp(c, "jobs")) cmd_ps();
        else if (!strcmp(c, "kill"))    cmd_kill(argc, argv);
        else if (!strcmp(c, "fg"))      cmd_fg(argc, argv);
        else if (!strcmp(c, "cat")) {
            if (argc < 2) { puts("usage: cat <file>\n"); continue; }
            file_t *f = fs_find(argv[1]);
            if (!f) { kprintf("no such file: %s\n", argv[1]); continue; }
            if (f->size) { for (u32 i = 0; i < f->size; i++) putc(f->data[i]); }
            if (!f->size || f->data[f->size - 1] != '\n') putc('\n');
        }
        else if (!strcmp(c, "write")) {
            if (argc < 2) { puts("usage: write <file> <text>\n"); continue; }
            const char *txt = rest(raw, argv, argc, 2);
            int r = fs_write(argv[1], txt, strlen(txt));
            if (r < 0) puts("write failed (disk full?)\n");
            else kprintf("wrote %d bytes to %s\n", r, argv[1]);
        }
        else if (!strcmp(c, "app") || !strcmp(c, "append")) {
            if (argc < 2) { puts("usage: app <file> <text>\n"); continue; }
            const char *txt = rest(raw, argv, argc, 2);
            fs_append(argv[1], txt, strlen(txt));
            fs_append(argv[1], "\n", 1);
            kprintf("appended to %s\n", argv[1]);
        }
        else if (!strcmp(c, "cp")) {
            if (argc < 3) { puts("usage: cp <src> <dst>\n"); continue; }
            file_t *f = fs_find(argv[1]);
            if (!f) { puts("no such file\n"); continue; }
            fs_write(argv[2], f->data ? f->data : "", f->size);
            kprintf("copied %u bytes\n", f->size);
        }
        else if (!strcmp(c, "rm") || !strcmp(c, "del")) {
            if (argc < 2) { puts("usage: rm <file>\n"); continue; }
            if (!fs_find(argv[1])) { puts("no such file\n"); continue; }
            int r = to_bin(argv[1]);
            if (r == 1)     kprintf("deleted %s -- it is in the bin (`bin`)\n", argv[1]);
            else if (r < 0) kprintf("hidden %s -- it is built in, reboot brings it back\n", argv[1]);
            else            kprintf("deleted %s\n", argv[1]);
        }
        else if (!strcmp(c, "bin"))         cmd_bin();
        else if (!strcmp(c, "mode"))        cmd_mode(argc, argv);
        else if (!strcmp(c, "bg"))          cmd_bg(argc, argv);
        else if (!strcmp(c, "sound"))       cmd_sound(argc, argv);
        else if (!strcmp(c, "restore"))     cmd_restore(argc, argv);
        else if (!strcmp(c, "empty"))       cmd_empty();
        else if (!strcmp(c, "time"))        cmd_time(argc, argv);
        else if (!strcmp(c, "date"))        cmd_date(argc, argv);
        else if (!strcmp(c, "calc"))    cmd_calc(argc, argv);
        else if (!strcmp(c, "peek")) {
            if (argc < 2) { puts("usage: peek <addr>\n"); continue; }
            u32 a = (u32)atoi(argv[1]);
            kprintf("[%p] = %u (0x%x)\n", a, *(volatile u8 *)a, *(volatile u8 *)a);
        }
        else if (!strcmp(c, "poke")) {
            if (argc < 3) { puts("usage: poke <addr> <byte>\n"); continue; }
            u32 a = (u32)atoi(argv[1]);
            *(volatile u8 *)a = (u8)atoi(argv[2]);
            puts("ok\n");
        }
        else if (!strcmp(c, "hex"))     cmd_hex(argc, argv);
        else if (!strcmp(c, "beep")) {
            u32 hz = argc > 1 ? (u32)atoi(argv[1]) : 880;
            u32 ms = argc > 2 ? (u32)atoi(argv[2]) : 200;
            beep(hz, ms);
        }
        else if (!strcmp(c, "sleep")) {
            if (argc < 2) { puts("usage: sleep <ms>\n"); continue; }
            sleep_ms((u32)atoi(argv[1]));
        }
        else if (!strcmp(c, "color")) {
            if (argc < 3) { puts("usage: color <fg 0-15> <bg 0-15>\n"); continue; }
            con_setcolor((u8)(atoi(argv[1]) & 15), (u8)(atoi(argv[2]) & 15));
            puts("colour changed\n");
        }
        else if (!strcmp(c, "reboot")) { puts("rebooting...\n"); sleep_ms(300); reboot(); }
        else if (!strcmp(c, "halt") || !strcmp(c, "shutdown")) {
            puts("shutting down. bye!\n");
            sleep_ms(300);
            shutdown();
        }
        else if (!strcmp(c, "crash")) {   /* exception handler demo */
            puts("triggering a divide-by-zero on purpose...\n");
            __asm__ volatile("xorl %%edx, %%edx\n"
                             "movl $1, %%eax\n"
                             "xorl %%ecx, %%ecx\n"
                             "divl %%ecx\n"
                             ::: "eax", "ecx", "edx");
        }
        else kprintf("unknown command: %s  (try 'help')\n", c);
    }
}

void shell_open(void)
{
    shell_nested = 1;
    shell_run();
    shell_nested = 0;
}
