/*
 * WynlandOS - the machine's TCP sockets as a list, for netlink's sock_diag
 * (ss): the IP stack's connections (drivers/net/tcp.c) and the loopback
 * ones, which are Unix sockets named "\0inet:PORT" (kernel/unix_socket.c).
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#ifndef WYNLAND_INET_LIST_H
#define WYNLAND_INET_LIST_H

#include <wynland/types.h>

/* Linux's TCP states (include/net/tcp_states.h) */
enum {
    LTCP_ESTABLISHED = 1, LTCP_SYN_SENT, LTCP_SYN_RECV, LTCP_FIN_WAIT1, LTCP_FIN_WAIT2,
    LTCP_TIME_WAIT, LTCP_CLOSE, LTCP_CLOSE_WAIT, LTCP_LAST_ACK, LTCP_LISTEN, LTCP_CLOSING
};

typedef struct {
    uint32_t lip, rip;       /* network byte order */
    uint16_t lport, rport;   /* host byte order */
    uint8_t  state;          /* LTCP_* */
    uint32_t rq, wq;         /* bytes waiting to be read / acknowledged */
    uint32_t uid;
} InetSockEnt;

int tcp_inet_list(InetSockEnt *out, int max);     /* drivers/net/tcp.c */
int usock_inet_list(InetSockEnt *out, int max);   /* kernel/unix_socket.c */

/* the network card: 0 none, 1 virtio, 2 USB, 3 Wi-Fi (drivers/net/net.c) */
int net_nic_kind(void);

#endif
