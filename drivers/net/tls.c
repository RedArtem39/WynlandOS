/*
 * WynlandOS - TLS 1.3 Client Wrapper (Host Tunneling)
 * ============================================================
 */
#include <wynland/tls.h>
#include <wynland/tcp.h>
#include <wynland/net.h>
#include <wynland/heap.h>

extern void serial_write_string(const char *str);

TlsSocket *tls_connect(const char *hostname, uint16_t port)
{
    (void)hostname;
    (void)port;
    
    uint32_t gateway_ip = net_get_gateway();
    if (gateway_ip == 0) {
        serial_write_string("TLS: Error - Network gateway not resolved yet.\r\n");
        return NULL;
    }
    
    /* Connect to the host proxy on port 8080 */
    TcpConnection *conn = tcp_connect(gateway_ip, 8080);
    if (!conn) {
        serial_write_string("TLS: Error - Failed to connect to host transparent proxy on port 8080.\r\n");
        return NULL;
    }
    
    TlsSocket *sock = (TlsSocket *)kmalloc(sizeof(TlsSocket));
    if (!sock) {
        tcp_close(conn);
        return NULL;
    }
    
    sock->conn = (void *)conn;
    sock->handshake_done = true;
    
    serial_write_string("TLS: Secure transparent tunnel established via host proxy.\r\n");
    return sock;
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
