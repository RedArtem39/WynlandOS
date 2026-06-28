/*
 * WynlandOS - TLS 1.3 Client Header
 * ============================================================
 */
#pragma once

#include <wynland/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *conn;
    
    /* Encryption keys */
    uint8_t client_write_key[16];
    uint8_t client_write_iv[12];
    uint8_t server_write_key[16];
    uint8_t server_write_iv[12];
    
    uint64_t client_sequence;
    uint64_t server_sequence;
    
    bool handshake_done;
} TlsSocket;

/* TLS socket operations */
TlsSocket *tls_connect(const char *hostname, uint16_t port);
int tls_send(TlsSocket *sock, const void *data, uint32_t len);
int tls_recv(TlsSocket *sock, void *buf, uint32_t max_len);
void tls_close(TlsSocket *sock);

#ifdef __cplusplus
}
#endif
