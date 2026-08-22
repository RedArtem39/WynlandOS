/*
 * WynlandOS - real userspace TCP verification.
 * Uses musl's real socket()/connect()/write()/read()/close() -- routed
 * through the new SYS_socket/SYS_connect/SYS_sendto/SYS_recvfrom kernel
 * wiring added on top of the pre-existing kernel-level TCP stack
 * (drivers/net/tcp.c) -- to open a real TCP connection to a known public
 * IP (1.1.1.1:80, Cloudflare) over the real network (QEMU SLIRP NAT ->
 * WSL host -> internet), issue a plain HTTP/1.0 GET, and confirm a real
 * HTTP response comes back.
 *
 * DNS is not wired to userspace sockets yet, so the target IP is
 * hardcoded rather than resolved by name.
 *
 * Build:
 *   x86_64-linux-musl-gcc -O2 -o test_tcp.elf test_tcp.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void) {
    printf("test_tcp: creating AF_INET/SOCK_STREAM socket\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("FAIL: socket() returned %d\n", fd);
        return 1;
    }
    printf("test_tcp: socket() fd=%d\n", fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(80);
    addr.sin_addr.s_addr = inet_addr("1.1.1.1");

    printf("test_tcp: calling connect() to 1.1.1.1:80\n");
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0) {
        printf("FAIL: connect() returned %d\n", rc);
        close(fd);
        return 1;
    }
    printf("test_tcp: connect() succeeded -- real TCP 3-way handshake completed\n");

    const char *req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
    int req_len = (int)strlen(req);
    int sent = (int)write(fd, req, req_len);
    printf("test_tcp: wrote %d/%d bytes of HTTP request\n", sent, req_len);
    if (sent != req_len) {
        printf("FAIL: short write on request\n");
        close(fd);
        return 1;
    }

    char resp[2048];
    int total = 0;
    for (int i = 0; i < 20 && total < (int)sizeof(resp) - 1; i++) {
        int n = (int)read(fd, resp + total, sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = 0;

    printf("test_tcp: read %d bytes total\n", total);
    printf("test_tcp: first 64 bytes of response = [%.64s]\n", resp);

    close(fd);
    printf("test_tcp: close() done\n");

    int ok = (total > 0) && (strncmp(resp, "HTTP/1.", 7) == 0);
    if (ok) {
        printf("PASS: real userspace TCP socket()/connect()/write()/read() works on WynlandOS\n");
        return 0;
    }
    printf("FAIL: response did not look like a real HTTP reply\n");
    return 1;
}
