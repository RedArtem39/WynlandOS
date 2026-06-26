/*
 * WynlandOS - TCP Protocol Implementation
 *
 * Phase 5.8: TCP — 3-way handshake, data transfer, flow control, graceful close
 *
 * Implements: tcp_init, tcp_connect, tcp_send, tcp_recv, tcp_close,
 *             tcp_handle_packet, and internal helpers.
 */

#include <wynland/tcp.h>
#include <wynland/net.h>
#include <wynland/types.h>

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
static uint16_t      ephemeral_port_counter;
static uint32_t      iss_counter;   /* Simple ISS generator */

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
    tcp->window     = htons(conn->rcv_wnd);
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
    ephemeral_port_counter = 0;
    iss_counter = 1000;

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
    conn->local_port  = TCP_EPHEMERAL_BASE + ephemeral_port_counter++;
    conn->remote_ip   = remote_ip;
    conn->remote_port = remote_port;

    /* Sequence numbers */
    conn->snd_iss = iss_counter;
    iss_counter  += 64000;  /* Advance ISS for next connection */
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

    /* Busy-poll for SYN-ACK */
    for (i = 0; i < TCP_CONNECT_TIMEOUT; i++) {
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
            uint32_t wait;

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

            /* Busy-poll for ACK */
            for (wait = 0; wait < TCP_RECV_TIMEOUT; wait++) {
                net_poll();

                if (conn->reset_received) {
                    serial_write_string("[TCP] Connection reset during send\n");
                    conn->state = TCP_STATE_CLOSED;
                    return -1;
                }

                /* Check if the remote acknowledged our data */
                if (conn->snd_una >= expected_ack) {
                    acked = true;
                    break;
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
 * Otherwise busy-poll with net_poll() up to TCP_RECV_TIMEOUT.
 * ============================================================ */

int tcp_recv(TcpConnection *conn, void *buf, uint32_t max_len)
{
    uint32_t i;
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
        return (int)copy_len;
    }

    /* No data buffered — busy-poll */
    conn->data_available = false;

    for (i = 0; i < TCP_RECV_TIMEOUT; i++) {
        net_poll();

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
    }

    /* Copy whatever we have */
    if (conn->rx_len == 0)
        return -1;  /* Timeout with no data */

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
        for (i = 0; i < TCP_CLOSE_TIMEOUT; i++) {
            net_poll();
            if (conn->state == TCP_STATE_CLOSED || conn->reset_received) {
                break;
            }
        }
        conn->in_use = false;
        return;
    }

    /* ---- Wait for ACK of our FIN (FIN_WAIT_1 → FIN_WAIT_2) ---- */
    for (i = 0; i < TCP_CLOSE_TIMEOUT; i++) {
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
    for (i = 0; i < TCP_CLOSE_TIMEOUT; i++) {
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
        serial_write_string("[TCP] RST received\n");
        conn->reset_received = true;
        conn->state = TCP_STATE_CLOSED;
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

        /* Update send unacknowledged pointer if ACK flag is set */
        if (flags & TCP_FLAG_ACK) {
            if (ack >= conn->snd_una && ack <= conn->snd_nxt) {
                conn->snd_una = ack;
            }
        }

        /* Handle incoming data */
        if (payload_len > 0) {
            /* Copy to receive buffer if space available */
            if (conn->rx_len + payload_len <= TCP_RX_BUF_SIZE) {
                memcpy(conn->rx_buf + conn->rx_len, payload, payload_len);
                conn->rx_len += payload_len;
            } else {
                serial_write_string("[TCP] RX buffer overflow, dropping\n");
            }

            /* Advance rcv_nxt */
            conn->rcv_nxt = seq + payload_len;
            conn->data_available = true;
        }

        /* Handle FIN */
        if (flags & TCP_FLAG_FIN) {
            serial_write_string("[TCP] FIN received (ESTABLISHED)\n");
            conn->rcv_nxt = seq + payload_len + 1;  /* FIN consumes one seq */
            conn->fin_received = true;
            conn->state = TCP_STATE_CLOSE_WAIT;

            /* ACK the FIN */
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            return;
        }

        /* If we had data but no FIN, ACK it */
        if (payload_len > 0) {
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            return;
        }
        break;

    /* --------------------------------------------------------
     * FIN_WAIT_1: we sent FIN, waiting for ACK of our FIN
     * -------------------------------------------------------- */
    case TCP_STATE_FIN_WAIT_1:
        /* ACK of our FIN */
        if (flags & TCP_FLAG_ACK) {
            if (ack >= conn->snd_una && ack <= conn->snd_nxt) {
                conn->snd_una = ack;
                if (ack >= conn->snd_nxt) {
                    conn->ack_of_fin = true;
                }
            }
        }

        /* Remote also sending FIN (simultaneous close) */
        if (flags & TCP_FLAG_FIN) {
            conn->rcv_nxt = seq + 1;
            conn->fin_received = true;

            /* ACK the remote FIN */
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
        }
        break;

    /* --------------------------------------------------------
     * FIN_WAIT_2: our FIN was ACKed, waiting for remote FIN
     * -------------------------------------------------------- */
    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FLAG_ACK) {
            if (ack >= conn->snd_una && ack <= conn->snd_nxt) {
                conn->snd_una = ack;
            }
        }

        if (flags & TCP_FLAG_FIN) {
            conn->rcv_nxt = seq + 1;  /* FIN consumes one seq */
            conn->fin_received = true;

            /* Final ACK is sent by tcp_close() after detecting fin_received */
        }

        /* Also absorb any data that arrives before FIN */
        if (payload_len > 0) {
            if (conn->rx_len + payload_len <= TCP_RX_BUF_SIZE) {
                memcpy(conn->rx_buf + conn->rx_len, payload, payload_len);
                conn->rx_len += payload_len;
            }
            conn->rcv_nxt = seq + payload_len;
            if (flags & TCP_FLAG_FIN)
                conn->rcv_nxt += 1;
            tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            conn->data_available = true;
        }
        break;

    /* --------------------------------------------------------
     * CLOSE_WAIT: remote closed, we may still be sending
     * -------------------------------------------------------- */
    case TCP_STATE_CLOSE_WAIT:
        if (flags & TCP_FLAG_ACK) {
            if (ack >= conn->snd_una && ack <= conn->snd_nxt) {
                conn->snd_una = ack;
            }
        }
        break;

    /* --------------------------------------------------------
     * LAST_ACK: we sent our FIN after CLOSE_WAIT, waiting ACK
     * -------------------------------------------------------- */
    case TCP_STATE_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            if (ack >= conn->snd_nxt) {
                conn->snd_una = ack;
                conn->ack_of_fin = true;
                conn->state  = TCP_STATE_CLOSED;
                conn->in_use = false;
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
}
