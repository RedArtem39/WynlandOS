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

/* Sequence-number comparisons modulo 2^32: plain < and >= broke when the
   numbers wrapped. */
#define SEQ_LT(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define SEQ_GT(a, b)  SEQ_LT(b, a)
#define SEQ_GEQ(a, b) SEQ_LEQ(b, a)

/* free receive space, as advertised (no window scaling: at most 65535) */
static uint16_t rx_window(const TcpConnection *c)
{
    uint32_t room = TCP_RX_BUF_SIZE - c->rx_len;
    return (uint16_t)(room > 65535 ? 65535 : room);
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

static bool tcp_send_segment(TcpConnection *conn, uint8_t flags,
                             const void *data, uint32_t data_len)
{
    uint8_t seg_buf[sizeof(TcpHeader) + NET_PKT_BUF];
    TcpHeader *tcp;
    uint32_t tcp_total;
    uint16_t cksum;

    tcp_total = sizeof(TcpHeader) + data_len;
    if (data_len > NET_PKT_BUF)
        return false;

    tcp = (TcpHeader *)seg_buf;
    tcp->src_port   = htons(conn->local_port);
    tcp->dst_port   = htons(conn->remote_port);
    tcp->seq_num    = htonl(conn->snd_nxt);
    tcp->ack_num    = htonl(conn->rcv_nxt);
    tcp->data_offset = (uint8_t)((sizeof(TcpHeader) / 4) << 4);  /* 0x50 */
    tcp->flags      = flags;
    tcp->window     = htons(rx_window(conn));
    conn->wnd_small = rx_window(conn) < TCP_MSS;
    tcp->checksum   = 0;
    tcp->urgent_ptr = 0;

    /* Copy payload after header */
    if (data_len > 0 && data != NULL) {
        memcpy(seg_buf + sizeof(TcpHeader), data, data_len);
    }

    /* Compute TCP checksum with pseudo-header */
    cksum = tcp_checksum(conn->local_ip, conn->remote_ip,
                         seg_buf, tcp_total);
    tcp->checksum = htons(cksum);

    return net_send_ipv4(conn->remote_ip, IP_PROTO_TCP, seg_buf, tcp_total);
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

    /* Initialise the connection (TCB) */
    memset(conn, 0, sizeof(TcpConnection));
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

    /* Busy-poll for SYN-ACK (an internet round trip: tens of ms) */
    uint64_t syn_deadline = net_deadline_ms(TCP_CONNECT_TIMEOUT_MS);
    for (i = 0; !net_past(syn_deadline); i++) {
        net_wait_tick();
        net_poll();

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
 * Splits into TCP_MSS-sized chunks.  Each chunk is sent with
 * PSH|ACK and a busy-wait for ACK.  Basic retransmit on timeout.
 * ============================================================ */

int tcp_send(TcpConnection *conn, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t total_sent = 0;

    if (conn == NULL || conn->state != TCP_STATE_ESTABLISHED)
        return -1;

    while (total_sent < len) {
        uint32_t chunk = len - total_sent;
        uint32_t retries;
        bool     acked;

        if (chunk > TCP_MSS)
            chunk = TCP_MSS;

        acked = false;

        for (retries = 0; retries < TCP_RETRANSMIT_MAX; retries++) {
            /* Set snd_nxt to snd_una to ensure first send or retransmission uses correct seq */
            conn->snd_nxt = conn->snd_una;

            uint32_t expected_ack = conn->snd_nxt + chunk;

            if (!tcp_send_segment(conn, TCP_FLAG_PSH | TCP_FLAG_ACK,
                                  p + total_sent, chunk)) {
                serial_write_string("[TCP] Failed to send data segment\n");
                return (total_sent > 0) ? (int)total_sent : -1;
            }

            /* Advance snd_nxt immediately to validate incoming ACK */
            conn->snd_nxt = expected_ack;

            /* Real blocking wait for ACK, bounded to one RTO-ish window per
               attempt (~1s) -- this genuinely needs a real timeout, that's
               what triggers TCP_RETRANSMIT_MAX's retransmit, not something
               to block on forever like tcp_recv(). net_poll() no longer
               needs calling here either way (timer-tick-driven now, see
               kernel/irq.c). */
            {
                uint64_t ack_deadline = timer_get_ticks() + 100;
                for (;;) {
                    if (conn->reset_received) {
                        serial_write_string("[TCP] Connection reset during send\n");
                        conn->state = TCP_STATE_CLOSED;
                        return -1;
                    }

                    if (SEQ_GEQ(conn->snd_una, expected_ack)) {
                        acked = true;
                        break;
                    }

                    if (timer_get_ticks() >= ack_deadline) break; /* RTO expired -- go retransmit */

                    waitqueue_wait(&conn->rx_wq, ack_deadline);
                }
            }

            if (acked)
                break;

            serial_write_string("[TCP] ACK timeout, retransmitting\n");
        }

        if (!acked) {
            serial_write_string("[TCP] Max retransmits reached\n");
            return (total_sent > 0) ? (int)total_sent : -1;
        }

        /* Advance past the acked data */
        conn->snd_nxt = conn->snd_una;
        total_sent += chunk;
    }

    return (int)total_sent;
}

/* ============================================================
 * tcp_recv — Receive data from connection
 *
 * If data is already in rx_buf, copy and return immediately.
 * Otherwise blocks for real until data/FIN/RST (Phase 22d).
 * ============================================================ */

int tcp_recv(TcpConnection *conn, void *buf, uint32_t max_len)
{
    uint32_t copy_len;

    if (conn == NULL || (conn->state != TCP_STATE_ESTABLISHED &&
                         conn->state != TCP_STATE_CLOSE_WAIT))
        return -1;

    /* If data already buffered, return it immediately */
    if (conn->rx_len > 0) {
        copy_len = conn->rx_len;
        if (copy_len > max_len)
            copy_len = max_len;
        memcpy(buf, conn->rx_buf, copy_len);

        /* Shift remaining data forward */
        if (copy_len < conn->rx_len) {
            uint32_t remaining = conn->rx_len - copy_len;
            uint32_t j;
            for (j = 0; j < remaining; j++) {
                conn->rx_buf[j] = conn->rx_buf[copy_len + j];
            }
            conn->rx_len = remaining;
        } else {
            conn->rx_len = 0;
        }

        conn->data_available = (conn->rx_len > 0);
        if (conn->wnd_small && rx_window(conn) >= 2 * TCP_MSS)
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);   /* window update */
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

    copy_len = conn->rx_len;
    if (copy_len > max_len)
        copy_len = max_len;
    memcpy(buf, conn->rx_buf, copy_len);

    /* Shift remaining data forward */
    if (copy_len < conn->rx_len) {
        uint32_t remaining = conn->rx_len - copy_len;
        uint32_t j;
        for (j = 0; j < remaining; j++) {
            conn->rx_buf[j] = conn->rx_buf[copy_len + j];
        }
        conn->rx_len = remaining;
    } else {
        conn->rx_len = 0;
    }

    conn->data_available = (conn->rx_len > 0);
    if (conn->wnd_small && rx_window(conn) >= 2 * TCP_MSS)
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);   /* window update */
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
    if (SEQ_GT(ack, c->snd_una) && SEQ_LEQ(ack, c->snd_nxt)) c->snd_una = ack;
}

/* Receive in order only: bytes we already have are trimmed, a segment
   past a gap is dropped (the ACK we send back tells the peer where we
   are, so it resends), and only what fits in the buffer is taken --
   rcv_nxt moves by exactly what was stored. The FIN counts once every
   byte before it is in. Every segment used to be appended whatever its
   sequence number (duplicates and reordering corrupted the stream) and
   data past a full buffer was ACKed and lost. Returns whether the
   segment needs an ACK. */
static bool take_data(TcpConnection *c, uint32_t seq, const uint8_t *p, uint32_t len, bool fin)
{
    if (len == 0 && !fin) return false;
    if (SEQ_GT(seq, c->rcv_nxt)) return true;           /* gap: dup ACK */
    uint32_t skip = c->rcv_nxt - seq;                    /* already have these */
    if (skip > len) return true;                         /* all old (a resent FIN too) */
    uint32_t fresh = len - skip;
    uint32_t room = TCP_RX_BUF_SIZE - c->rx_len;
    uint32_t take = fresh < room ? fresh : room;
    if (take) {
        memcpy(c->rx_buf + c->rx_len, p + skip, take);
        c->rx_len += take;
        c->rcv_nxt += take;
        c->data_available = true;
    }
    if (fin && take == fresh && !c->fin_received) {
        c->rcv_nxt += 1;                                 /* FIN takes one number */
        c->fin_received = true;
    }
    return true;
}

/* ============================================================
 * tcp_handle_packet — Incoming TCP segment handler
 *
 * Called from handle_ipv4 in net.c when protocol == TCP.
 * Parses the TcpHeader, finds the matching connection, and
 * processes based on the current connection state.
 * ============================================================ */

void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip,
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
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
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
            conn->snd_wnd = ntohs(tcp->window);

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

        if (flags & TCP_FLAG_ACK) take_ack(conn, ack);
        {
            bool had_fin = conn->fin_received;
            bool need_ack = take_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_FIN) != 0);
            if (conn->fin_received && !had_fin) {
                serial_write_string("[TCP] FIN received (ESTABLISHED)\n");
                conn->state = TCP_STATE_CLOSE_WAIT;
            }
            if (need_ack) tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
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
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
        break;

    /* --------------------------------------------------------
     * FIN_WAIT_2: our FIN was ACKed, waiting for remote FIN
     * -------------------------------------------------------- */
    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FLAG_ACK) take_ack(conn, ack);
        /* remaining data, then the peer's FIN (tcp_close() also sends a
           final ACK once it sees fin_received) */
        if (take_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_FIN) != 0))
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
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
