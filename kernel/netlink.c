/*
 * WynlandOS - NETLINK_ROUTE sockets, as much as configuring an isolated
 * network namespace needs.
 *
 * bubblewrap brings up "lo" in a new network namespace with rtnetlink
 * (RTM_NEWADDR 127.0.0.1, RTM_NEWLINK up) and waits for the
 * acknowledgements. A network namespace here has no IP network at all, so
 * there is nothing to configure: every request is acknowledged (error 0),
 * and a dump request (RTM_GET*) answers an empty list (NLMSG_DONE). No
 * multicast groups, no notifications.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/process.h>

#define NL_SLOTS 32
#define NL_BUF   2048

#define NLMSG_ERROR 2
#define NLMSG_DONE  3
#define NLM_F_REQUEST 0x1
#define NLM_F_ACK     0x4
#define NLM_F_DUMP    0x300

typedef struct {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
} NlHdr;

static struct {
    uint32_t refs;
    uint32_t portid;         /* answers carry it (glibc drops any that do not) */
    uint32_t len;            /* bytes of replies waiting */
    uint8_t  buf[NL_BUF];
} g_nl[NL_SLOTS];

int netlink_create(uint32_t portid) {
    for (int i = 0; i < NL_SLOTS; i++)
        if (!g_nl[i].refs) { g_nl[i].refs = 1; g_nl[i].len = 0; g_nl[i].portid = portid; return i; }
    return -23;   /* -ENFILE */
}

void netlink_ref(int i) { if (i >= 0 && i < NL_SLOTS) g_nl[i].refs++; }
void netlink_unref(int i) { if (i >= 0 && i < NL_SLOTS && g_nl[i].refs) g_nl[i].refs--; }

bool netlink_readable(int i) { return i >= 0 && i < NL_SLOTS && g_nl[i].len > 0; }

static void reply(int i, const NlHdr *req, uint16_t type, int32_t error) {
    uint32_t n = type == NLMSG_ERROR ? 16 + 4 + 16 : 16 + 4;
    if (g_nl[i].len + n > NL_BUF) return;
    uint8_t *o = g_nl[i].buf + g_nl[i].len;
    NlHdr h = { n, type, 0, req->seq, g_nl[i].portid };
    memcpy(o, &h, 16);
    memcpy(o + 16, &error, 4);
    if (type == NLMSG_ERROR) memcpy(o + 20, req, 16);   /* the request's header, as Linux echoes it */
    g_nl[i].len += n;
}

/* a datagram of requests: every one answered */
int64_t netlink_send(int i, const uint8_t *data, uint32_t len) {
    if (i < 0 || i >= NL_SLOTS || !g_nl[i].refs) return -9;
    uint32_t off = 0;
    while (len - off >= 16) {
        NlHdr h;
        memcpy(&h, data + off, 16);
        /* a message inside what is left (the sum used to wrap around) */
        if (h.len < 16 || h.len > len - off) break;
        if ((h.flags & NLM_F_DUMP) == NLM_F_DUMP) reply(i, &h, NLMSG_DONE, 0);
        else if (h.flags & NLM_F_REQUEST) reply(i, &h, NLMSG_ERROR, 0);
        uint32_t step = (h.len + 3) & ~3u;
        if (step > len - off) break;
        off += step;
    }
    return len;
}

/* the waiting replies, up to len (one read takes what fits) */
int64_t netlink_recv(int i, uint8_t *out, uint32_t len) {
    if (i < 0 || i >= NL_SLOTS || !g_nl[i].refs) return -9;
    uint32_t n = g_nl[i].len < len ? g_nl[i].len : len;
    memcpy(out, g_nl[i].buf, n);
    for (uint32_t k = n; k < g_nl[i].len; k++) g_nl[i].buf[k - n] = g_nl[i].buf[k];
    g_nl[i].len -= n;
    return n;
}
