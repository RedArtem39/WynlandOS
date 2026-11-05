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
/* A browser keeps dozens of connections open, and an 8 KB window made
   every download crawl: more slots, and a 256 KB ring whose free space is
   the window we advertise (window scaling: past 64 KB, which at a mobile
   link's 100 ms round trip held one connection to ~0.6 MB/s). */
#define TCP_MAX_CONNECTIONS    32
#define TCP_RX_BUF_SIZE        262144
#define TCP_RCV_WSCALE         3         /* our window in units of 8 bytes (a 256 KB one fits 16 bits) */
#define TCP_OOO_MAX            4         /* out-of-order ranges kept (and SACKed) */
#define TCP_WINDOW_SIZE        65535
#define TCP_MSS                1460  /* Max Segment Size (ETH_MTU - IP_HDR - TCP_HDR) */
#define TCP_CONNECT_TIMEOUT    5000000   /* Poll iterations for connect */
#define TCP_RECV_TIMEOUT       5000000   /* Poll iterations for recv */
#define TCP_CLOSE_TIMEOUT      2000000   /* Poll iterations for close */
/* Wall-clock limits (net_deadline_ms). Both run with interrupts off, so
   they stall the whole system while they wait: keep them short. A close
   only waits for the peer's FIN/ACK as a courtesy. */
#define TCP_CONNECT_TIMEOUT_MS 15000     /* the SYN sent again at 1, 3 and 7 s */
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
    uint16_t snd_wnd;        /* Send window (from receiver), as sent */
    uint32_t snd_wnd32;      /* ... scaled: in bytes */
    uint32_t snd_max;        /* the highest sequence number sent (an ACK up to it is good) */
    uint16_t snd_mss;        /* the peer's MSS (536 when it says none) */
    uint8_t  snd_wscale;     /* the peer's window shift */
    bool     wscale_ok;      /* both sides scale their windows */
    bool     sack_ok;        /* the peer takes SACK blocks */

    /* Receive sequence variables */
    uint32_t rcv_nxt;        /* Next expected sequence number */
    uint32_t rcv_irs;        /* Initial receive sequence number */
    uint16_t rcv_wnd;        /* Receive window (we advertise) */

    /* Connection state */
    uint8_t  state;
    bool     in_use;

    /* Receive ring (TCP_RX_BUF_SIZE bytes, allocated once per slot and
       kept): rx_len bytes in order from rx_head, then -- at their places
       relative to rcv_nxt -- what came past a gap, its ranges in ooo_* */
    uint8_t *rx_buf;
    uint32_t rx_head;
    uint32_t rx_len;         /* Bytes available, in order */
    uint32_t ooo_s[TCP_OOO_MAX], ooo_e[TCP_OOO_MAX];   /* [start, end) sequence numbers */
    uint8_t  ooo_n;
    bool     fin_pending;    /* the peer's FIN came past a gap: taken at fin_seq */
    uint32_t fin_seq;
    uint32_t adv_wnd;        /* the window last advertised (bytes) */
    bool     wnd_small;      /* advertised less than an MSS: tell the peer when it opens */

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
