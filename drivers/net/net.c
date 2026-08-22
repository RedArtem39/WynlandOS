/*
 * WynlandOS - Network Protocol Stack
 *
 * Phase 5: Full network stack — Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS
 *
 * Implements: net_init, net_poll, net_process_packet, net_dhcp_request,
 *             net_ping, net_dns_resolve, net_send_udp, net_arp_resolve,
 *             and all configuration getters.
 */

#include <wynland/net.h>
#include <wynland/virtio.h>
#include <wynland/types.h>
#include <wynland/tcp.h>
#include <wynland/udpsock.h>

/* ============================================================
 * External helpers (provided by the kernel elsewhere)
 * ============================================================ */

extern void serial_write_string(const char *s);
extern void uint_to_str(uint32_t val, char *buf);
extern void uint_to_hex(uint32_t val, char *buf);

/* ============================================================
 * Static state
 * ============================================================ */

static uint8_t  our_mac[ETH_ALEN];
static uint32_t our_ip;
static uint32_t our_netmask;
static uint32_t our_gateway;
static uint32_t our_dns;

static ArpEntry arp_cache[ARP_CACHE_SIZE];

static bool     network_up;
static volatile bool ping_reply_received;
static volatile bool dhcp_offer_received;
static volatile bool dhcp_ack_received;
static volatile bool dns_reply_received;
static uint32_t dns_resolved_ip;

static uint16_t ip_id_counter;

static uint8_t  tx_buf[NET_PKT_BUF];

static uint32_t dhcp_offered_ip;
static uint32_t dhcp_server_id;

/* ============================================================
 * Forward declarations of internal handlers
 * ============================================================ */

static void handle_arp(const uint8_t *data, uint32_t len);
static void handle_ipv4(const uint8_t *data, uint32_t len, const uint8_t *eth_frame);
static void handle_icmp(const uint8_t *data, uint32_t len, const Ipv4Header *ip_hdr);
static void handle_udp(const uint8_t *data, uint32_t len, const Ipv4Header *ip_hdr);
static void handle_dhcp(const uint8_t *data, uint32_t len);
static void handle_dns_response(const uint8_t *data, uint32_t len);

uint16_t ip_checksum(const void *data, uint32_t len);
static bool net_send_raw_eth(const uint8_t *dst_mac, const uint8_t *src_mac,
                             uint16_t type, const void *payload, uint32_t payload_len);
bool net_send_ipv4(uint32_t dst_ip, uint8_t protocol,
                   const void *payload, uint32_t len);

/* ============================================================
 * IP checksum — standard ones-complement sum
 * ============================================================ */

uint16_t ip_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2) {
        uint16_t word = (uint16_t)((uint16_t)p[i] << 8 | p[i + 1]);
        sum += word;
    }
    if (len & 1) {
        sum += (uint16_t)p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* ============================================================
 * Initialization / poll
 * ============================================================ */

bool net_init(void)
{
    virtio_net_init();
    virtio_net_get_mac(our_mac);

    our_ip      = 0;
    our_netmask = 0;
    our_gateway = 0;
    our_dns     = 0;
    network_up  = false;
    ip_id_counter = 0;

    memset(arp_cache, 0, sizeof(arp_cache));

    serial_write_string("[NET] Network stack initialised\n");
    return true;
}

void net_poll(void)
{
    static uint8_t rx_buf[NET_PKT_BUF];
    int received;

    for (;;) {
        received = virtio_net_receive(rx_buf, sizeof(rx_buf));
        if (received <= 0)
            break;
        net_process_packet(rx_buf, (uint32_t)received);
    }
}

/* ============================================================
 * Getters
 * ============================================================ */

uint32_t net_get_ip(void)      { return our_ip; }
uint32_t net_get_gateway(void) { return our_gateway; }
uint32_t net_get_netmask(void) { return our_netmask; }
uint32_t net_get_dns(void)     { return our_dns; }

void net_get_mac(uint8_t *mac)
{
    memcpy(mac, our_mac, ETH_ALEN);
}

bool net_is_up(void)
{
    return network_up;
}

/* ============================================================
 * Packet dispatch
 * ============================================================ */

void net_process_packet(const void *data, uint32_t len)
{
    const EthHeader *eth;

    if (len < sizeof(EthHeader))
        return;

    eth = (const EthHeader *)data;

    switch (ntohs(eth->type)) {
    case ETH_TYPE_ARP:
        handle_arp((const uint8_t *)data + ETH_HLEN, len - ETH_HLEN);
        break;
    case ETH_TYPE_IPV4:
        handle_ipv4((const uint8_t *)data + ETH_HLEN, len - ETH_HLEN,
                    (const uint8_t *)data);
        break;
    default:
        break;
    }
}

/* ============================================================
 * ARP handling
 * ============================================================ */

static void handle_arp(const uint8_t *data, uint32_t len)
{
    const ArpHeader *arp;
    uint32_t i;

    if (len < sizeof(ArpHeader))
        return;

    arp = (const ArpHeader *)data;

    if (ntohs(arp->opcode) == ARP_OP_REQUEST && arp->target_ip == our_ip) {
        /* Build ARP reply */
        uint8_t frame[ETH_HLEN + sizeof(ArpHeader)];
        EthHeader *eth_reply = (EthHeader *)frame;
        ArpHeader *arp_reply = (ArpHeader *)(frame + ETH_HLEN);

        memcpy(eth_reply->dst, arp->sender_mac, ETH_ALEN);
        memcpy(eth_reply->src, our_mac, ETH_ALEN);
        eth_reply->type = htons(ETH_TYPE_ARP);

        arp_reply->hw_type    = htons(1);
        arp_reply->proto_type = htons(ETH_TYPE_IPV4);
        arp_reply->hw_len     = ETH_ALEN;
        arp_reply->proto_len  = 4;
        arp_reply->opcode     = htons(ARP_OP_REPLY);
        memcpy(arp_reply->sender_mac, our_mac, ETH_ALEN);
        arp_reply->sender_ip  = our_ip;
        memcpy(arp_reply->target_mac, arp->sender_mac, ETH_ALEN);
        arp_reply->target_ip  = arp->sender_ip;

        virtio_net_send(frame, sizeof(frame));

    } else if (ntohs(arp->opcode) == ARP_OP_REPLY) {
        /* Update ARP cache */
        /* First check if entry already exists */
        for (i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && arp_cache[i].ip == arp->sender_ip) {
                memcpy(arp_cache[i].mac, arp->sender_mac, ETH_ALEN);
                return;
            }
        }
        /* Find empty slot */
        for (i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!arp_cache[i].valid) {
                arp_cache[i].ip    = arp->sender_ip;
                memcpy(arp_cache[i].mac, arp->sender_mac, ETH_ALEN);
                arp_cache[i].valid = true;
                return;
            }
        }
        /* Cache full — overwrite first entry */
        arp_cache[0].ip    = arp->sender_ip;
        memcpy(arp_cache[0].mac, arp->sender_mac, ETH_ALEN);
        arp_cache[0].valid = true;
    }
}

/* ============================================================
 * IPv4 handling
 * ============================================================ */

static void handle_ipv4(const uint8_t *data, uint32_t len,
                        const uint8_t *eth_frame)
{
    const Ipv4Header *ip_hdr;
    uint32_t ihl;
    uint32_t payload_len;
    const uint8_t *payload;

    (void)eth_frame;

    if (len < sizeof(Ipv4Header))
        return;

    ip_hdr = (const Ipv4Header *)data;

    /* Check IPv4 version */
    if ((ip_hdr->ver_ihl >> 4) != 4)
        return;

    ihl = (uint32_t)(ip_hdr->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len)
        return;

    /* Use the IP header's own total_length, not the raw Ethernet frame
       length -- small packets (bare ACKs, FINs) get zero-padded by the
       sender/NIC to Ethernet's 60-byte minimum frame size, and treating
       that padding as real IP payload silently corrupts every short
       segment's payload_len (and whatever protocol handler reads past
       the real packet boundary into the padding zeros). */
    {
        uint32_t ip_total_len = (uint32_t)ntohs(ip_hdr->total_length);
        if (ip_total_len < ihl)
            return; /* malformed */
        if (ip_total_len > len)
            ip_total_len = len; /* truncated capture safety clamp */
        payload     = data + ihl;
        payload_len = ip_total_len - ihl;
    }

    switch (ip_hdr->protocol) {
    case IP_PROTO_ICMP:
        handle_icmp(payload, payload_len, ip_hdr);
        break;
    case IP_PROTO_TCP:
        tcp_handle_packet(ip_hdr->src_ip, ip_hdr->dst_ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        handle_udp(payload, payload_len, ip_hdr);
        break;
    default:
        break;
    }
}

/* ============================================================
 * ICMP handling
 * ============================================================ */

static void handle_icmp(const uint8_t *data, uint32_t len,
                        const Ipv4Header *ip_hdr)
{
    const IcmpHeader *icmp;

    if (len < sizeof(IcmpHeader))
        return;

    icmp = (const IcmpHeader *)data;

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0) {
        /* Build echo reply */
        uint32_t total_ip_len = sizeof(Ipv4Header) + len;
        uint8_t  reply_buf[ETH_HLEN + sizeof(Ipv4Header) + NET_PKT_BUF];
        EthHeader  *eth_r;
        Ipv4Header *ip_r;
        IcmpHeader *icmp_r;
        uint8_t dst_mac[ETH_ALEN];

        if (len > NET_PKT_BUF)
            return;

        /* Resolve destination MAC */
        if (!net_arp_resolve(ip_hdr->src_ip, dst_mac))
            return;

        eth_r = (EthHeader *)reply_buf;
        memcpy(eth_r->dst, dst_mac, ETH_ALEN);
        memcpy(eth_r->src, our_mac, ETH_ALEN);
        eth_r->type = htons(ETH_TYPE_IPV4);

        ip_r = (Ipv4Header *)(reply_buf + ETH_HLEN);
        ip_r->ver_ihl      = 0x45;
        ip_r->tos           = 0;
        ip_r->total_length  = htons((uint16_t)total_ip_len);
        ip_r->id            = htons(ip_id_counter++);
        ip_r->flags_frag    = 0;
        ip_r->ttl           = 64;
        ip_r->protocol      = IP_PROTO_ICMP;
        ip_r->checksum      = 0;
        ip_r->src_ip        = our_ip;
        ip_r->dst_ip        = ip_hdr->src_ip;
        ip_r->checksum      = htons(ip_checksum(ip_r, sizeof(Ipv4Header)));

        icmp_r = (IcmpHeader *)(reply_buf + ETH_HLEN + sizeof(Ipv4Header));
        memcpy(icmp_r, data, len);
        icmp_r->type     = ICMP_TYPE_ECHO_REPLY;
        icmp_r->code     = 0;
        icmp_r->checksum = 0;
        icmp_r->checksum = htons(ip_checksum(icmp_r, len));

        virtio_net_send(reply_buf, ETH_HLEN + total_ip_len);

    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        ping_reply_received = true;
    }
}

/* ============================================================
 * UDP handling
 * ============================================================ */

static void handle_udp(const uint8_t *data, uint32_t len,
                       const Ipv4Header *ip_hdr)
{
    const UdpHeader *udp;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t udp_payload_len;
    const uint8_t *udp_payload;

    if (len < sizeof(UdpHeader))
        return;

    udp = (const UdpHeader *)data;
    dst_port = ntohs(udp->dst_port);
    src_port = ntohs(udp->src_port);

    udp_payload     = data + sizeof(UdpHeader);
    udp_payload_len = len - sizeof(UdpHeader);

    if (dst_port == DHCP_CLIENT_PORT) {
        handle_dhcp(udp_payload, udp_payload_len);
    } else if (src_port == 53 && dst_port == 12345) {
        /* The OS's own dedicated Ring-0 DNS client (net_dns_resolve())
           always queries from this fixed local port -- kept as its own
           narrow check (not just "src_port == 53") so a real userspace
           resolver's own DNS query, sent from a completely different
           ephemeral port via udp_socket_*, doesn't get misrouted into
           this legacy singleton-state handler instead of its own socket. */
        handle_dns_response(udp_payload, udp_payload_len);
    } else {
        udp_socket_deliver(ip_hdr->src_ip, src_port, dst_port,
                           udp_payload, udp_payload_len);
    }
}

/* ============================================================
 * Raw Ethernet send
 * ============================================================ */

static bool net_send_raw_eth(const uint8_t *dst_mac, const uint8_t *src_mac,
                             uint16_t type, const void *payload,
                             uint32_t payload_len)
{
    EthHeader *eth;
    uint32_t frame_len = ETH_HLEN + payload_len;

    if (frame_len > sizeof(tx_buf))
        return false;

    eth = (EthHeader *)tx_buf;
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, src_mac, ETH_ALEN);
    eth->type = htons(type);
    memcpy(tx_buf + ETH_HLEN, payload, payload_len);

    return virtio_net_send(tx_buf, frame_len);
}

/* ============================================================
 * IPv4 send helper
 * ============================================================ */

bool net_send_ipv4(uint32_t dst_ip, uint8_t protocol,
                   const void *payload, uint32_t len)
{
    uint8_t ip_pkt[sizeof(Ipv4Header) + NET_PKT_BUF];
    Ipv4Header *ip_hdr;
    uint32_t total_len = sizeof(Ipv4Header) + len;
    uint8_t dst_mac[ETH_ALEN];

    if (len > NET_PKT_BUF)
        return false;

    /* Resolve destination MAC */
    if (!net_arp_resolve(dst_ip, dst_mac))
        return false;

    ip_hdr = (Ipv4Header *)ip_pkt;
    ip_hdr->ver_ihl      = 0x45;
    ip_hdr->tos           = 0;
    ip_hdr->total_length  = htons((uint16_t)total_len);
    ip_hdr->id            = htons(ip_id_counter++);
    ip_hdr->flags_frag    = 0;
    ip_hdr->ttl           = 64;
    ip_hdr->protocol      = protocol;
    ip_hdr->checksum      = 0;
    ip_hdr->src_ip        = our_ip;
    ip_hdr->dst_ip        = dst_ip;
    ip_hdr->checksum      = htons(ip_checksum(ip_hdr, sizeof(Ipv4Header)));

    memcpy(ip_pkt + sizeof(Ipv4Header), payload, len);

    return net_send_raw_eth(dst_mac, our_mac, ETH_TYPE_IPV4, ip_pkt, total_len);
}

/* ============================================================
 * UDP send
 * ============================================================ */

bool net_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, uint32_t len)
{
    uint8_t udp_pkt[sizeof(UdpHeader) + NET_PKT_BUF];
    UdpHeader *udp;
    uint32_t udp_total = sizeof(UdpHeader) + len;

    if (len > NET_PKT_BUF)
        return false;

    udp = (UdpHeader *)udp_pkt;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)udp_total);
    udp->checksum = 0;  /* UDP checksum is optional in IPv4 */

    memcpy(udp_pkt + sizeof(UdpHeader), data, len);

    return net_send_ipv4(dst_ip, IP_PROTO_UDP, udp_pkt, udp_total);
}

/* ============================================================
 * ARP resolution
 * ============================================================ */

bool net_arp_resolve(uint32_t ip, uint8_t *out_mac)
{
    uint32_t i;
    uint32_t target_ip;
    uint8_t broadcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t arp_pkt[sizeof(ArpHeader)];
    ArpHeader *arp;

    /* Check ARP cache first */
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(out_mac, arp_cache[i].mac, ETH_ALEN);
            return true;
        }
    }

    /* Determine actual target: if different subnet, resolve gateway */
    if ((ip & our_netmask) != (our_ip & our_netmask)) {
        target_ip = our_gateway;
        /* Check cache for gateway too */
        for (i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].valid && arp_cache[i].ip == target_ip) {
                memcpy(out_mac, arp_cache[i].mac, ETH_ALEN);
                return true;
            }
        }
    } else {
        target_ip = ip;
    }

    /* Build ARP request */
    arp = (ArpHeader *)arp_pkt;
    arp->hw_type    = htons(1);
    arp->proto_type = htons(ETH_TYPE_IPV4);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->opcode     = htons(ARP_OP_REQUEST);
    memcpy(arp->sender_mac, our_mac, ETH_ALEN);
    arp->sender_ip  = our_ip;
    memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip  = target_ip;

    net_send_raw_eth(broadcast_mac, our_mac, ETH_TYPE_ARP,
                     arp_pkt, sizeof(ArpHeader));

    /* Poll for reply */
    for (i = 0; i < 500000; i++) {
        net_poll();

        /* Check if the target appeared in the cache */
        {
            uint32_t j;
            for (j = 0; j < ARP_CACHE_SIZE; j++) {
                if (arp_cache[j].valid && arp_cache[j].ip == target_ip) {
                    memcpy(out_mac, arp_cache[j].mac, ETH_ALEN);
                    return true;
                }
            }
        }
    }

    return false;
}

/* ============================================================
 * DHCP flow
 * ============================================================ */

static void handle_dhcp(const uint8_t *data, uint32_t len)
{
    const DhcpHeader *dhcp;
    const uint8_t *opts;
    uint32_t opts_len;
    uint8_t msg_type = 0;

    if (len < sizeof(DhcpHeader))
        return;

    dhcp = (const DhcpHeader *)data;

    /* Only process replies */
    if (dhcp->op != 2)
        return;

    /* Parse options after magic cookie */
    opts     = data + sizeof(DhcpHeader);
    opts_len = len  - sizeof(DhcpHeader);

    {
        uint32_t offset = 0;
        while (offset < opts_len) {
            uint8_t code = opts[offset];
            uint8_t opt_len;

            if (code == DHCP_OPT_END)
                break;
            if (code == 0) {  /* Pad */
                offset++;
                continue;
            }

            if (offset + 1 >= opts_len)
                break;
            opt_len = opts[offset + 1];
            if (offset + 2 + opt_len > opts_len)
                break;

            switch (code) {
            case DHCP_OPT_MSGTYPE:
                if (opt_len >= 1)
                    msg_type = opts[offset + 2];
                break;
            case DHCP_OPT_SUBNET:
                if (opt_len >= 4)
                    memcpy(&our_netmask, &opts[offset + 2], 4);
                break;
            case DHCP_OPT_ROUTER:
                if (opt_len >= 4)
                    memcpy(&our_gateway, &opts[offset + 2], 4);
                break;
            case DHCP_OPT_DNS:
                if (opt_len >= 4)
                    memcpy(&our_dns, &opts[offset + 2], 4);
                break;
            case DHCP_OPT_SERVERID:
                if (opt_len >= 4)
                    memcpy(&dhcp_server_id, &opts[offset + 2], 4);
                break;
            default:
                break;
            }

            offset += 2 + opt_len;
        }
    }

    if (msg_type == DHCP_OFFER) {
        dhcp_offered_ip = dhcp->yiaddr;
        dhcp_offer_received = true;
        serial_write_string("[DHCP] Offer received\n");

    } else if (msg_type == DHCP_ACK) {
        our_ip = dhcp->yiaddr;
        /* netmask, gateway, dns already parsed from options */
        dhcp_ack_received = true;
        network_up = true;
        serial_write_string("[DHCP] ACK received — network up\n");
    }
}

bool net_dhcp_request(void)
{
    /*
     * We build DHCP frames manually because we have no IP yet —
     * can't use net_send_udp (which calls net_send_ipv4 / ARP resolve).
     */
    uint8_t frame[ETH_HLEN + sizeof(Ipv4Header) + sizeof(UdpHeader) +
                  sizeof(DhcpHeader) + 64];
    EthHeader  *eth;
    Ipv4Header *ip;
    UdpHeader  *udp;
    DhcpHeader *dhcp;
    uint8_t    *opts;
    uint32_t    frame_len;
    uint32_t    dhcp_len;
    uint32_t    udp_len;
    uint32_t    ip_total;
    uint32_t    opt_offset;
    uint32_t    i;

    /* ---- DHCPDISCOVER ---- */

    memset(frame, 0, sizeof(frame));

    eth = (EthHeader *)frame;
    memset(eth->dst, 0xFF, ETH_ALEN);   /* broadcast */
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IPV4);

    dhcp = (DhcpHeader *)(frame + ETH_HLEN + sizeof(Ipv4Header) + sizeof(UdpHeader));
    dhcp->op           = 1;   /* BOOTREQUEST */
    dhcp->htype        = 1;   /* Ethernet */
    dhcp->hlen         = 6;
    dhcp->hops         = 0;
    dhcp->xid          = htonl(0x12345678);
    dhcp->secs         = 0;
    dhcp->flags        = htons(0x8000);  /* Broadcast flag */
    dhcp->ciaddr       = 0;
    dhcp->yiaddr       = 0;
    dhcp->siaddr       = 0;
    dhcp->giaddr       = 0;
    memcpy(dhcp->chaddr, our_mac, ETH_ALEN);
    dhcp->magic_cookie = htonl(0x63825363);

    opts = (uint8_t *)(dhcp) + sizeof(DhcpHeader);
    opt_offset = 0;

    /* Option 53: DHCP Message Type = DISCOVER (1) */
    opts[opt_offset++] = DHCP_OPT_MSGTYPE;
    opts[opt_offset++] = 1;
    opts[opt_offset++] = DHCP_DISCOVER;

    /* Option 255: End */
    opts[opt_offset++] = DHCP_OPT_END;

    dhcp_len  = sizeof(DhcpHeader) + opt_offset;
    udp_len   = sizeof(UdpHeader) + dhcp_len;
    ip_total  = sizeof(Ipv4Header) + udp_len;
    frame_len = ETH_HLEN + ip_total;

    udp = (UdpHeader *)(frame + ETH_HLEN + sizeof(Ipv4Header));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons((uint16_t)udp_len);
    udp->checksum = 0;

    ip = (Ipv4Header *)(frame + ETH_HLEN);
    ip->ver_ihl     = 0x45;
    ip->tos          = 0;
    ip->total_length = htons((uint16_t)ip_total);
    ip->id           = htons(ip_id_counter++);
    ip->flags_frag   = 0;
    ip->ttl          = 64;
    ip->protocol     = IP_PROTO_UDP;
    ip->checksum     = 0;
    ip->src_ip       = 0;
    ip->dst_ip       = 0xFFFFFFFF;
    ip->checksum     = htons(ip_checksum(ip, sizeof(Ipv4Header)));

    dhcp_offer_received = false;
    dhcp_ack_received   = false;

    serial_write_string("[DHCP] Sending DISCOVER\n");
    virtio_net_send(frame, frame_len);

    /* Poll for DHCPOFFER */
    for (i = 0; i < 3000000; i++) {
        net_poll();
        if (dhcp_offer_received)
            break;
    }
    if (!dhcp_offer_received) {
        serial_write_string("[DHCP] No OFFER received — timeout\n");
        return false;
    }

    /* ---- DHCPREQUEST ---- */

    memset(frame, 0, sizeof(frame));

    eth = (EthHeader *)frame;
    memset(eth->dst, 0xFF, ETH_ALEN);
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IPV4);

    dhcp = (DhcpHeader *)(frame + ETH_HLEN + sizeof(Ipv4Header) + sizeof(UdpHeader));
    dhcp->op           = 1;
    dhcp->htype        = 1;
    dhcp->hlen         = 6;
    dhcp->hops         = 0;
    dhcp->xid          = htonl(0x12345678);
    dhcp->secs         = 0;
    dhcp->flags        = htons(0x8000);
    dhcp->ciaddr       = 0;
    dhcp->yiaddr       = 0;
    dhcp->siaddr       = 0;
    dhcp->giaddr       = 0;
    memcpy(dhcp->chaddr, our_mac, ETH_ALEN);
    dhcp->magic_cookie = htonl(0x63825363);

    opts = (uint8_t *)(dhcp) + sizeof(DhcpHeader);
    opt_offset = 0;

    /* Option 53: DHCP Message Type = REQUEST (3) */
    opts[opt_offset++] = DHCP_OPT_MSGTYPE;
    opts[opt_offset++] = 1;
    opts[opt_offset++] = DHCP_REQUEST;

    /* Option 50: Requested IP */
    opts[opt_offset++] = DHCP_OPT_REQIP;
    opts[opt_offset++] = 4;
    memcpy(&opts[opt_offset], &dhcp_offered_ip, 4);
    opt_offset += 4;

    /* Option 54: Server Identifier */
    opts[opt_offset++] = DHCP_OPT_SERVERID;
    opts[opt_offset++] = 4;
    memcpy(&opts[opt_offset], &dhcp_server_id, 4);
    opt_offset += 4;

    /* Option 255: End */
    opts[opt_offset++] = DHCP_OPT_END;

    dhcp_len  = sizeof(DhcpHeader) + opt_offset;
    udp_len   = sizeof(UdpHeader) + dhcp_len;
    ip_total  = sizeof(Ipv4Header) + udp_len;
    frame_len = ETH_HLEN + ip_total;

    udp = (UdpHeader *)(frame + ETH_HLEN + sizeof(Ipv4Header));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons((uint16_t)udp_len);
    udp->checksum = 0;

    ip = (Ipv4Header *)(frame + ETH_HLEN);
    ip->ver_ihl     = 0x45;
    ip->tos          = 0;
    ip->total_length = htons((uint16_t)ip_total);
    ip->id           = htons(ip_id_counter++);
    ip->flags_frag   = 0;
    ip->ttl          = 64;
    ip->protocol     = IP_PROTO_UDP;
    ip->checksum     = 0;
    ip->src_ip       = 0;
    ip->dst_ip       = 0xFFFFFFFF;
    ip->checksum     = htons(ip_checksum(ip, sizeof(Ipv4Header)));

    serial_write_string("[DHCP] Sending REQUEST\n");
    virtio_net_send(frame, frame_len);

    /* Poll for DHCPACK */
    for (i = 0; i < 3000000; i++) {
        net_poll();
        if (dhcp_ack_received)
            break;
    }
    if (!dhcp_ack_received) {
        serial_write_string("[DHCP] No ACK received — timeout\n");
        return false;
    }

    serial_write_string("[DHCP] Network configured successfully\n");
    return true;
}

/* ============================================================
 * ICMP Ping
 * ============================================================ */

int net_ping(uint32_t dst_ip)
{
    static uint16_t ping_seq = 0;

    uint8_t icmp_pkt[sizeof(IcmpHeader)];
    IcmpHeader *icmp;
    uint32_t i;

    icmp = (IcmpHeader *)icmp_pkt;
    icmp->type     = ICMP_TYPE_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x1234);
    icmp->seq      = htons(ping_seq++);
    icmp->checksum = htons(ip_checksum(icmp, sizeof(IcmpHeader)));

    ping_reply_received = false;

    if (!net_send_ipv4(dst_ip, IP_PROTO_ICMP, icmp_pkt, sizeof(IcmpHeader)))
        return -1;

    for (i = 0; i < 2000000; i++) {
        net_poll();
        if (ping_reply_received)
            return (int)i;
    }

    return -1;
}

/* ============================================================
 * DNS resolution
 * ============================================================ */

static void handle_dns_response(const uint8_t *data, uint32_t len)
{
    const DnsHeader *dns;
    uint32_t offset;
    uint16_t ancount;
    uint16_t qdcount;
    uint16_t i;

    if (len < sizeof(DnsHeader))
        return;

    dns = (const DnsHeader *)data;
    ancount = ntohs(dns->ancount);
    qdcount = ntohs(dns->qdcount);

    /* Skip past the header */
    offset = sizeof(DnsHeader);

    /* Skip question section */
    for (i = 0; i < qdcount; i++) {
        /* Skip QNAME (label format) */
        while (offset < len) {
            uint8_t label_len = data[offset];
            if (label_len == 0) {
                offset++;  /* skip the zero terminator */
                break;
            }
            if ((label_len & 0xC0) == 0xC0) {
                /* Compressed pointer — 2 bytes */
                offset += 2;
                break;
            }
            offset += 1 + label_len;
        }
        /* Skip QTYPE (2) + QCLASS (2) */
        offset += 4;
    }

    /* Parse answer records */
    for (i = 0; i < ancount; i++) {
        uint16_t rtype;
        uint16_t rdlength;

        if (offset >= len)
            break;

        /* Skip NAME (may be compressed) */
        while (offset < len) {
            uint8_t label_len = data[offset];
            if (label_len == 0) {
                offset++;
                break;
            }
            if ((label_len & 0xC0) == 0xC0) {
                offset += 2;
                break;
            }
            offset += 1 + label_len;
        }

        /* TYPE (2) + CLASS (2) + TTL (4) + RDLENGTH (2) = 10 bytes */
        if (offset + 10 > len)
            break;

        rtype    = (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
        offset  += 2;  /* skip TYPE */
        offset  += 2;  /* skip CLASS */
        offset  += 4;  /* skip TTL */
        rdlength = (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
        offset  += 2;  /* skip RDLENGTH */

        if (rtype == 1 && rdlength == 4 && offset + 4 <= len) {
            /* A record — 4-byte IPv4 address */
            memcpy(&dns_resolved_ip, &data[offset], 4);
            dns_reply_received = true;
            return;
        }

        offset += rdlength;
    }
}

bool net_dns_resolve(const char *hostname, uint32_t *out_ip)
{
    uint8_t dns_pkt[512];
    DnsHeader *dns;
    uint32_t offset;
    const char *p;
    uint32_t i;

    memset(dns_pkt, 0, sizeof(dns_pkt));

    dns = (DnsHeader *)dns_pkt;
    dns->id      = htons(0xABCD);
    dns->flags   = htons(0x0100);  /* Standard query, recursion desired */
    dns->qdcount = htons(1);
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    offset = sizeof(DnsHeader);

    /* Encode hostname as DNS label format: [len][chars]...[0] */
    p = hostname;
    while (*p) {
        const char *dot;
        uint32_t label_len;

        /* Find next dot or end of string */
        dot = p;
        while (*dot && *dot != '.')
            dot++;
        label_len = (uint32_t)(dot - p);

        if (label_len == 0 || offset + 1 + label_len >= sizeof(dns_pkt) - 5)
            return false;

        dns_pkt[offset++] = (uint8_t)label_len;
        memcpy(&dns_pkt[offset], p, label_len);
        offset += label_len;

        if (*dot == '.')
            p = dot + 1;
        else
            p = dot;
    }
    dns_pkt[offset++] = 0;  /* Terminate QNAME */

    /* QTYPE = A (1) */
    dns_pkt[offset++] = 0;
    dns_pkt[offset++] = 1;

    /* QCLASS = IN (1) */
    dns_pkt[offset++] = 0;
    dns_pkt[offset++] = 1;

    dns_reply_received = false;
    dns_resolved_ip    = 0;

    /* Send DNS query as UDP to our DNS server, port 53 */
    if (!net_send_udp(our_dns, 12345, 53, dns_pkt, offset))
        return false;

    /* Poll for response */
    for (i = 0; i < 2000000; i++) {
        net_poll();
        if (dns_reply_received) {
            *out_ip = dns_resolved_ip;
            return true;
        }
    }

    serial_write_string("[DNS] Resolution timeout\n");
    return false;
}

/* ============================================================
 * IP address formatting
 * ============================================================ */

void net_ip_to_str(uint32_t ip_net_order, char *buf)
{
    uint8_t *b = (uint8_t *)&ip_net_order;
    uint32_t pos = 0;
    uint32_t octet;
    int i;

    for (i = 0; i < 4; i++) {
        octet = b[i];

        if (octet >= 100) {
            buf[pos++] = (char)('0' + octet / 100);
            buf[pos++] = (char)('0' + (octet / 10) % 10);
            buf[pos++] = (char)('0' + octet % 10);
        } else if (octet >= 10) {
            buf[pos++] = (char)('0' + octet / 10);
            buf[pos++] = (char)('0' + octet % 10);
        } else {
            buf[pos++] = (char)('0' + octet);
        }

        if (i < 3)
            buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

uint32_t net_str_to_ip(const char *str)
{
    uint8_t octets[4];
    int i;
    uint32_t val;
    uint32_t result;

    for (i = 0; i < 4; i++) {
        val = 0;
        while (*str >= '0' && *str <= '9') {
            val = val * 10 + (uint32_t)(*str - '0');
            str++;
        }
        octets[i] = (uint8_t)val;
        if (i < 3 && *str == '.')
            str++;
    }

    memcpy(&result, octets, 4);
    return result;
}
