/*
 * WynlandOS - real userspace DNS verification.
 * Uses musl's real, unmodified getaddrinfo() -- which internally opens
 * a real AF_INET/SOCK_DGRAM socket, reads /etc/resolv.conf for the
 * nameserver, and does a real sendto()/recvfrom() DNS round-trip --
 * to resolve "one.one.one.one" (Cloudflare's own hostname, guaranteed
 * to resolve to 1.1.1.1 or 1.0.0.1), then opens a real TCP connection
 * to the resolved address and confirms a real HTTP response comes back.
 * This is the same path a real, unmodified curl would take.
 *
 * Build:
 *   x86_64-linux-musl-gcc -O2 -o test_dns.elf test_dns.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(void) {
    printf("test_dns: calling getaddrinfo(\"one.one.one.one\")\n");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo("one.one.one.one", "80", &hints, &res);
    if (rc != 0 || !res) {
        printf("FAIL: getaddrinfo() returned %d\n", rc);
        return 1;
    }

    struct sockaddr_in *resolved = (struct sockaddr_in *)res->ai_addr;
    printf("test_dns: resolved to %s\n", inet_ntoa(resolved->sin_addr));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("FAIL: socket() returned %d\n", fd);
        freeaddrinfo(res);
        return 1;
    }

    printf("test_dns: connecting to resolved address\n");
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        printf("FAIL: connect() returned %d\n", rc);
        close(fd);
        return 1;
    }
    printf("test_dns: connect() succeeded\n");

    const char *req = "GET / HTTP/1.0\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
    write(fd, req, (int)strlen(req));

    char resp[512];
    int total = 0;
    for (int i = 0; i < 20 && total < (int)sizeof(resp) - 1; i++) {
        int n = (int)read(fd, resp + total, sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = 0;
    close(fd);

    printf("test_dns: read %d bytes, first 64 = [%.64s]\n", total, resp);

    int ok = (total > 0) && (strncmp(resp, "HTTP/1.", 7) == 0);
    if (ok) {
        printf("PASS: real DNS (getaddrinfo) + real TCP works on WynlandOS\n");
        return 0;
    }
    printf("FAIL: response did not look like a real HTTP reply\n");
    return 1;
}
