/*
 * WynlandOS - TCP Protocol Implementation
 *
 * Phase 5.8: TCP — 3-way handshake, data transfer, flow control, graceful close
 *
 * Implements: tcp_init, tcp_connect, tcp_send, tcp_recv, tcp_close,
 *             tcp_handle_packet, and internal helpers.
 */

#include <wynland/random.h>
#include <wynland/sched.h>
#include <wynland/tcp.h>
#include <wynland/unix_socket.h>
#include <wynland/net.h>
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/spinlock.h>

extern uint64_t timer_get_ticks(void);

/* Phase 22d: bounded retry slice for real blocking recv, same reasoning
   as KPipe/Pty's own (see kernel/syscall.c) -- a real wake fires
   immediately via tcp_handle_packet()'s waitqueue_wake_all() on any
   state change, this is only a safety net against a connection that
   goes silent with no RST/FIN ever arriving (dead route, no ICMP). */
#define TCP_WAIT_RETRY_TICKS 500

/* ============================================================
 * External helpers (provided by the kernel / net.c)
 * ============================================================ */

extern void     serial_write_string(const char *s);
extern bool     net_send_ipv4(uint32_t dst_ip, uint8_t protocol,
                              const void *payload, uint32_t len);
extern uint16_t ip_checksum(const void *data, uint32_t len);
extern void     net_poll(void);
extern uint32_t net_get_ip(void);

/* ============================================================
 * Static state
 * ============================================================ */

static TcpConnection connections[TCP_MAX_CONNECTIONS];

/* The connections' state between the receive path -- net_poll() from the
   timer IRQ, on any core, outside the big kernel lock -- and the system
   calls reading and sending on another core: the reader used to shift
   the buffer while the IRQ appended to it (four downloads at once on four
   cores came out corrupt). Taken with interrupts off; never across a
   copy to user memory (ring_read bounces through the stack). */
static Spinlock g_tcp_lock = SPINLOCK_INIT("tcp");

/* Sequence-number comparisons modulo 2^32: plain < and >= broke when the
   numbers wrapped. */
#define SEQ_LT(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define SEQ_GT(a, b)  SEQ_LT(b, a)
#define SEQ_GEQ(a, b) SEQ_LEQ(b, a)

/* free receive space: the window (from rcv_nxt; what came past a gap
   lies inside it) */
static uint32_t rx_window(const TcpConnection *c)
{
    return TCP_RX_BUF_SIZE - c->rx_len;
}

/* ...as the 16-bit field says it: scaled when both sides agreed (never in
   a SYN), else at most 65535 */
static uint16_t rx_window_field(const TcpConnection *c, bool syn)
{
    uint32_t w = rx_window(c);
    if (c->wscale_ok && !syn) w >>= TCP_RCV_WSCALE;
    return (uint16_t)(w > 65535 ? 65535 : w);
}

/* bytes into / out of the ring at `off` past its first unread byte */
static void ring_put(TcpConnection *c, uint32_t off, const uint8_t *p, uint32_t n)
{
    uint32_t pos = (c->rx_head + off) % TCP_RX_BUF_SIZE;
    uint32_t first = TCP_RX_BUF_SIZE - pos < n ? TCP_RX_BUF_SIZE - pos : n;
    memcpy(c->rx_buf + pos, p, first);
    if (n > first) memcpy(c->rx_buf, p + first, n - first);
}

static uint32_t ring_take(TcpConnection *c, uint8_t *buf, uint32_t max);

/* up to max bytes for the reader: taken from the ring under the lock a
   piece at a time, copied out (maybe to user memory) without it */
static uint32_t ring_read(TcpConnection *c, uint8_t *dst, uint32_t max)
{
    uint8_t bounce[2048];
    uint32_t total = 0;
    while (total < max) {
        uint32_t want = max - total < sizeof bounce ? max - total : (uint32_t)sizeof bounce;
        uint64_t fl = spin_lock_irqsave(&g_tcp_lock);
        uint32_t n = ring_take(c, bounce, want);
        c->data_available = c->rx_len > 0;
        spin_unlock_irqrestore(&g_tcp_lock, fl);
        if (!n) break;
        memcpy(dst + total, bounce, n);
        total += n;
    }
    return total;
}

static uint32_t ring_take(TcpConnection *c, uint8_t *buf, uint32_t max)
{
    uint32_t n = c->rx_len < max ? c->rx_len : max;
    uint32_t first = TCP_RX_BUF_SIZE - c->rx_head < n ? TCP_RX_BUF_SIZE - c->rx_head : n;
    memcpy(buf, c->rx_buf + c->rx_head, first);
    if (n > first) memcpy(buf + first, c->rx_buf, n - first);
    c->rx_head = (c->rx_head + n) % TCP_RX_BUF_SIZE;
    c->rx_len -= n;
    return n;
}

/* A random free local port. The old counter was predictable and, being
   added to 49152 in a uint16_t, wrapped to ports 0, 1, 2... */
static uint16_t pick_local_port(void)
{
    for (int tries = 0; tries < 128; tries++) {
        uint16_t p = (uint16_t)(TCP_EPHEMERAL_BASE + random_u32() % (65536 - TCP_EPHEMERAL_BASE));
        bool used = false;
        for (uint32_t i = 0; i < TCP_MAX_CONNECTIONS; i++)
            if (connections[i].in_use && connections[i].local_port == p) used = true;
        if (!used) return p;
    }
    return 0;
}

/* ============================================================
 * Forward declarations of internal helpers
 * ============================================================ */

static TcpConnection *find_connection(uint32_t src_ip, uint16_t src_port,
                                      uint16_t dst_port);
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void *tcp_data, uint32_t tcp_len);
static bool tcp_send_segment(TcpConnection *conn, uint8_t flags,
                             const void *data, uint32_t data_len);

/* ============================================================
 * TCP checksum — with IPv4 pseudo-header
 *
 * pseudo-header: src_ip(4) + dst_ip(4) + zero(1) + proto(1) + tcp_len(2)
 * followed by the TCP header + data.
 * ============================================================ */

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void *tcp_data, uint32_t tcp_len)
{
    uint8_t pseudo[12];
    uint32_t sum = 0;
    uint32_t i;
    const uint8_t *p;

    /* Build pseudo-header (already in network byte order) */
    memcpy(&pseudo[0], &src_ip, 4);
    memcpy(&pseudo[4], &dst_ip, 4);
    pseudo[8]  = 0;
    pseudo[9]  = IP_PROTO_TCP;
    pseudo[10] = (uint8_t)(tcp_len >> 8);
    pseudo[11] = (uint8_t)(tcp_len & 0xFF);

    /* Sum pseudo-header */
    p = pseudo;
    for (i = 0; i + 1 < 12; i += 2) {
        uint16_t word = (uint16_t)((uint16_t)p[i] << 8 | p[i + 1]);
        sum += word;
    }

    /* Sum TCP header + data */
    p = (const uint8_t *)tcp_data;
    for (i = 0; i + 1 < tcp_len; i += 2) {
        uint16_t word = (uint16_t)((uint16_t)p[i] << 8 | p[i + 1]);
        sum += word;
    }
    if (tcp_len & 1) {
        sum += (uint16_t)p[tcp_len - 1] << 8;
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* ============================================================
 * Find connection by remote IP / remote port / local port
 * ============================================================ */

static TcpConnection *find_connection(uint32_t src_ip, uint16_t src_port,
                                      uint16_t dst_port)
{
    uint32_t i;

    for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        TcpConnection *c = &connections[i];
        if (c->in_use &&
            c->remote_ip   == src_ip   &&
            c->remote_port == src_port &&
            c->local_port  == dst_port) {
            return c;
        }
    }
    return NULL;
}

/* ============================================================
 * Build and send a TCP segment
 *
 * Constructs a TcpHeader with proper seq/ack numbers, computes
 * the TCP checksum (including pseudo-header), and hands the
 * segment to net_send_ipv4.
 * ============================================================ */

#define TCP_SEG_MAX (sizeof(TcpHeader) + 40 + NET_PKT_BUF)

static uint32_t tcp_build_segment(TcpConnection *conn, uint8_t flags,
                                  const void *data, uint32_t data_len, uint8_t *seg_buf);

/* Built under the lock, sent without it: sending may wait (the first
   packet to a host waits for its ARP answer, polling the network -- and
   this core's own packets come in through tcp_handle_packet, which takes
   the lock). A wait with the spinlock held also slept with it and woke on
   another core ("released by a core that does not hold 'tcp'"). */
static bool tcp_send_segment(TcpConnection *conn, uint8_t flags,
                             const void *data, uint32_t data_len)
{
    uint8_t seg_buf[TCP_SEG_MAX];
    uint64_t fl = spin_lock_irqsave(&g_tcp_lock);
    uint32_t n = tcp_build_segment(conn, flags, data, data_len, seg_buf);
    uint32_t ip = conn->remote_ip;
    spin_unlock_irqrestore(&g_tcp_lock, fl);
    return n && net_send_ipv4(ip, IP_PROTO_TCP, seg_buf, n);
}

/* From the receive path (the lock held): the ACK it answers with is only
   built here; tcp_handle_packet sends it once the lock is gone. One a
   packet -- a later one replaces it (it acknowledges as much or more). */
static uint8_t  g_ack_buf[sizeof(TcpHeader) + 40];
static uint32_t g_ack_len, g_ack_ip;
static bool tcp_send_segment_locked(TcpConnection *conn, uint8_t flags,
                                    const void *data, uint32_t data_len)
{
    if (data_len) return false;                        /* (only bare ACKs come this way) */
    uint8_t seg_buf[sizeof(TcpHeader) + 40 + 64];
    uint32_t n = tcp_build_segment(conn, flags, data, 0, seg_buf);
    if (!n || n > sizeof g_ack_buf) return false;
    memcpy(g_ack_buf, seg_buf, n);
    g_ack_len = n;
    g_ack_ip = conn->remote_ip;
    return true;
}

static uint32_t tcp_build_segment(TcpConnection *conn, uint8_t flags,
                                  const void *data, uint32_t data_len, uint8_t *seg_buf)
{
    TcpHeader *tcp;
    uint32_t tcp_total;
    uint16_t cksum;
    uint8_t opt[40];
    uint32_t ol = 0;
    bool syn = (flags & TCP_FLAG_SYN) != 0;

    if (syn) {
        /* our MSS (a full Ethernet frame: without it the peer sends 536-byte
           segments), our window's shift, SACK welcome */
        opt[ol++] = 2; opt[ol++] = 4; opt[ol++] = (uint8_t)(TCP_MSS >> 8); opt[ol++] = (uint8_t)(TCP_MSS & 0xFF);
        opt[ol++] = 1; opt[ol++] = 3; opt[ol++] = 3; opt[ol++] = TCP_RCV_WSCALE;
        opt[ol++] = 1; opt[ol++] = 1; opt[ol++] = 4; opt[ol++] = 2;
    } else if (conn->sack_ok && conn->ooo_n && (flags & TCP_FLAG_ACK)) {
        /* what we hold past the gap: the peer resends only what is missing */
        opt[ol++] = 1; opt[ol++] = 1; opt[ol++] = 5; opt[ol++] = (uint8_t)(2 + 8 * conn->ooo_n);
        for (uint32_t k = 0; k < conn->ooo_n; k++) {
            uint32_t s = htonl(conn->ooo_s[k]), e = htonl(conn->ooo_e[k]);
            memcpy(opt + ol, &s, 4); memcpy(opt + ol + 4, &e, 4);
            ol += 8;
        }
    }

    tcp_total = sizeof(TcpHeader) + ol + data_len;
    if (data_len > NET_PKT_BUF)
        return 0;

    tcp = (TcpHeader *)seg_buf;
    tcp->src_port   = htons(conn->local_port);
    tcp->dst_port   = htons(conn->remote_port);
    tcp->seq_num    = htonl(conn->snd_nxt);
    tcp->ack_num    = htonl(conn->rcv_nxt);
    tcp->data_offset = (uint8_t)(((sizeof(TcpHeader) + ol) / 4) << 4);
    tcp->flags      = flags;
    tcp->window     = htons(rx_window_field(conn, syn));
    conn->adv_wnd   = rx_window(conn);
    conn->wnd_small = rx_window(conn) < TCP_MSS;
    tcp->checksum   = 0;
    tcp->urgent_ptr = 0;

    if (ol) memcpy(seg_buf + sizeof(TcpHeader), opt, ol);
    /* Copy payload after header */
    if (data_len > 0 && data != NULL) {
        memcpy(seg_buf + sizeof(TcpHeader) + ol, data, data_len);
    }

    /* Compute TCP checksum with pseudo-header */
    cksum = tcp_checksum(conn->local_ip, conn->remote_ip,
                         seg_buf, tcp_total);
    tcp->checksum = htons(cksum);

    return tcp_total;
}

/* ============================================================
 * tcp_init — zero all TcpConnection slots
 * ============================================================ */

void tcp_init(void)
{
    memset(connections, 0, sizeof(connections));

    serial_write_string("[TCP] TCP subsystem initialised\n");
}

/* ============================================================
 * tcp_connect — Active open: 3-way handshake
 *
 * 1. Allocate a free connection slot
 * 2. Assign ephemeral local port
 * 3. Set ISS, build and send SYN
 * 4. Busy-poll waiting for SYN-ACK
 * 5. Send ACK to complete handshake
 * ============================================================ */

TcpConnection *tcp_connect(uint32_t remote_ip, uint16_t remote_port)
{
    TcpConnection *conn = NULL;
    uint32_t i;

    /* Find a free connection slot */
    for (i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].in_use) {
            conn = &connections[i];
            break;
        }
    }
    if (conn == NULL) {
        serial_write_string("[TCP] No free connection slots\n");
        return NULL;
    }

    /* Initialise the connection (TCB) -- its ring kept from the slot's
       last use (allocated once) */
    uint8_t *ring = conn->rx_buf;
    memset(conn, 0, sizeof(TcpConnection));
    conn->rx_buf = ring ? ring : (uint8_t *)kmalloc(TCP_RX_BUF_SIZE);
    if (!conn->rx_buf) {
        serial_write_string("[TCP] No memory for a receive buffer\n");
        return NULL;
    }
    conn->snd_mss     = 536;           /* until the peer says */
    conn->in_use      = true;
    conn->local_ip    = net_get_ip();
    conn->local_port  = pick_local_port();
    if (!conn->local_port) { conn->in_use = false; return NULL; }
    conn->remote_ip   = remote_ip;
    conn->remote_port = remote_port;

    /* Sequence numbers */
    /* random ISS (RFC 6528): a counter let anyone guess the numbers and
       inject into or reset the connection */
    conn->snd_iss = random_u32();
    conn->snd_nxt = conn->snd_iss;
    conn->snd_una = conn->snd_iss;

    /* Receive window */
    conn->rcv_wnd = TCP_WINDOW_SIZE;

    /* Clear synchronisation flags */
    conn->syn_ack_received = false;
    conn->fin_received     = false;
    conn->data_available   = false;
    conn->reset_received   = false;
    conn->ack_of_fin       = false;

    /* Enter SYN_SENT state */
    conn->state = TCP_STATE_SYN_SENT;

    serial_write_string("[TCP] Sending SYN\n");

    /* Send SYN — sequence number is snd_iss, no data */
    if (!tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0)) {
        serial_write_string("[TCP] Failed to send SYN\n");
        conn->in_use = false;
        return NULL;
    }

    /* SYN consumes one sequence number */
    conn->snd_nxt = conn->snd_iss + 1;
    conn->snd_max = conn->snd_nxt;

    /* Wait for the SYN-ACK (an internet round trip: tens of ms), the SYN
       sent again at 1, 3 and 7 s (RFC 6298's first RTO, doubled): a lost
       one used to cost the whole 5 s and a failed connection */
    uint64_t syn_deadline = net_deadline_ms(TCP_CONNECT_TIMEOUT_MS);
    uint64_t resend_ms = 1000, resend_at = net_deadline_ms(1000);
    for (i = 0; !net_past(syn_deadline); i++) {
        net_wait_tick();
        net_poll();
        if (!conn->syn_ack_received && net_past(resend_at) && resend_ms < 8000) {
            conn->snd_nxt = conn->snd_iss;
            tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0);
            conn->snd_nxt = conn->snd_iss + 1;
            resend_ms *= 2;
            resend_at = net_deadline_ms(resend_ms);
            serial_write_string("[TCP] SYN sent again\n");
        }

        if (conn->reset_received) {
            serial_write_string("[TCP] Connection reset during handshake\n");
            conn->in_use = false;
            return NULL;
        }

        if (conn->syn_ack_received) {
            /* Complete the handshake: send ACK */
            conn->state = TCP_STATE_ESTABLISHED;

            serial_write_string("[TCP] SYN-ACK received, sending ACK\n");

            if (!tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0)) {
                serial_write_string("[TCP] Failed to send ACK\n");
                conn->in_use = false;
                return NULL;
            }

            serial_write_string("[TCP] Connection established\n");
            return conn;
        }
    }

    /* Timeout — no SYN-ACK received */
    serial_write_string("[TCP] Connect timeout — no SYN-ACK\n");
    conn->state  = TCP_STATE_CLOSED;
    conn->in_use = false;
    return NULL;
}

/* ============================================================
 * tcp_send — Send data over an established connection
 *
 * As much as the peer's window takes in flight at once (capped), in
 * segments of the peer's MSS; on an RTO without progress, from the oldest
 * unacknowledged byte again (go-back-N), TCP_RETRANSMIT_MAX times. It
 * used to send one segment and wait for its ACK: a round trip per 1460
 * bytes (~15 KB/s at a phone's 100 ms).
 * ============================================================ */

#define TCP_SEND_INFLIGHT_MAX (64u * TCP_MSS)

int tcp_send(TcpConnection *conn, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (conn == NULL || conn->state != TCP_STATE_ESTABLISHED)
        return -1;

    uint32_t base = conn->snd_una;                       /* (every earlier send was acked in full) */
    uint32_t mss = conn->snd_mss ? conn->snd_mss : 536;
    uint32_t sent = 0, retries = 0;
    uint64_t rto_ticks = 100;                            /* ~1 s */
    conn->snd_nxt = base;

    for (;;) {
        if (conn->reset_received) {
            serial_write_string("[TCP] Connection reset during send\n");
            conn->state = TCP_STATE_CLOSED;
            return -1;
        }
        uint32_t acked = conn->snd_una - base;
        if (acked >= len) break;
        if (sent < acked) sent = acked;

        uint32_t wnd = conn->snd_wnd32 < TCP_SEND_INFLIGHT_MAX ? conn->snd_wnd32 : TCP_SEND_INFLIGHT_MAX;
        while (sent < len) {
            uint32_t chunk = len - sent < mss ? len - sent : mss;
            uint32_t inflight = sent - acked;
            if (inflight && inflight + chunk > wnd) break;   /* window full (one always goes: a zero-window probe) */
            conn->snd_nxt = base + sent;
            if (!tcp_send_segment(conn, TCP_FLAG_PSH | TCP_FLAG_ACK, p + sent, chunk)) {
                serial_write_string("[TCP] Failed to send data segment\n");
                return acked ? (int)acked : -1;
            }
            sent += chunk;
            conn->snd_nxt = base + sent;
            if (SEQ_GT(conn->snd_nxt, conn->snd_max)) conn->snd_max = conn->snd_nxt;
        }

        /* an ACK that moves us on, or the RTO */
        uint32_t before = conn->snd_una;
        uint64_t rto_at = timer_get_ticks() + rto_ticks;
        while (conn->snd_una == before && !conn->reset_received && timer_get_ticks() < rto_at && !sched_dying())
            waitqueue_wait(&conn->rx_wq, rto_at);
        if (sched_dying()) return -1;
        if (conn->snd_una != before) { retries = 0; rto_ticks = 100; continue; }
        if (conn->reset_received) continue;
        if (++retries >= TCP_RETRANSMIT_MAX) {
            serial_write_string("[TCP] Max retransmits reached\n");
            conn->snd_nxt = conn->snd_una;
            acked = conn->snd_una - base;
            return acked ? (int)acked : -1;
        }
        serial_write_string("[TCP] ACK timeout, retransmitting\n");
        sent = conn->snd_una - base;                     /* go back */
        if (rto_ticks < 800) rto_ticks *= 2;
    }

    conn->snd_nxt = conn->snd_una;
    return (int)len;
}

/* ============================================================
 * tcp_recv — Receive data from connection
 *
 * If data is already in rx_buf, copy and return immediately.
 * Otherwise blocks for real until data/FIN/RST (Phase 22d).
 * ============================================================ */

/* the reader took bytes: tell the peer once the window has opened by a
   good part (a quarter of the ring), or from below an MSS -- not on every
   read (an ACK per read was as many packets as the data) */
static void tcp_window_update(TcpConnection *conn)
{
    uint32_t w = rx_window(conn);
    if (conn->wnd_small ? w >= 2 * TCP_MSS : w >= conn->adv_wnd + TCP_RX_BUF_SIZE / 4)
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
}

int tcp_recv(TcpConnection *conn, void *buf, uint32_t max_len)
{
    uint32_t copy_len;

    if (conn == NULL || (conn->state != TCP_STATE_ESTABLISHED &&
                         conn->state != TCP_STATE_CLOSE_WAIT))
        return -1;

    /* If data already buffered, return it immediately */
    if (conn->rx_len > 0) {
        copy_len = ring_read(conn, (uint8_t *)buf, max_len);
        tcp_window_update(conn);
        return (int)copy_len;
    }

    /* No data buffered yet -- real blocking recv: park until data/FIN/RST
       instead of spinning net_poll() up to TCP_RECV_TIMEOUT times (net_poll()
       is now driven unconditionally off the timer tick, see kernel/irq.c,
       so this loop no longer needs to call it at all). Real POSIX recv()
       on an open socket with nothing buffered yet just waits -- it
       doesn't give up on its own, so this loops until one of the three
       terminal conditions is true, each wait bounded only as an internal
       safety-net retry slice (see TCP_WAIT_RETRY_TICKS), not an overall
       timeout. */
    conn->data_available = false;

    for (;;) {
        if (conn->reset_received) {
            serial_write_string("[TCP] Connection reset during recv\n");
            conn->state = TCP_STATE_CLOSED;
            return -1;
        }

        if (conn->fin_received) {
            /* Peer closed — return 0 if no data, or data first */
            if (conn->rx_len > 0)
                break;
            return 0;
        }

        if (conn->data_available && conn->rx_len > 0)
            break;
        if (sched_dying()) return -1;

        waitqueue_wait(&conn->rx_wq, timer_get_ticks() + TCP_WAIT_RETRY_TICKS);
    }

    copy_len = ring_read(conn, (uint8_t *)buf, max_len);
    tcp_window_update(conn);
    return (int)copy_len;
}

/* ============================================================
 * tcp_close — Graceful close (FIN handshake)
 *
 * 1. Send FIN|ACK
 * 2. Wait for ACK of our FIN   (FIN_WAIT_1 → FIN_WAIT_2)
 * 3. Wait for remote FIN       (FIN_WAIT_2 → TIME_WAIT)
 * 4. Send final ACK
 * 5. Mark connection CLOSED
 * ============================================================ */

void tcp_close(TcpConnection *conn)
{
    uint32_t i;

    if (conn == NULL || !conn->in_use)
        return;

    /* If already closed or reset, just clean up */
    if (conn->state == TCP_STATE_CLOSED || conn->reset_received) {
        conn->in_use = false;
        return;
    }

    bool was_close_wait = (conn->state == TCP_STATE_CLOSE_WAIT);

    conn->ack_of_fin   = false;

    if (was_close_wait) {
        conn->state = TCP_STATE_LAST_ACK;
    } else {
        conn->state = TCP_STATE_FIN_WAIT_1;
        conn->fin_received = false;
    }

    serial_write_string("[TCP] Sending FIN\n");

    if (!tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0)) {
        serial_write_string("[TCP] Failed to send FIN\n");
        conn->state  = TCP_STATE_CLOSED;
        conn->in_use = false;
        return;
    }

    /* FIN consumes one sequence number */
    conn->snd_nxt += 1;

    /* If we were in CLOSE_WAIT, wait for ACK of our FIN (LAST_ACK -> CLOSED) */
    if (was_close_wait) {
        uint64_t la_deadline = net_deadline_ms(TCP_CLOSE_TIMEOUT_MS);
        for (i = 0; !net_past(la_deadline); i++) {
            net_wait_tick();
            net_poll();
            if (conn->state == TCP_STATE_CLOSED || conn->reset_received) {
                break;
            }
        }
        conn->in_use = false;
        return;
    }

    /* ---- Wait for ACK of our FIN (FIN_WAIT_1 → FIN_WAIT_2) ---- */
    uint64_t fw1_deadline = net_deadline_ms(TCP_CLOSE_TIMEOUT_MS);
    for (i = 0; !net_past(fw1_deadline); i++) {
        net_wait_tick();
        net_poll();

        if (conn->reset_received) {
            serial_write_string("[TCP] Reset during close\n");
            conn->state  = TCP_STATE_CLOSED;
            conn->in_use = false;
            return;
        }

        /* Simultaneous close: remote FIN arrived before our ACK */
        if (conn->fin_received && conn->ack_of_fin) {
            /* Send final ACK and go to TIME_WAIT/CLOSED */
            conn->state = TCP_STATE_TIME_WAIT;
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            serial_write_string("[TCP] Connection closed (simultaneous)\n");
            conn->state  = TCP_STATE_CLOSED;
            conn->in_use = false;
            return;
        }

        if (conn->ack_of_fin) {
            conn->state = TCP_STATE_FIN_WAIT_2;
            break;
        }
    }

    if (conn->state == TCP_STATE_FIN_WAIT_1) {
        /* Timeout waiting for ACK of FIN */
        serial_write_string("[TCP] Close timeout (FIN_WAIT_1)\n");
        conn->state  = TCP_STATE_CLOSED;
        conn->in_use = false;
        return;
    }

    /* ---- Wait for remote FIN (FIN_WAIT_2 → TIME_WAIT) ---- */
    uint64_t fw2_deadline = net_deadline_ms(TCP_CLOSE_TIMEOUT_MS);
    for (i = 0; !net_past(fw2_deadline); i++) {
        net_wait_tick();
        net_poll();

        if (conn->reset_received) {
            serial_write_string("[TCP] Reset during FIN_WAIT_2\n");
            conn->state  = TCP_STATE_CLOSED;
            conn->in_use = false;
            return;
        }

        if (conn->fin_received) {
            /* Send final ACK */
            conn->state = TCP_STATE_TIME_WAIT;

            serial_write_string("[TCP] Remote FIN received, sending ACK\n");
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);

            conn->state  = TCP_STATE_CLOSED;
            conn->in_use = false;

            serial_write_string("[TCP] Connection closed\n");
            return;
        }
    }

    /* Timeout waiting for remote FIN */
    serial_write_string("[TCP] Close timeout (FIN_WAIT_2)\n");
    conn->state  = TCP_STATE_CLOSED;
    conn->in_use = false;
}

/* ============================================================
 * tcp_connect_slot / tcp_get_connection — index-based wrappers
 * for callers (syscall.c) that can only stash a uint32_t per fd.
 * ============================================================ */

int tcp_connect_slot(uint32_t remote_ip, uint16_t remote_port)
{
    TcpConnection *conn = tcp_connect(remote_ip, remote_port);
    if (conn == NULL)
        return -1;
    return (int)(conn - connections);
}

TcpConnection *tcp_get_connection(int idx)
{
    if (idx < 0 || idx >= (int)TCP_MAX_CONNECTIONS)
        return NULL;
    if (!connections[idx].in_use)
        return NULL;
    return &connections[idx];
}

/* A new cumulative ACK: inside (snd_una, snd_nxt] only. */
static void take_ack(TcpConnection *c, uint32_t ack)
{
    uint32_t top = SEQ_GT(c->snd_max, c->snd_nxt) ? c->snd_max : c->snd_nxt;
    if (SEQ_GT(ack, c->snd_una) && SEQ_LEQ(ack, top)) c->snd_una = ack;
}

/* Receive in order only: bytes we already have are trimmed, a segment
   past a gap is dropped (the ACK we send back tells the peer where we
   are, so it resends), and only what fits in the buffer is taken --
   rcv_nxt moves by exactly what was stored. The FIN counts once every
   byte before it is in. Every segment used to be appended whatever its
   sequence number (duplicates and reordering corrupted the stream) and
   data past a full buffer was ACKed and lost. Returns whether the
   segment needs an ACK. */
/* [s, e) into the out-of-order ranges, merged with any it touches; the
   newest first (SACK reports it first). Full: not kept (resent later). */
static void ooo_add(TcpConnection *c, uint32_t s, uint32_t e)
{
    for (uint32_t k = 0; k < c->ooo_n;) {
        if (SEQ_LEQ(c->ooo_s[k], e) && SEQ_LEQ(s, c->ooo_e[k])) {   /* touch or overlap: merge */
            if (SEQ_LT(c->ooo_s[k], s)) s = c->ooo_s[k];
            if (SEQ_GT(c->ooo_e[k], e)) e = c->ooo_e[k];
            c->ooo_n--;
            c->ooo_s[k] = c->ooo_s[c->ooo_n];
            c->ooo_e[k] = c->ooo_e[c->ooo_n];
            continue;
        }
        k++;
    }
    if (c->ooo_n == TCP_OOO_MAX) return;
    for (uint32_t k = c->ooo_n; k > 0; k--) { c->ooo_s[k] = c->ooo_s[k - 1]; c->ooo_e[k] = c->ooo_e[k - 1]; }
    c->ooo_s[0] = s; c->ooo_e[0] = e;
    c->ooo_n++;
}

/* rcv_nxt moved: the ranges it reached join the in-order bytes */
static void ooo_join(TcpConnection *c)
{
    for (bool moved = true; moved;) {
        moved = false;
        for (uint32_t k = 0; k < c->ooo_n; k++) {
            if (SEQ_LEQ(c->ooo_s[k], c->rcv_nxt)) {
                if (SEQ_GT(c->ooo_e[k], c->rcv_nxt)) {
                    uint32_t n = c->ooo_e[k] - c->rcv_nxt;
                    c->rx_len += n;
                    c->rcv_nxt += n;
                }
                c->ooo_n--;
                c->ooo_s[k] = c->ooo_s[c->ooo_n];
                c->ooo_e[k] = c->ooo_e[c->ooo_n];
                moved = true;
                break;
            }
        }
    }
}

/* A segment's data into the ring at its place: what we already have is
   trimmed, what is past the window dropped; bytes past a gap are kept
   (and SACKed) and join the stream once the gap fills -- they used to be
   dropped, so one lost packet on a lossy link (a phone's) cost every
   packet after it and the peer's whole window. The FIN counts once every
   byte before it is in. Returns whether the segment needs an ACK. */
static bool take_data(TcpConnection *c, uint32_t seq, const uint8_t *p, uint32_t len, bool fin)
{
    if (len == 0 && !fin) return false;
    if (SEQ_LT(seq, c->rcv_nxt)) {
        uint32_t skip = c->rcv_nxt - seq;                /* already have these */
        if (skip >= len && !(fin && skip == len)) return true;   /* all old (a resent FIN too) */
        p += skip; len -= skip; seq = c->rcv_nxt;
    }
    uint32_t off = seq - c->rcv_nxt;                     /* past rcv_nxt (0: in order) */
    uint32_t room = rx_window(c);
    if (off >= room && len) return true;                 /* past the window */
    if (off + len > room) { len = room - off; fin = false; }
    if (len) ring_put(c, c->rx_len + off, p, len);
    if (fin && !c->fin_received) { c->fin_pending = true; c->fin_seq = seq + len; }
    if (off == 0) {
        c->rx_len += len;
        c->rcv_nxt += len;
        ooo_join(c);
    } else if (len) {
        ooo_add(c, seq, seq + len);
    }
    if (c->fin_pending && !c->fin_received && c->rcv_nxt == c->fin_seq) {
        c->rcv_nxt += 1;                                 /* FIN takes one number */
        c->fin_received = true;
        c->fin_pending = false;
    }
    c->data_available = c->rx_len > 0;
    return true;
}

/* the SYN-ACK's options: the peer's MSS, window shift, SACK */
static void take_syn_options(TcpConnection *c, const uint8_t *o, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        uint8_t k = o[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (i + 1 >= n || o[i + 1] < 2 || i + o[i + 1] > n) break;
        uint8_t l = o[i + 1];
        if (k == 2 && l == 4) {
            uint16_t m = (uint16_t)(o[i + 2] << 8 | o[i + 3]);
            if (m >= 64) c->snd_mss = m < TCP_MSS ? m : TCP_MSS;
        } else if (k == 3 && l == 3) {
            c->snd_wscale = o[i + 2] > 14 ? 14 : o[i + 2];
            c->wscale_ok = true;
        } else if (k == 4 && l == 2) {
            c->sack_ok = true;
        }
        i += l;
    }
}

/* ============================================================
 * tcp_handle_packet — Incoming TCP segment handler
 *
 * Called from handle_ipv4 in net.c when protocol == TCP.
 * Parses the TcpHeader, finds the matching connection, and
 * processes based on the current connection state.
 * ============================================================ */

static void tcp_handle_packet_locked(uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t *data, uint32_t len);

void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip,
                       const uint8_t *data, uint32_t len)
{
    uint8_t ack[sizeof g_ack_buf];
    uint64_t fl = spin_lock_irqsave(&g_tcp_lock);
    g_ack_len = 0;
    tcp_handle_packet_locked(src_ip, dst_ip, data, len);
    uint32_t n = g_ack_len, ip = g_ack_ip;
    if (n) memcpy(ack, g_ack_buf, n);
    g_ack_len = 0;
    spin_unlock_irqrestore(&g_tcp_lock, fl);
    if (n) net_send_ipv4(ip, IP_PROTO_TCP, ack, n);
}

static void tcp_handle_packet_locked(uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t *data, uint32_t len)
{
    const TcpHeader *tcp;
    TcpConnection *conn;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  flags;
    uint32_t hdr_len;
    uint32_t payload_len;
    const uint8_t *payload;

    (void)dst_ip;

    if (len < sizeof(TcpHeader))
        return;

    tcp = (const TcpHeader *)data;

    src_port = ntohs(tcp->src_port);
    dst_port = ntohs(tcp->dst_port);
    seq      = ntohl(tcp->seq_num);
    ack      = ntohl(tcp->ack_num);
    flags    = tcp->flags;

    /* Data offset: upper 4 bits of data_offset field = header len in 32-bit words */
    hdr_len = (uint32_t)((tcp->data_offset >> 4) & 0x0F) * 4;
    if (hdr_len < sizeof(TcpHeader) || hdr_len > len)
        return;

    payload     = data + hdr_len;
    payload_len = len - hdr_len;

    /* Find matching connection */
    conn = find_connection(src_ip, src_port, dst_port);
    if (conn == NULL) {
        /* No connection found — optionally send RST, but for now just drop */
        return;
    }

    /* ---- Handle RST at any state ---- */
    if (flags & TCP_FLAG_RST) {
        /* RFC 5961: only an RST at exactly the next expected sequence
           number resets (in SYN_SENT: one acknowledging our SYN); one
           elsewhere in the window gets a challenge ACK, the rest are
           dropped. Any RST with the right 4-tuple used to kill the
           connection. */
        bool exact = conn->state == TCP_STATE_SYN_SENT
                   ? ((flags & TCP_FLAG_ACK) && ack == conn->snd_nxt)
                   : seq == conn->rcv_nxt;
        if (!exact) {
            if (conn->state != TCP_STATE_SYN_SENT &&
                SEQ_GEQ(seq, conn->rcv_nxt) && SEQ_LT(seq, conn->rcv_nxt + rx_window(conn)))
                tcp_send_segment_locked(conn, TCP_FLAG_ACK, NULL, 0);
            return;
        }
        serial_write_string("[TCP] RST received\n");
        conn->reset_received = true;
        conn->state = TCP_STATE_CLOSED;
        waitqueue_wake_all(&conn->rx_wq); waitqueue_wake_all(&g_poll_any_wq);
        return;
    }

    /* ---- State machine ---- */
    switch (conn->state) {

    /* --------------------------------------------------------
     * SYN_SENT: expecting SYN-ACK
     * -------------------------------------------------------- */
    case TCP_STATE_SYN_SENT:
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
            (TCP_FLAG_SYN | TCP_FLAG_ACK)) {

            /* Validate ACK number */
            if (ack != conn->snd_nxt) {
                serial_write_string("[TCP] SYN-ACK: bad ACK number\n");
                return;
            }

            conn->rcv_irs = seq;
            conn->rcv_nxt = seq + 1;  /* SYN consumes one seq */
            conn->snd_una = ack;
            take_syn_options(conn, data + sizeof(TcpHeader), hdr_len - sizeof(TcpHeader));
            conn->snd_wnd = ntohs(tcp->window);
            conn->snd_wnd32 = conn->snd_wnd;          /* (never scaled in a SYN) */

            conn->syn_ack_received = true;

            serial_write_string("[TCP] SYN-ACK processed\n");
        }
        break;

    /* --------------------------------------------------------
     * ESTABLISHED: data transfer
     * -------------------------------------------------------- */
    case TCP_STATE_ESTABLISHED:
        /* Update send window */
        conn->snd_wnd = ntohs(tcp->window);
        conn->snd_wnd32 = (uint32_t)conn->snd_wnd << (conn->wscale_ok ? conn->snd_wscale : 0);

        if (flags & TCP_FLAG_ACK) take_ack(conn, ack);
        {
            bool had_fin = conn->fin_received;
            bool need_ack = take_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_FIN) != 0);
            if (conn->fin_received && !had_fin) {
                serial_write_string("[TCP] FIN received (ESTABLISHED)\n");
                conn->state = TCP_STATE_CLOSE_WAIT;
            }
            if (need_ack) tcp_send_segment_locked(conn, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    /* --------------------------------------------------------
     * FIN_WAIT_1: we sent FIN, waiting for ACK of our FIN
     * -------------------------------------------------------- */
    case TCP_STATE_FIN_WAIT_1:
        /* ACK of our FIN */
        if (flags & TCP_FLAG_ACK) {
            take_ack(conn, ack);
            if (SEQ_GEQ(conn->snd_una, conn->snd_nxt)) conn->ack_of_fin = true;
        }
        /* data still in flight from the peer, and its FIN (simultaneous
           close); data here used to be dropped */
        if (take_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_FIN) != 0))
            tcp_send_segment_locked(conn, TCP_FLAG_ACK, NULL, 0);
        break;

    /* --------------------------------------------------------
     * FIN_WAIT_2: our FIN was ACKed, waiting for remote FIN
     * -------------------------------------------------------- */
    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FLAG_ACK) take_ack(conn, ack);
        /* remaining data, then the peer's FIN (tcp_close() also sends a
           final ACK once it sees fin_received) */
        if (take_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_FIN) != 0))
            tcp_send_segment_locked(conn, TCP_FLAG_ACK, NULL, 0);
        break;

    /* --------------------------------------------------------
     * CLOSE_WAIT: remote closed, we may still be sending
     * -------------------------------------------------------- */
    case TCP_STATE_CLOSE_WAIT:
        if (flags & TCP_FLAG_ACK) take_ack(conn, ack);
        break;

    /* --------------------------------------------------------
     * LAST_ACK: we sent our FIN after CLOSE_WAIT, waiting ACK
     * -------------------------------------------------------- */
    case TCP_STATE_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            if (SEQ_GEQ(ack, conn->snd_nxt)) {
                conn->snd_una = ack;
                conn->ack_of_fin = true;
                conn->state  = TCP_STATE_CLOSED;
                /* the slot is freed by tcp_close(), which is waiting for
                   this: freeing it here let another connection take the
                   slot while the closer still held it */
                serial_write_string("[TCP] LAST_ACK → CLOSED\n");
            }
        }
        break;

    /* --------------------------------------------------------
     * TIME_WAIT / CLOSED / others: drop
     * -------------------------------------------------------- */
    default:
        break;
    }

    /* Covers every switch case that falls through via break (SYN_SENT's
       bad-ACK path, FIN_WAIT_1/2, CLOSE_WAIT, LAST_ACK, TIME_WAIT/default)
       -- the two ESTABLISHED paths that `return` early wake explicitly
       above instead of falling through to here. */
    waitqueue_wake_all(&conn->rx_wq); waitqueue_wake_all(&g_poll_any_wq);
}

/* ============================================================
 * The connections as a list (netlink sock_diag: ss)
 * ============================================================ */
#include <wynland/inet_list.h>

int tcp_inet_list(InetSockEnt *out, int max)
{
    static const uint8_t lstate[] = {
        [TCP_STATE_CLOSED] = LTCP_CLOSE,           [TCP_STATE_LISTEN] = LTCP_LISTEN,
        [TCP_STATE_SYN_SENT] = LTCP_SYN_SENT,      [TCP_STATE_SYN_RECEIVED] = LTCP_SYN_RECV,
        [TCP_STATE_ESTABLISHED] = LTCP_ESTABLISHED, [TCP_STATE_FIN_WAIT_1] = LTCP_FIN_WAIT1,
        [TCP_STATE_FIN_WAIT_2] = LTCP_FIN_WAIT2,   [TCP_STATE_CLOSING] = LTCP_CLOSING,
        [TCP_STATE_TIME_WAIT] = LTCP_TIME_WAIT,    [TCP_STATE_CLOSE_WAIT] = LTCP_CLOSE_WAIT,
        [TCP_STATE_LAST_ACK] = LTCP_LAST_ACK,
    };
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_tcp_lock);
    for (uint32_t i = 0; i < TCP_MAX_CONNECTIONS && n < max; i++) {
        TcpConnection *c = &connections[i];
        if (!c->in_use) continue;
        InetSockEnt *e = &out[n++];
        e->lip = c->local_ip;     e->lport = c->local_port;
        e->rip = c->remote_ip;    e->rport = c->remote_port;
        e->state = c->state <= TCP_STATE_LAST_ACK ? lstate[c->state] : LTCP_CLOSE;
        e->rq = c->rx_len;
        e->wq = c->snd_nxt - c->snd_una;
        e->uid = 0;
    }
    spin_unlock_irqrestore(&g_tcp_lock, fl);
    return n;
}
