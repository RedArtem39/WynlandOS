/*
 * WynlandOS - TCP Protocol Header
 *
 * Phase 5.8: TCP — 3-way handshake, data transfer, flow control, graceful close
 */
#pragma once

#include <wynland/types.h>
#include <wynland/net.h>
#include <wynland/waitqueue.h>

/* ============================================================
 * TCP Constants
 * ============================================================ */

/* TCP Flags */
#define TCP_FLAG_FIN   0x01
#define TCP_FLAG_SYN   0x02
#define TCP_FLAG_RST   0x04
#define TCP_FLAG_PSH   0x08
#define TCP_FLAG_ACK   0x10
#define TCP_FLAG_URG   0x20

/* TCP States */
#define TCP_STATE_CLOSED       0
#define TCP_STATE_LISTEN       1
#define TCP_STATE_SYN_SENT     2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED  4
#define TCP_STATE_FIN_WAIT_1   5
#define TCP_STATE_FIN_WAIT_2   6
#define TCP_STATE_CLOSING      7
#define TCP_STATE_TIME_WAIT    8
#define TCP_STATE_CLOSE_WAIT   9
#define TCP_STATE_LAST_ACK     10

/* Limits */
#define TCP_MAX_CONNECTIONS    8
#define TCP_RX_BUF_SIZE        16384
#define TCP_WINDOW_SIZE        8192
#define TCP_MSS                1460  /* Max Segment Size (ETH_MTU - IP_HDR - TCP_HDR) */
#define TCP_CONNECT_TIMEOUT    5000000   /* Poll iterations for connect */
#define TCP_RECV_TIMEOUT       5000000   /* Poll iterations for recv */
#define TCP_CLOSE_TIMEOUT      2000000   /* Poll iterations for close */
/* Wall-clock limits (net_deadline_ms). Both run with interrupts off, so
   they stall the whole system while they wait: keep them short. A close
   only waits for the peer's FIN/ACK as a courtesy. */
#define TCP_CONNECT_TIMEOUT_MS 5000
#define TCP_CLOSE_TIMEOUT_MS   300
#define TCP_RETRANSMIT_MAX     5
#define TCP_EPHEMERAL_BASE     49152     /* Ephemeral port range start */

/* ============================================================
 * TCP Header
 * ============================================================ */

typedef struct {
    uint16_t src_port;       /* Network byte order */
    uint16_t dst_port;       /* Network byte order */
    uint32_t seq_num;        /* Network byte order */
    uint32_t ack_num;        /* Network byte order */
    uint8_t  data_offset;    /* Upper 4 bits: header length in 32-bit words */
    uint8_t  flags;          /* TCP flags (FIN, SYN, RST, PSH, ACK, URG) */
    uint16_t window;         /* Network byte order */
    uint16_t checksum;       /* Network byte order */
    uint16_t urgent_ptr;     /* Network byte order */
} __attribute__((packed)) TcpHeader;

/* ============================================================
 * TCP Connection (Transmission Control Block)
 * ============================================================ */

typedef struct {
    /* Connection identification (4-tuple) */
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    /* Send sequence variables */
    uint32_t snd_una;        /* Oldest unacknowledged sequence number */
    uint32_t snd_nxt;        /* Next sequence number to send */
    uint32_t snd_iss;        /* Initial send sequence number */
    uint16_t snd_wnd;        /* Send window (from receiver) */

    /* Receive sequence variables */
    uint32_t rcv_nxt;        /* Next expected sequence number */
    uint32_t rcv_irs;        /* Initial receive sequence number */
    uint16_t rcv_wnd;        /* Receive window (we advertise) */

    /* Connection state */
    uint8_t  state;
    bool     in_use;

    /* Receive buffer */
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint32_t rx_len;         /* Bytes available in rx_buf */

    /* Synchronisation flags (volatile for busy-wait loops) */
    volatile bool syn_ack_received;
    volatile bool fin_received;
    volatile bool data_available;
    volatile bool reset_received;
    volatile bool ack_of_fin;

    /* Phase 22d: real blocking recv/send. Woken from tcp_handle_packet()
       (net.c's IRQ0-timer-driven net_poll(), not a caller's own busy-spin
       -- see kernel/irq.c) whenever any of the flags/state above change. */
    WaitQueue rx_wq;
} TcpConnection;

/* ============================================================
 * TCP Public API
 * ============================================================ */

/* Initialise TCP subsystem (zeroes all connections) */
void tcp_init(void);

/* Active open: 3-way handshake to remote host, returns connection or NULL */
TcpConnection *tcp_connect(uint32_t remote_ip, uint16_t remote_port);

/* Send data over an established connection.  Returns bytes sent or -1 */
int tcp_send(TcpConnection *conn, const void *data, uint32_t len);

/* Receive data from connection.  Returns bytes received or -1 */
int tcp_recv(TcpConnection *conn, void *buf, uint32_t max_len);

/* Graceful close (FIN handshake) */
void tcp_close(TcpConnection *conn);

/* Active open returning a connection-table slot index instead of a
   pointer, so callers that can only store a uint32_t per-fd index
   (e.g. syscall.c's VfsFile.current_cluster, matching the existing
   pipe/SHM convention) can still reach a real connection. Returns
   -1 on failure. */
int tcp_connect_slot(uint32_t remote_ip, uint16_t remote_port);

/* Bounds-checked lookup of a connection by slot index. Returns NULL
   if the index is out of range or the slot isn't a live connection
   (e.g. it was already closed). */
TcpConnection *tcp_get_connection(int idx);

/* ============================================================
 * Internal — called by the IPv4 handler in net.c
 * ============================================================ */

/* Dispatch an incoming TCP segment (called from handle_ipv4) */
void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip,
                       const uint8_t *data, uint32_t len);
