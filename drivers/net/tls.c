/*
 * WynlandOS - kernel TLS stub (no TLS here: see tls_connect())
 * ============================================================
 */
#include <wynland/tls.h>
#include <wynland/tcp.h>
#include <wynland/net.h>
#include <wynland/heap.h>

extern void serial_write_string(const char *str);

/* There is no TLS in the kernel. This used to open plain TCP to the
   gateway's port 8080 (in QEMU's user network: the host itself), mark
   the "handshake" done and send everything in clear text -- callers
   believed they had an encrypted channel. Refusing is the honest answer;
   HTTPS on WynlandOS is curl/LibreSSL in user space. */
TlsSocket *tls_connect(const char *hostname, uint16_t port)
{
    (void)hostname;
    (void)port;
    serial_write_string("TLS: not available in the kernel (use curl / LibreSSL in user space)\r\n");
    return NULL;
}

int tls_send(TlsSocket *sock, const void *data, uint32_t len)
{
    if (!sock || !sock->conn) return -1;
    return tcp_send(sock->conn, data, len);
}

int tls_recv(TlsSocket *sock, void *buf, uint32_t max_len)
{
    if (!sock || !sock->conn) return -1;
    return tcp_recv(sock->conn, buf, max_len);
}

void tls_close(TlsSocket *sock)
{
    if (!sock) return;
    if (sock->conn) {
        tcp_close(sock->conn);
    }
    kfree(sock);
    serial_write_string("TLS: Tunnel closed.\r\n");
}
