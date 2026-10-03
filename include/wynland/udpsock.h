/*
 * WynlandOS - generic UDP client socket layer
 *
 * A small per-socket abstraction (mirrors drivers/net/tcp.c's
 * TcpConnection table) sitting on top of net.c's existing
 * net_send_udp()/net_poll() primitives, so that a real Ring-3
 * process can open an ordinary AF_INET/SOCK_DGRAM socket and use
 * it for arbitrary traffic -- not just the OS's own hardcoded
 * Ring-0 DHCP/DNS clients in net.c, which predate this and stay
 * untouched. The motivating use case is letting musl's real,
 * unmodified DNS resolver (socket()+sendto()/recvfrom() to
 * /etc/resolv.conf's nameserver) work for real Ring-3 programs.
 */
#pragma once

#include <wynland/types.h>

#define UDP_MAX_SOCKETS      16
#define UDP_RX_BUF_SIZE      2048
#define UDP_RX_SLOTS         8        /* datagrams queued per socket */
#define UDP_EPHEMERAL_BASE   50000
#define UDP_RECV_TIMEOUT     3000000  /* poll iterations */

void udp_socket_init(void);

/* Allocates a socket + assigns an ephemeral local port. Returns a
   slot index (0..UDP_MAX_SOCKETS-1) or -1 if none free. */
int  udp_socket_create(void);

/* Sets the socket's "connected" default peer (real UDP connect()
   semantics: no handshake, just remembers where plain send()/write()
   and a NULL-dest sendto() should go). */
bool udp_socket_connect(int idx, uint32_t remote_ip, uint16_t remote_port);

/* dst_ip == 0 means "use the connected peer" (set via udp_socket_connect).
   Returns bytes sent, or -1 on failure. */
int  udp_socket_sendto(int idx, const void *data, uint32_t len,
                       uint32_t dst_ip, uint16_t dst_port);

/* Busy-polls (bounded by UDP_RECV_TIMEOUT) for a datagram addressed to
   this socket's local port. Returns bytes received (>=0) or -1 on
   timeout. from_ip/from_port (may be NULL) receive the sender's address. */
int  udp_socket_recvfrom(int idx, void *buf, uint32_t max_len,
                         uint32_t *from_ip, uint16_t *from_port);

void udp_socket_close(int idx);

uint16_t udp_socket_local_port(int idx);

/* A datagram is waiting (poll(), non-blocking reads); the queue blocking
   readers sleep on. */
bool udp_socket_readable(int idx);
struct WaitQueue *udp_socket_wq(int idx);

/* The peer set by udp_socket_connect() (ip in network order, port in host
   order); false when not connected. */
bool udp_socket_remote(int idx, uint32_t *ip, uint16_t *port);

/* Called by net.c's handle_udp() for any datagram not consumed by the
   OS's own legacy DHCP/DNS Ring-0 clients. Returns true if some live
   socket's local_port matched and the datagram was delivered. */
bool udp_socket_deliver(uint32_t src_ip, uint16_t src_port,
                        uint16_t dst_port, const void *data, uint32_t len);
