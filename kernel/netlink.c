/*
 * WynlandOS - netlink sockets: NETLINK_ROUTE and NETLINK_SOCK_DIAG.
 *
 * On the machine's network, rtnetlink describes what there is: "lo"
 * (127.0.0.1/8) and the network card -- eth0, or wlan0 for Wi-Fi -- with
 * its DHCP address and the routes through its gateway. glibc reads it
 * (getifaddrs, getaddrinfo's AI_ADDRCONFIG), and so do ip, ss, Node's
 * os.networkInterfaces(), Chromium. Changing anything is not offered
 * (EOPNOTSUPP): DHCP sets the network up.
 *
 * sock_diag lists the TCP sockets (include/wynland/inet_list.h): the IP
 * stack's connections and the loopback ones -- ss -t, ss -l.
 *
 * In an isolated network namespace there is nothing to configure:
 * bubblewrap brings up "lo" there (RTM_NEWADDR 127.0.0.1, RTM_NEWLINK up)
 * and waits for the acknowledgements, so every request is acknowledged
 * (error 0) and a dump answers an empty list.
 *
 * Replies are datagrams, as Linux's: a read takes one (what does not fit
 * the buffer is dropped, MSG_TRUNC says how long it was; MSG_PEEK leaves
 * it -- iproute2 peeks first to size its buffer). No multicast groups,
 * no notifications.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/heap.h>
#include <wynland/inet_list.h>

extern uint32_t net_get_ip(void);
extern uint32_t net_get_gateway(void);
extern uint32_t net_get_netmask(void);
extern void     net_get_mac(uint8_t *mac);
extern bool     net_is_up(void);

/* (the kernel has no strlen/memcmp/memmove) */
static uint32_t nl_strlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static bool nl_eq(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t k = 0; k < n; k++) if (x[k] != y[k]) return false;
    return true;
}

#define NL_SLOTS 32
#define NL_CAP   65536            /* replies waiting, per socket */
#define NL_MAXDG 32768            /* one datagram (a dump's batch) */

#define NLMSG_ERROR 2
#define NLMSG_DONE  3
#define NLM_F_REQUEST 0x1
#define NLM_F_MULTI   0x2
#define NLM_F_ACK     0x4
#define NLM_F_DUMP    0x300

#define RTM_NEWLINK  16
#define RTM_GETLINK  18
#define RTM_NEWADDR  20
#define RTM_GETADDR  22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define SOCK_DIAG_BY_FAMILY 20
#define TCPDIAG_GETSOCK     18

#define AF_INET_ 2

typedef struct {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
} NlHdr;

static struct {
    uint32_t refs;
    uint32_t portid;         /* answers carry it (glibc drops any that do not) */
    int      proto;          /* 0 NETLINK_ROUTE, 4 NETLINK_SOCK_DIAG */
    bool     host;           /* the machine's network (not a namespace's) */
    uint8_t *buf;            /* datagrams: [u32 length][messages], one after another */
    uint32_t len;
} g_nl[NL_SLOTS];

int netlink_create(uint32_t portid, int proto, bool host) {
    for (int i = 0; i < NL_SLOTS; i++)
        if (!g_nl[i].refs) {
            if (!g_nl[i].buf && !(g_nl[i].buf = (uint8_t *)kmalloc(NL_CAP))) return -12;
            g_nl[i].refs = 1; g_nl[i].len = 0; g_nl[i].portid = portid;
            g_nl[i].proto = proto; g_nl[i].host = host;
            return i;
        }
    return -23;   /* -ENFILE */
}

void netlink_ref(int i) { if (i >= 0 && i < NL_SLOTS) g_nl[i].refs++; }
void netlink_unref(int i) { if (i >= 0 && i < NL_SLOTS && g_nl[i].refs) g_nl[i].refs--; }

bool netlink_readable(int i) { return i >= 0 && i < NL_SLOTS && g_nl[i].len > 0; }

/* ---------------------------------------------------------------- building
   One datagram at a time: dg_begin, then messages (msg_begin, the body,
   attributes, msg_end), then dg_end queues it. Out of room: the rest is
   dropped and the datagram ends there (a dump stays well inside). */

static struct {
    int      slot;
    uint8_t *base;           /* the datagram's length word */
    uint32_t used;           /* bytes of messages */
    uint32_t room;
    uint32_t msg;            /* the open message's offset */
    bool     full;
} B;

static bool dg_begin(int i) {
    uint32_t free = NL_CAP - g_nl[i].len;
    if (free < 4 + 64) return false;
    B.slot = i;
    B.base = g_nl[i].buf + g_nl[i].len;
    B.used = 0;
    B.room = free - 4 < NL_MAXDG ? free - 4 : NL_MAXDG;
    B.full = false;
    return true;
}

static void dg_end(void) {
    if (!B.used) return;
    memcpy(B.base, &B.used, 4);
    g_nl[B.slot].len += 4 + B.used;
}

static void *put(uint32_t n) {
    uint32_t a = (n + 3) & ~3u;
    if (B.full || B.used + a > B.room) { B.full = true; return NULL; }
    uint8_t *p = B.base + 4 + B.used;
    memset(p, 0, a);
    B.used += a;
    return p;
}

static void *msg_begin(const NlHdr *req, uint16_t type, uint16_t flags, uint32_t body) {
    uint32_t at = B.used;
    NlHdr *h = (NlHdr *)put(16);
    void *b = put(body);
    if (!h || !b) { B.used = at; return NULL; }
    h->type = type; h->flags = flags; h->seq = req->seq; h->pid = g_nl[B.slot].portid;
    B.msg = at;
    return b;
}

static void attr(uint16_t type, const void *data, uint32_t n) {
    uint8_t *p = (uint8_t *)put(4 + n);
    if (!p) return;
    uint16_t l = (uint16_t)(4 + n);
    memcpy(p, &l, 2); memcpy(p + 2, &type, 2);
    memcpy(p + 4, data, n);
}
static void attr32(uint16_t type, uint32_t v) { attr(type, &v, 4); }
static void attr8(uint16_t type, uint8_t v) { attr(type, &v, 1); }
static void attrs(uint16_t type, const char *s) { attr(type, s, nl_strlen(s) + 1); }

static void msg_end(void) {
    if (B.full) return;
    NlHdr *h = (NlHdr *)(B.base + 4 + B.msg);
    h->len = B.used - B.msg;
}

static void reply_status(int i, const NlHdr *req, uint16_t type, int32_t error) {
    if (!dg_begin(i)) return;
    uint8_t *b = (uint8_t *)msg_begin(req, type, type == NLMSG_DONE ? NLM_F_MULTI : 0,
                                      type == NLMSG_ERROR ? 4 + 16 : 4);
    if (b) {
        memcpy(b, &error, 4);
        if (type == NLMSG_ERROR) memcpy(b + 4, req, 16);   /* the request's header, as Linux echoes it */
        msg_end();
    }
    dg_end();
}

/* ---------------------------------------------------------------- rtnetlink */

typedef struct {   /* struct ifinfomsg */
    uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change;
} IfInfo;
typedef struct {   /* struct ifaddrmsg */
    uint8_t family, prefixlen, flags, scope; uint32_t index;
} IfAddr;
typedef struct {   /* struct rtmsg */
    uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags;
} RtMsg;

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define IFF_LOWER_UP 0x10000

static int nic_index(void) { return net_nic_kind() ? 2 : 0; }
static const char *nic_name(void) { return net_nic_kind() == 3 ? "wlan0" : "eth0"; }

static uint8_t prefix_len(uint32_t mask_be) {
    uint32_t m = __builtin_bswap32(mask_be);
    uint8_t n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

static void link_msg(const NlHdr *req, uint16_t flags, int index) {
    IfInfo *ii = (IfInfo *)msg_begin(req, RTM_NEWLINK, flags, sizeof(IfInfo));
    if (!ii) return;
    ii->index = index;
    if (index == 1) {
        ii->type = 772;                                     /* ARPHRD_LOOPBACK */
        ii->flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_LOWER_UP;
        attrs(3, "lo");                                     /* IFLA_IFNAME */
        attr32(4, 65536);                                   /* IFLA_MTU */
        uint8_t z[6] = {0};
        attr(1, z, 6); attr(2, z, 6);                       /* IFLA_ADDRESS, IFLA_BROADCAST */
        attr8(16, 0);                                       /* IFLA_OPERSTATE: unknown, as Linux's lo */
    } else {
        bool up = net_is_up();
        ii->type = 1;                                       /* ARPHRD_ETHER */
        ii->flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST | (up ? IFF_RUNNING | IFF_LOWER_UP : 0);
        attrs(3, nic_name());
        attr32(4, 1500);
        uint8_t mac[6], bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        net_get_mac(mac);
        attr(1, mac, 6); attr(2, bc, 6);
        attr8(16, up ? 6 : 2);                              /* IF_OPER_UP / DOWN */
    }
    attr32(13, 1000);                                       /* IFLA_TXQLEN */
    attr8(17, 0);                                           /* IFLA_LINKMODE */
    msg_end();
}

static void addr_msg(const NlHdr *req, uint16_t flags, int index) {
    IfAddr *ia = (IfAddr *)msg_begin(req, RTM_NEWADDR, flags, sizeof(IfAddr));
    if (!ia) return;
    ia->family = AF_INET_;
    ia->index = (uint32_t)index;
    if (index == 1) {
        ia->prefixlen = 8; ia->scope = 254; ia->flags = 0x80;   /* host, IFA_F_PERMANENT */
        attr32(1, 0x0100007F); attr32(2, 0x0100007F);           /* IFA_ADDRESS, IFA_LOCAL */
        attrs(3, "lo");                                          /* IFA_LABEL */
    } else {
        uint32_t ip = net_get_ip(), mask = net_get_netmask();
        ia->prefixlen = prefix_len(mask); ia->scope = 0;
        attr32(1, ip); attr32(2, ip);
        attr32(4, ip | ~mask);                                   /* IFA_BROADCAST */
        attrs(3, nic_name());
    }
    attr32(8, ia->flags);                                        /* IFA_FLAGS */
    msg_end();
}

static void route_msg(const NlHdr *req, uint16_t flags, uint8_t table, uint8_t type, uint8_t proto,
                      uint8_t scope, uint32_t dst, uint8_t dst_len, uint32_t gw, int oif, uint32_t src) {
    RtMsg *rt = (RtMsg *)msg_begin(req, RTM_NEWROUTE, flags, sizeof(RtMsg));
    if (!rt) return;
    rt->family = AF_INET_; rt->dst_len = dst_len; rt->table = table;
    rt->protocol = proto; rt->scope = scope; rt->type = type;
    attr32(15, table);                                          /* RTA_TABLE */
    if (dst_len) attr32(1, dst);                                /* RTA_DST */
    if (src) attr32(7, src);                                    /* RTA_PREFSRC */
    if (gw) attr32(5, gw);                                      /* RTA_GATEWAY */
    attr32(4, (uint32_t)oif);                                   /* RTA_OIF */
    msg_end();
}

/* the routing tables: main (the network's), local (this machine's own) */
static void routes(const NlHdr *req) {
    uint32_t ip = net_get_ip(), gw = net_get_gateway(), mask = net_get_netmask();
    int nic = nic_index();
    if (nic && ip) {
        if (gw) route_msg(req, NLM_F_MULTI, 254, 1, 16, 0, 0, 0, gw, nic, ip);       /* default via gw, proto dhcp */
        route_msg(req, NLM_F_MULTI, 254, 1, 2, 253, ip & mask, prefix_len(mask), 0, nic, ip);
    }
    route_msg(req, NLM_F_MULTI, 255, 2, 2, 254, 0x0000007F, 8, 0, 1, 0x0100007F);   /* local 127.0.0.0/8 */
    route_msg(req, NLM_F_MULTI, 255, 2, 2, 254, 0x0100007F, 32, 0, 1, 0x0100007F);
    if (nic && ip) route_msg(req, NLM_F_MULTI, 255, 2, 2, 254, ip, 32, 0, nic, ip);
}

/* RTM_GETROUTE for one address: the way there (ip route get) */
static int32_t route_get(const NlHdr *req, const uint8_t *body, uint32_t blen) {
    uint32_t dst = 0;
    for (uint32_t off = sizeof(RtMsg); off + 4 <= blen;) {
        uint16_t l, t;
        memcpy(&l, body + off, 2); memcpy(&t, body + off + 2, 2);
        if (l < 4 || off + l > blen) break;
        if ((t & 0x3fff) == 1 && l >= 8) memcpy(&dst, body + off + 4, 4);
        off += (l + 3u) & ~3u;
    }
    uint32_t ip = net_get_ip(), gw = net_get_gateway(), mask = net_get_netmask();
    int nic = nic_index();
    if ((dst & 0xFF) == 127 || (ip && dst == ip))
        route_msg(req, 0, 255, 2, 2, 254, dst, 32, 0, (dst & 0xFF) == 127 ? 1 : nic, (dst & 0xFF) == 127 ? 0x0100007F : ip);
    else if (!nic || !ip) return -101;                          /* -ENETUNREACH */
    else if ((dst & mask) == (ip & mask)) route_msg(req, 0, 254, 1, 2, 0, dst, 32, 0, nic, ip);
    else if (gw) route_msg(req, 0, 254, 1, 2, 0, dst, 32, gw, nic, ip);
    else return -101;
    return 0;
}

/* one request: answered into the open datagram; a status to send after
   it (0 = an acknowledgement if asked for, 1 = none) */
static int32_t route_request(const NlHdr *h, const uint8_t *body, uint32_t blen) {
    bool dump = (h->flags & NLM_F_DUMP) == NLM_F_DUMP;
    uint8_t family = blen ? body[0] : 0;
    bool v4 = family == 0 || family == AF_INET_;
    switch (h->type) {
    case RTM_GETLINK:
        if (dump) {
            link_msg(h, NLM_F_MULTI, 1);
            if (nic_index()) link_msg(h, NLM_F_MULTI, 2);
            return 0;
        } else {
            int32_t idx = 0;
            if (blen >= sizeof(IfInfo)) memcpy(&idx, body + 4, 4);
            /* or by name: IFLA_IFNAME */
            for (uint32_t off = sizeof(IfInfo); !idx && off + 4 <= blen;) {
                uint16_t l, t;
                memcpy(&l, body + off, 2); memcpy(&t, body + off + 2, 2);
                if (l < 4 || off + l > blen) break;
                if (t == 3) {
                    const char *n = (const char *)body + off + 4;
                    uint32_t nl = l - 4u;
                    if (nl >= 2 && nl_eq(n, "lo", 2) && (nl == 2 || !n[2])) idx = 1;
                    else if (nic_index() && nl >= 4 && nl_eq(n, nic_name(), 4) && (nl == 4 || !n[4])) idx = 2;
                    else idx = -1;
                }
                off += (l + 3u) & ~3u;
            }
            if (idx != 1 && !(idx == 2 && nic_index())) return -19;   /* -ENODEV */
            link_msg(h, 0, idx);
            return 1;
        }
    case RTM_GETADDR:
        if (!dump) return -95;
        if (v4) {
            addr_msg(h, NLM_F_MULTI, 1);
            if (nic_index() && net_get_ip()) addr_msg(h, NLM_F_MULTI, 2);
        }
        return 0;
    case RTM_GETROUTE:
        if (dump) { if (v4) routes(h); return 0; }
        if (!v4) return -101;
        { int32_t r = route_get(h, body, blen); return r ? r : 1; }
    default:
        if (dump) return 0;                                     /* neighbours, rules, ...: none */
        return -95;                                             /* changes: -EOPNOTSUPP */
    }
}

/* ---------------------------------------------------------------- sock_diag */

typedef struct {   /* struct inet_diag_sockid */
    uint16_t sport, dport; uint32_t src[4], dst[4]; uint32_t ifindex; uint32_t cookie[2];
} DiagId;
typedef struct {   /* struct inet_diag_msg */
    uint8_t family, state, timer, retrans; DiagId id;
    uint32_t expires, rqueue, wqueue, uid, inode;
} DiagMsg;

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static int32_t diag_request(const NlHdr *h, const uint8_t *body, uint32_t blen) {
    if (!((h->flags & NLM_F_DUMP) == NLM_F_DUMP)) return -95;
    uint8_t family, protocol;
    uint32_t states;
    if (h->type == SOCK_DIAG_BY_FAMILY && blen >= 8) {          /* inet_diag_req_v2 */
        family = body[0]; protocol = body[1];
        memcpy(&states, body + 4, 4);
    } else if (h->type == TCPDIAG_GETSOCK && blen >= 4) {       /* the old inet_diag_req */
        family = body[0]; protocol = 6;
        if (blen < 4 + 48 + 8) return 0;
        memcpy(&states, body + 4 + 48 + 4, 4);
    } else return 0;
    if (family != AF_INET_ || protocol != 6) return 0;          /* TCP over IPv4 only; the rest: none */
    InetSockEnt *e = (InetSockEnt *)kmalloc(sizeof(InetSockEnt) * 160);
    if (!e) return -12;
    int n = tcp_inet_list(e, 64);
    n += usock_inet_list(e + n, 160 - n);
    for (int k = 0; k < n; k++) {
        if (!(states & (1u << e[k].state))) continue;
        DiagMsg *m = (DiagMsg *)msg_begin(h, h->type, NLM_F_MULTI, sizeof(DiagMsg));
        if (!m) break;
        m->family = AF_INET_; m->state = e[k].state;
        m->id.sport = be16(e[k].lport); m->id.dport = be16(e[k].rport);
        m->id.src[0] = e[k].lip; m->id.dst[0] = e[k].rip;
        m->id.cookie[0] = m->id.cookie[1] = 0xFFFFFFFFu;        /* INET_DIAG_NOCOOKIE */
        m->rqueue = e[k].rq; m->wqueue = e[k].wq; m->uid = e[k].uid;
        msg_end();
    }
    kfree(e);
    return 0;
}

/* ---------------------------------------------------------------- the socket */

/* a datagram of requests: every one answered */
int64_t netlink_send(int i, const uint8_t *data, uint32_t len) {
    if (i < 0 || i >= NL_SLOTS || !g_nl[i].refs) return -9;
    uint32_t off = 0;
    while (len - off >= 16) {
        NlHdr h;
        memcpy(&h, data + off, 16);
        /* a message inside what is left (the sum used to wrap around) */
        if (h.len < 16 || h.len > len - off) break;
        bool dump = (h.flags & NLM_F_DUMP) == NLM_F_DUMP;
        if (!g_nl[i].host) {
            if (dump) reply_status(i, &h, NLMSG_DONE, 0);
            else if (h.flags & NLM_F_REQUEST) reply_status(i, &h, NLMSG_ERROR, 0);
        } else if (h.flags & NLM_F_REQUEST) {
            int32_t st = 0;
            if (dg_begin(i)) {
                st = g_nl[i].proto == 4 ? diag_request(&h, data + off + 16, h.len - 16)
                                        : route_request(&h, data + off + 16, h.len - 16);
                dg_end();
            }
            if (dump && st >= 0) reply_status(i, &h, NLMSG_DONE, 0);
            else if (st < 0) reply_status(i, &h, NLMSG_ERROR, st);
            else if (st == 0 && (h.flags & NLM_F_ACK)) reply_status(i, &h, NLMSG_ERROR, 0);
            else if (st == 1 && (h.flags & NLM_F_ACK)) reply_status(i, &h, NLMSG_ERROR, 0);
        }
        uint32_t step = (h.len + 3) & ~3u;
        if (step > len - off) break;
        off += step;
    }
    return len;
}

/* the first datagram waiting: up to len bytes of it into out; *full is its
   whole length. flags: MSG_PEEK (2) leaves it waiting. */
int64_t netlink_recv(int i, uint8_t *out, uint32_t len, int flags, uint32_t *full) {
    if (i < 0 || i >= NL_SLOTS || !g_nl[i].refs) return -9;
    if (!g_nl[i].len) return -11;
    uint32_t dl;
    memcpy(&dl, g_nl[i].buf, 4);
    uint32_t n = dl < len ? dl : len;
    memcpy(out, g_nl[i].buf + 4, n);
    if (full) *full = dl;
    if (!(flags & 2)) {
        uint32_t take = 4 + dl;
        for (uint32_t k = take; k < g_nl[i].len; k++) g_nl[i].buf[k - take] = g_nl[i].buf[k];
        g_nl[i].len -= take;
    }
    return n;
}
