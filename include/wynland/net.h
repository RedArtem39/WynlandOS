/*
 * WynlandOS - Network Stack Header
 *
 * Phase 5: Full network stack — Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS
 */
#pragma once

#include <wynland/types.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define ETH_ALEN        6       /* Ethernet address length */
#define ETH_HLEN        14      /* Ethernet header length */
#define ETH_MTU         1500    /* Maximum transmission unit */
#define ETH_FRAME_MAX   (ETH_HLEN + ETH_MTU)

#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define ARP_CACHE_SIZE  16
#define NET_PKT_BUF     2048   /* Packet buffer size */

/* ============================================================
 * Byte order helpers (compile-time for constants, runtime for vars)
 * ============================================================ */

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000U);
}
static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

/* Construct an IPv4 address in network byte order */
#define IP4(a,b,c,d) htonl(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                           ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ============================================================
 * Protocol Headers
 * ============================================================ */

/* Ethernet frame header */
typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;             /* Big-endian EtherType */
} __attribute__((packed)) EthHeader;

/* ARP header (Ethernet + IPv4) */
typedef struct {
    uint16_t hw_type;          /* 1 = Ethernet */
    uint16_t proto_type;       /* 0x0800 = IPv4 */
    uint8_t  hw_len;           /* 6 */
    uint8_t  proto_len;        /* 4 */
    uint16_t opcode;           /* 1=Request, 2=Reply */
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;        /* Network byte order */
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;        /* Network byte order */
} __attribute__((packed)) ArpHeader;

/* IPv4 header */
typedef struct {
    uint8_t  ver_ihl;          /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;
    uint16_t total_length;     /* Network byte order */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;           /* Network byte order */
    uint32_t dst_ip;           /* Network byte order */
} __attribute__((packed)) Ipv4Header;

/* ICMP header */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) IcmpHeader;

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

/* UDP header */
typedef struct {
    uint16_t src_port;         /* Network byte order */
    uint16_t dst_port;         /* Network byte order */
    uint16_t length;           /* Network byte order */
    uint16_t checksum;
} __attribute__((packed)) UdpHeader;

/* DHCP header */
typedef struct {
    uint8_t  op;               /* 1=BOOTREQUEST, 2=BOOTREPLY */
    uint8_t  htype;            /* 1=Ethernet */
    uint8_t  hlen;             /* 6 */
    uint8_t  hops;
    uint32_t xid;              /* Transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;           /* Client IP */
    uint32_t yiaddr;           /* Your (client) IP */
    uint32_t siaddr;           /* Server IP */
    uint32_t giaddr;           /* Gateway IP */
    uint8_t  chaddr[16];       /* Client hardware address */
    uint8_t  sname[64];        /* Server name */
    uint8_t  file[128];        /* Boot filename */
    uint32_t magic_cookie;     /* 0x63825363 */
} __attribute__((packed)) DhcpHeader;

#define DHCP_MAGIC_COOKIE  0x63538263  /* Network byte order of 0x63825363 */
#define DHCP_CLIENT_PORT   68
#define DHCP_SERVER_PORT   67

/* DHCP option codes */
#define DHCP_OPT_SUBNET    1
#define DHCP_OPT_ROUTER    3
#define DHCP_OPT_DNS       6
#define DHCP_OPT_REQIP     50
#define DHCP_OPT_MSGTYPE   53
#define DHCP_OPT_SERVERID  54
#define DHCP_OPT_END       255

/* DHCP message types */
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5

/* DNS header */
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;          /* Number of questions */
    uint16_t ancount;          /* Number of answers */
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) DnsHeader;

/* ARP cache entry */
typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    bool     valid;
} ArpEntry;

/* ============================================================
 * Virtio-net Driver API
 * ============================================================ */

bool virtio_net_init(void);
bool virtio_net_send(const void *data, uint32_t len);
int  virtio_net_receive(void *buf, uint32_t max_len);
void virtio_net_get_mac(uint8_t *mac);

/* ============================================================
 * Network Stack API
 * ============================================================ */

/* Initialization */
bool net_init(void);
void net_poll(void);

/* Configuration */
uint32_t net_get_ip(void);
uint32_t net_get_gateway(void);
uint32_t net_get_netmask(void);
uint32_t net_get_dns(void);
void     net_get_mac(uint8_t *mac);
bool     net_is_up(void);

/* High-level operations */
bool net_dhcp_request(void);
int  net_ping(uint32_t dst_ip);    /* Returns poll-cycles RTT, or -1 on timeout */
bool net_dns_resolve(const char *hostname, uint32_t *out_ip);

/* Lower-level sending */
bool net_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, uint32_t len);

/* ARP */
bool net_arp_resolve(uint32_t ip, uint8_t *out_mac);

/* Internal packet handler (called from poll) */
void net_process_packet(const void *data, uint32_t len);

/* ============================================================
 * IP address formatting helper
 * ============================================================ */

void net_ip_to_str(uint32_t ip_net_order, char *buf);
uint32_t net_str_to_ip(const char *str);
