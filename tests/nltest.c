/*
 * WynlandOS - netlink check (kernel/netlink.c): what ip, ss and glibc ask.
 *
 *   getifaddrs: lo 127.0.0.1 (and the NIC's address when it has one)
 *   getaddrinfo("localhost", AI_ADDRCONFIG): 127.0.0.1, IPv4 only
 *   rtnetlink: a link dump, a route dump; MSG_PEEK|MSG_TRUNC sizes a
 *   datagram without taking it (iproute2 does that)
 *   sock_diag: a TCP listener on 127.0.0.1 shows up as LISTEN (ss -lt)
 *
 * One "[nltest] PASS|FAIL" line per check, then "[nltest] DONE".
 * Build: gcc -O2 -o build/nltest.elf tests/nltest.c
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail;
static void check(int ok, const char *what) {
    fprintf(stderr, "[nltest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

/* one dump request; every message's type counted, the bodies handed to fn */
static int dump(int proto, void *req, unsigned len, void (*fn)(struct nlmsghdr *, void *), void *arg) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, proto);
    if (s < 0) { perror("[nltest] socket"); return -1; }
    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    if (sendto(s, req, len, 0, (struct sockaddr *)&k, sizeof k) < 0) { close(s); return -1; }
    int msgs = 0;
    for (;;) {
        /* iproute2's way: peek for the size first */
        struct iovec v0 = { NULL, 0 };
        struct msghdr mh = { .msg_iov = &v0, .msg_iovlen = 1 };
        ssize_t need = recvmsg(s, &mh, MSG_PEEK | MSG_TRUNC);
        if (need <= 0) { close(s); return -1; }
        static char buf[65536];
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n != need) { fprintf(stderr, "[nltest]   peek said %zd, read %zd\n", need, n); close(s); return -1; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { close(s); return msgs; }
            if (h->nlmsg_type == NLMSG_ERROR) { close(s); return -1; }
            msgs++;
            if (fn) fn(h, arg);
        }
    }
}

static void on_link(struct nlmsghdr *h, void *arg) {
    struct ifinfomsg *ii = NLMSG_DATA(h);
    struct rtattr *a = IFLA_RTA(ii);
    int l = IFLA_PAYLOAD(h);
    for (; RTA_OK(a, l); a = RTA_NEXT(a, l))
        if (a->rta_type == IFLA_IFNAME) {
            fprintf(stderr, "[nltest]   link %d %s\n", ii->ifi_index, (char *)RTA_DATA(a));
            if (!strcmp(RTA_DATA(a), "lo")) *(int *)arg = 1;
        }
}

static void on_route(struct nlmsghdr *h, void *arg) {
    struct rtmsg *r = NLMSG_DATA(h);
    (void)arg;
    fprintf(stderr, "[nltest]   route table %d dst/%d type %d\n", r->rtm_table, r->rtm_dst_len, r->rtm_type);
}

static void on_sock(struct nlmsghdr *h, void *arg) {
    struct inet_diag_msg *m = NLMSG_DATA(h);
    int port = ntohs(m->id.idiag_sport);
    fprintf(stderr, "[nltest]   tcp state %d :%d -> :%d\n", m->idiag_state, port, ntohs(m->id.idiag_dport));
    if (m->idiag_state == 10 && port == 8765) *(int *)arg = 1;
}

int main(void) {
    /* getifaddrs */
    struct ifaddrs *ifa;
    int lo4 = 0;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
            char a[64] = "-";
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET)
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
            fprintf(stderr, "[nltest]   ifaddr %s %s\n", i->ifa_name, a);
            if (!strcmp(i->ifa_name, "lo") && !strcmp(a, "127.0.0.1")) lo4 = 1;
        }
        freeifaddrs(ifa);
    }
    check(lo4, "getifaddrs: lo 127.0.0.1");

    /* getaddrinfo with AI_ADDRCONFIG */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_ADDRCONFIG }, *res;
    int ok = 0, v6 = 0;
    if (getaddrinfo("localhost", "80", &hints, &res) == 0) {
        for (struct addrinfo *r = res; r; r = r->ai_next) {
            if (r->ai_family == AF_INET6) v6 = 1;
            if (r->ai_family == AF_INET && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(0x7f000001)) ok = 1;
        }
        freeaddrinfo(res);
    }
    check(ok && !v6, "getaddrinfo(localhost, AI_ADDRCONFIG): 127.0.0.1, no IPv6");

    /* rtnetlink dumps */
    struct { struct nlmsghdr h; struct ifinfomsg i; } lr = {
        { sizeof lr, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 1, 0 }, { .ifi_family = AF_UNSPEC } };
    int saw_lo = 0;
    check(dump(NETLINK_ROUTE, &lr, sizeof lr, on_link, &saw_lo) >= 1 && saw_lo, "RTM_GETLINK dump (peek+trunc): lo");
    struct { struct nlmsghdr h; struct rtmsg r; } rr = {
        { sizeof rr, RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP, 2, 0 }, { .rtm_family = AF_INET } };
    check(dump(NETLINK_ROUTE, &rr, sizeof rr, on_route, NULL) >= 2, "RTM_GETROUTE dump: the local routes at least");

    /* a change is refused, not pretended */
    {
        int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        struct { struct nlmsghdr h; struct ifaddrmsg a; } ar = {
            { sizeof ar, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE, 3, 0 }, { .ifa_family = AF_INET, .ifa_index = 1 } };
        send(s, &ar, sizeof ar, 0);
        char b[256];
        ssize_t n = recv(s, b, sizeof b, 0);
        struct nlmsgerr *e = NLMSG_DATA((struct nlmsghdr *)b);
        check(n > 0 && ((struct nlmsghdr *)b)->nlmsg_type == NLMSG_ERROR && e->error < 0, "RTM_NEWADDR: refused with an error");
        close(s);
    }

    /* sock_diag: a listener */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(8765), .sin_addr.s_addr = htonl(0x7f000001) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int lok = bind(ls, (struct sockaddr *)&sin, sizeof sin) == 0 && listen(ls, 8) == 0;
    struct { struct nlmsghdr h; struct inet_diag_req_v2 r; } dr = {
        { sizeof dr, SOCK_DIAG_BY_FAMILY, NLM_F_REQUEST | NLM_F_DUMP, 4, 0 },
        { .sdiag_family = AF_INET, .sdiag_protocol = IPPROTO_TCP, .idiag_states = ~0u } };
    int saw = 0;
    check(lok && dump(NETLINK_SOCK_DIAG, &dr, sizeof dr, on_sock, &saw) >= 1 && saw, "sock_diag: TCP LISTEN on :8765");
    close(ls);

    fprintf(stderr, "[nltest] DONE %s\n", g_fail ? "FAIL" : "PASS");
    printf("[nltest] DONE %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
