/*
 * WynlandOS - generic UDP client socket layer implementation.
 * See include/wynland/udpsock.h for the design note.
 */
#include <wynland/udpsock.h>
#include <wynland/net.h>
#include <wynland/waitqueue.h>

extern bool net_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                         const void *data, uint32_t len);
extern uint64_t timer_get_ticks(void);

/* Phase 22d: bounded retry slice for real blocking recvfrom(), same
   reasoning as KPipe/TcpConnection (see kernel/syscall.c / drivers/net/tcp.c) --
   a real wake fires immediately via udp_socket_deliver()'s
   waitqueue_wake_all() when a datagram arrives, this only bounds the
   pathological case of nothing ever arriving. */
#define UDP_WAIT_RETRY_TICKS 500

typedef struct {
    bool     in_use;
    uint16_t local_port;

    /* "Connected" default peer (set by udp_socket_connect, used when
       a caller passes dst_ip==0 to udp_socket_sendto). */
    uint32_t remote_ip;
    uint16_t remote_port;

    /* Single-datagram receive buffer -- last datagram delivered,
       overwritten by the next one if the caller hasn't drained it yet.
       Simple and sufficient for a request/response client like a DNS
       resolver; not a real multi-datagram queue. */
    uint8_t  rx_buf[UDP_RX_BUF_SIZE];
    uint32_t rx_len;
    uint32_t rx_from_ip;
    uint16_t rx_from_port;
    volatile bool data_available;
    WaitQueue rx_wq; /* Phase 22d: real blocking recvfrom() */
} UdpSocket;

static UdpSocket sockets[UDP_MAX_SOCKETS];
static uint16_t  ephemeral_counter;

void udp_socket_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    ephemeral_counter = 0;
}

int udp_socket_create(void)
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            memset(&sockets[i], 0, sizeof(UdpSocket));
            sockets[i].in_use     = true;
            sockets[i].local_port = UDP_EPHEMERAL_BASE + ephemeral_counter++;
            return i;
        }
    }
    return -1;
}

bool udp_socket_connect(int idx, uint32_t remote_ip, uint16_t remote_port)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS || !sockets[idx].in_use)
        return false;
    sockets[idx].remote_ip   = remote_ip;
    sockets[idx].remote_port = remote_port;
    return true;
}

int udp_socket_sendto(int idx, const void *data, uint32_t len,
                      uint32_t dst_ip, uint16_t dst_port)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS || !sockets[idx].in_use)
        return -1;

    UdpSocket *s = &sockets[idx];
    uint32_t   real_ip   = dst_ip   ? dst_ip   : s->remote_ip;
    uint16_t   real_port = dst_port ? dst_port : s->remote_port;

    if (!net_send_udp(real_ip, s->local_port, real_port, data, len))
        return -1;
    return (int)len;
}

int udp_socket_recvfrom(int idx, void *buf, uint32_t max_len,
                        uint32_t *from_ip, uint16_t *from_port)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS || !sockets[idx].in_use)
        return -1;

    UdpSocket *s = &sockets[idx];

    /* Real blocking recvfrom(): net_poll() is timer-tick-driven now (see
       kernel/irq.c), so this only needs to wait for udp_socket_deliver()
       to wake it -- no more busy-spinning net_poll() itself. */
    while (!(s->data_available && s->rx_len > 0)) {
        waitqueue_wait(&s->rx_wq, timer_get_ticks() + UDP_WAIT_RETRY_TICKS);
    }

    uint32_t copy_len = s->rx_len;
    if (copy_len > max_len)
        copy_len = max_len;
    memcpy(buf, s->rx_buf, copy_len);

    if (from_ip)   *from_ip   = s->rx_from_ip;
    if (from_port) *from_port = s->rx_from_port;

    s->rx_len         = 0;
    s->data_available = false;
    return (int)copy_len;
}

void udp_socket_close(int idx)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS)
        return;
    sockets[idx].in_use = false;
}

uint16_t udp_socket_local_port(int idx)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS || !sockets[idx].in_use)
        return 0;
    return sockets[idx].local_port;
}

bool udp_socket_remote(int idx, uint32_t *ip, uint16_t *port)
{
    if (idx < 0 || idx >= UDP_MAX_SOCKETS || !sockets[idx].in_use || !sockets[idx].remote_ip)
        return false;
    *ip = sockets[idx].remote_ip;
    *port = sockets[idx].remote_port;
    return true;
}

bool udp_socket_deliver(uint32_t src_ip, uint16_t src_port,
                        uint16_t dst_port, const void *data, uint32_t len)
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].local_port == dst_port) {
            uint32_t copy_len = len;
            if (copy_len > UDP_RX_BUF_SIZE)
                copy_len = UDP_RX_BUF_SIZE;
            memcpy(sockets[i].rx_buf, data, copy_len);
            sockets[i].rx_len         = copy_len;
            sockets[i].rx_from_ip     = src_ip;
            sockets[i].rx_from_port   = src_port;
            sockets[i].data_available = true;
            waitqueue_wake_all(&sockets[i].rx_wq);
            return true;
        }
    }
    return false;
}
