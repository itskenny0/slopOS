#ifndef NET_H
#define NET_H

#include "pico.h"

/* ---------------- the network ----------------
 *
 * PicoOS talks to the world for the first time. A network card is the first
 * device this OS has ever driven that has no fixed address: the firmware
 * hands it one at boot, so the card has to be found on the PCI bus before
 * anything can be said to it.
 *
 * Everything below the drivers is deliberately small. There is one ARP
 * table, one TCP socket you actually use, no reassembly of fragmented
 * packets and no retransmission timers to speak of: a request goes out, an
 * acknowledgement comes back or the whole thing is tried again. That is
 * enough to ask a router for an address, to ask a name server what a name
 * means, and to fetch a game.
 */

#define NET_MAX_NIC  4
#define NET_MTU      1514
#define NET_TCP_MAX  2
#define NET_TCP_BUF  4096        /* bytes of incoming stream buffered */

struct netif;

typedef struct netif {
    char     name[12];           /* "rtl8139", "e1000", ...          */
    u8       mac[6];
    int      present;
    int      link;               /* 1 = cable in and carrier seen     */
    int      is_wifi;
    u8       irq;
    u32      io;                 /* i/o base, 0 when memory mapped    */
    volatile u32 *mmio;          /* mmio base, 0 when port i/o        */
    u32      speed_mbps;         /* whatever the card says, 0 = ?     */
    pci_dev_t pci;

    int  (*send)(struct netif *n, const u8 *pkt, u32 len);
    int  (*recv)(struct netif *n, u8 *pkt, u32 max);
    void (*diag)(struct netif *n);

    /* filled in by dhcp, used by everybody else */
    u32  ip, gw, mask, dns;
    u8   gmac[6];                /* the router's hardware address     */

    u32  rx_packets, tx_packets;
    u32  tx_ok;              /* of those, how many the card took     */
    void *priv;
} netif_t;

/* ---- the stack itself ---- */
void net_init(void);             /* find the cards, nothing else       */
int  net_count(void);
netif_t *net_at(int i);
netif_t *net_active(void);       /* the one we send through            */
int  net_select(int i);
void nic_undo_last(void);            /* give a card slot back            */
void net_delay(u32 ms);              /* busy-ish wait that works pre-mt   */

netif_t *net_register(const char *name, pci_dev_t *pci, u32 io,
                      volatile u32 *mmio);

/* the wait loops below all pump the card while they wait, so a driver
 * without interrupts works exactly like one with them */
void net_pump(u32 ms);           /* poll for up to ms milliseconds     */
void net_tap(void (*fn)(const u8 *pkt, u32 len), int count);
int  net_send_packet(netif_t *n, u8 *dst_mac, const u8 *payload,
                     u32 len, u16 ethertype);

int  net_arp_lookup(netif_t *n, u32 ip, u8 *mac_out);
int  net_ip_send(netif_t *n, u32 dst_ip, u8 proto, const u8 *payload, u32 len);
int  net_ip_send_raw(netif_t *n, u32 dst_ip, const u8 *dst_mac, u8 proto,
                     const u8 *payload, u32 len);
void net_rx(netif_t *n, const u8 *pkt, u32 len);   /* from net_pump   */
int  udp_send(netif_t *n, u32 dst_ip, u16 sport, u16 dport,
              const u8 *data, u32 len, int broadcast);
int  net_dhcp(netif_t *n);       /* 0 = we have an address             */
int  net_autonet(netif_t *n);    /* read the network off the cable      */
int  net_need_ip(void);          /* dhcp on first use, 0 when we have one */
int  net_ping(u32 ip, u32 timeout_ms);
int  net_dns(const char *name, u32 *out, u32 timeout_ms);
int  net_http_get(const char *url, const char *file, u32 max, u32 *got,
                  char *status, int status_max);
const char *net_tls_error(void);
int  net_http_fetch(const char *url, char *buf, u32 cap, u32 *got,
                    char *status, int status_max);   /* into a buffer */

/* ---- tcp: one request at a time is all this OS needs ---- */
int  tcp_open(u32 ip, u16 port, u32 timeout_ms);
int  tcp_write(int fd, const u8 *data, u32 len, u32 timeout_ms);
int  tcp_read(int fd, u8 *out, u32 max, u32 timeout_ms);
int  tcp_done(int fd);
void tcp_close(int fd);

u32  net_ip(const char *s);      /* "10.0.2.2" -> 0x0A000202, 0 = bad  */
void net_fmt_ip(u32 ip, char *buf);
void net_fmt_mac(const u8 *mac, char *buf);

/* ---- drivers ---- */
void rtl8139_init(void);
void rtl8168_init(void);
void e1000_init(void);
void pcnet_init(void);
int  ne2000_init(u32 io);        /* ISA: only when you ask for it      */

/* ---- wireless, for now: what the machine has and what it would take ---- */
int  wifi_probe(void);           /* how many 802.11 controllers seen   */
const char *wifi_name(u16 vendor, u16 device);
/* ---- an esp module on the serial port: the one wireless that works ---- */
int  wifi_ser_probe(void);       /* is there one on COM1? (0/1)          */
int  wifi_ser_joined(void);
const char *wifi_ser_ssid(void);
int  wifi_ser_join(const char *ssid, const char *pass);
int  wifi_ser_get(const char *url, u8 *out, u32 max, u32 *got,
                  char *status, int status_max);

u16  wifi_vendor(int i);
u16  wifi_device(int i);
int  wifi_is_atheros_rom(int i); /* firmware in rom: we could drive it */

/* ---- helpers drivers share ---- */
void *net_alloc(u32 size, u32 align);   /* kmalloc, aligned, physical   */
void  net_free(void *p);
static inline u32 net_phys(void *p) { return (u32)(u32)p; }  /* 1:1 map */

static inline u16 htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 ntohl(u32 v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
         | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
static inline u32 htonl(u32 v) { return ntohl(v); }

#endif
