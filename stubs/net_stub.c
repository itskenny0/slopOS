/* net_stub.c -- stand-in for the network stack.
 *
 * The PicoOS Pro 2.0 source zip did not contain the net/ directory, so
 * this file provides every function declared in include/net.h as "no
 * network". If you have the original net/ folder, put it next to kernel/
 * and the Makefile uses that instead of this file. */
#include "pico.h"
#include "net.h"

static netif_t none;

void net_init(void) {}
int  net_count(void) { return 0; }
netif_t *net_at(int i) { (void)i; return &none; }
netif_t *net_active(void) { return NULL; }
int  net_select(int i) { (void)i; return -1; }
void nic_undo_last(void) {}
void net_delay(u32 ms) { (void)ms; }
netif_t *net_register(const char *name, pci_dev_t *pci, u32 io, volatile u32 *mmio)
{ (void)name; (void)pci; (void)io; (void)mmio; return NULL; }
void net_pump(u32 ms) { (void)ms; }
void net_tap(void (*fn)(const u8 *pkt, u32 len), int count) { (void)fn; (void)count; }
int  net_send_packet(netif_t *n, u8 *d, const u8 *p, u32 l, u16 t)
{ (void)n; (void)d; (void)p; (void)l; (void)t; return -1; }
int  net_arp_lookup(netif_t *n, u32 ip, u8 *m) { (void)n; (void)ip; (void)m; return -1; }
int  net_ip_send(netif_t *n, u32 d, u8 pr, const u8 *p, u32 l)
{ (void)n; (void)d; (void)pr; (void)p; (void)l; return -1; }
int  net_ip_send_raw(netif_t *n, u32 d, const u8 *m, u8 pr, const u8 *p, u32 l)
{ (void)n; (void)d; (void)m; (void)pr; (void)p; (void)l; return -1; }
void net_rx(netif_t *n, const u8 *p, u32 l) { (void)n; (void)p; (void)l; }
int  udp_send(netif_t *n, u32 d, u16 sp, u16 dp, const u8 *data, u32 l, int b)
{ (void)n; (void)d; (void)sp; (void)dp; (void)data; (void)l; (void)b; return -1; }
int  net_dhcp(netif_t *n) { (void)n; return -1; }
int  net_autonet(netif_t *n) { (void)n; return -1; }
int  net_need_ip(void) { return -1; }
int  net_ping(u32 ip, u32 t) { (void)ip; (void)t; return -1; }
int  net_dns(const char *n, u32 *o, u32 t) { (void)n; (void)o; (void)t; return -1; }
int  net_http_get(const char *u, const char *f, u32 m, u32 *g, char *s, int sm)
{ (void)u; (void)f; (void)m; (void)g; if (s && sm > 0) s[0] = 0; return -1; }
const char *net_tls_error(void) { return "network stack not built in"; }
int  net_http_fetch(const char *u, char *b, u32 c, u32 *g, char *s, int sm)
{ (void)u; (void)b; (void)c; (void)g; if (s && sm > 0) s[0] = 0; return -1; }
int  tcp_open(u32 ip, u16 p, u32 t) { (void)ip; (void)p; (void)t; return -1; }
int  tcp_write(int fd, const u8 *d, u32 l, u32 t) { (void)fd; (void)d; (void)l; (void)t; return -1; }
int  tcp_read(int fd, u8 *o, u32 m, u32 t) { (void)fd; (void)o; (void)m; (void)t; return -1; }
int  tcp_done(int fd) { (void)fd; return 1; }
void tcp_close(int fd) { (void)fd; }
u32  net_ip(const char *s)
{
    u32 v = 0, part = 0, dots = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') part = part * 10 + (u32)(*s - '0');
        else if (*s == '.') { v = (v << 8) | (part & 255); part = 0; dots++; }
        else return 0;
    }
    if (dots != 3) return 0;
    return (v << 8) | (part & 255);
}
void net_fmt_ip(u32 ip, char *b)
{
    char t[8]; b[0] = 0;
    for (int i = 3; i >= 0; i--) {
        itoa((int)((ip >> (i * 8)) & 255), t, 10);
        strcpy(b + strlen(b), t);
        if (i) strcpy(b + strlen(b), ".");
    }
}
void net_fmt_mac(const u8 *m, char *b)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        b[i * 3] = hx[m[i] >> 4]; b[i * 3 + 1] = hx[m[i] & 15];
        b[i * 3 + 2] = i < 5 ? ':' : 0;
    }
}
int  ne2000_init(u32 io) { (void)io; return -1; }
int  wifi_probe(void) { return 0; }
const char *wifi_name(u16 v, u16 d) { (void)v; (void)d; return "none"; }
int  wifi_ser_probe(void) { return 0; }
int  wifi_ser_joined(void) { return 0; }
const char *wifi_ser_ssid(void) { return ""; }
int  wifi_ser_join(const char *s, const char *p) { (void)s; (void)p; return -1; }
int  wifi_ser_get(const char *u, u8 *o, u32 m, u32 *g, char *s, int sm)
{ (void)u; (void)o; (void)m; (void)g; if (s && sm > 0) s[0] = 0; return -1; }
u16  wifi_vendor(int i) { (void)i; return 0; }
u16  wifi_device(int i) { (void)i; return 0; }
int  wifi_is_atheros_rom(int i) { (void)i; return 0; }
void *net_alloc(u32 size, u32 align) { (void)align; return kmalloc(size); }
void  net_free(void *p) { kfree(p); }
