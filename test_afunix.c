/*
 * WynlandOS - AF_UNIX / SCM_RIGHTS / memfd end-to-end test.
 *
 * A plain glibc program (built by the host gcc, dynamically linked like the
 * /lib64 binaries in ext2_manifest.txt). Run it from the shell:
 *     exec /test_afunix.elf
 * Every check prints one "AFUNIX-TEST: PASS|FAIL <name>" line to stdout
 * (serial log), then a final "AFUNIX-TEST: DONE pass=N fail=M".
 *
 * Children are never waited for (this kernel has no wait4 yet); every
 * hand-off is synchronized over the sockets themselves.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <unistd.h>

static int g_pass, g_fail;

static void out(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(1, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void check(int ok, const char *name) {
    if (ok) g_pass++; else g_fail++;
    out("AFUNIX-TEST: %s %s (errno=%d)\n", ok ? "PASS" : "FAIL", name, ok ? 0 : errno);
}

static int send_fd(int sock, int fd, const char *data, size_t len) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct iovec iov = { (void *)data, len };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return (int)sendmsg(sock, &mh, 0);
}

static int recv_fd(int sock, char *data, size_t len, int *fd_out) {
    char cbuf[CMSG_SPACE(4 * sizeof(int))];
    struct iovec iov = { data, len };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    int n = (int)recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    *fd_out = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    }
    return n;
}

/* ---- 1: seqpacket + SCM_RIGHTS + shared memfd across fork ---- */
static void test_seqpacket_memfd(void) {
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0, "socketpair(SEQPACKET)");

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int mfd = memfd_create("afunix-test", MFD_CLOEXEC);
        if (mfd < 0 || ftruncate(mfd, 8192) != 0) _exit(1);
        char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (p == MAP_FAILED) _exit(2);
        strcpy(p, "hello from child");
        send_fd(sv[1], mfd, "M", 1);
        close(mfd);                      /* the mapping + the in-flight fd keep it alive */
        char ack[8];
        if (read(sv[1], ack, sizeof(ack)) != 3) _exit(3);
        /* the parent wrote into ITS mapping; we must see it in ours */
        const char *verdict = strcmp(p + 4096, "parent-was-here") == 0 ? "shared-ok" : "shared-bad";
        write(sv[1], verdict, strlen(verdict));
        write(sv[1], "one", 3);
        write(sv[1], "two", 3);
        _exit(0);                        /* parent then sees EOF */
    }
    check(pid > 0, "fork() via clone");
    close(sv[1]);

    struct pollfd pf = { sv[0], POLLIN, 0 };
    check(poll(&pf, 1, 5000) == 1 && (pf.revents & POLLIN), "poll() readable");

    char b[32]; int fd = -1;
    int n = recv_fd(sv[0], b, sizeof(b), &fd);
    check(n == 1 && b[0] == 'M' && fd >= 0, "recvmsg() got SCM_RIGHTS fd");
    check(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "MSG_CMSG_CLOEXEC honoured");

    struct stat st;
    check(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 8192, "fstat(memfd) size");
    char *p = fd >= 0 ? mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    check(p != MAP_FAILED && strcmp(p, "hello from child") == 0, "mmap(MAP_SHARED) sees child's data");
    if (p != MAP_FAILED) strcpy(p + 4096, "parent-was-here");
    write(sv[0], "ack", 3);

    memset(b, 0, sizeof(b));
    n = (int)read(sv[0], b, sizeof(b));
    check(n == 9 && memcmp(b, "shared-ok", 9) == 0, "child sees parent's write (shared pages)");

    memset(b, 0, sizeof(b));
    int n1 = (int)read(sv[0], b, sizeof(b));
    int ok1 = n1 == 3 && memcmp(b, "one", 3) == 0;
    memset(b, 0, sizeof(b));
    int n2 = (int)read(sv[0], b, sizeof(b));
    check(ok1 && n2 == 3 && memcmp(b, "two", 3) == 0, "SEQPACKET keeps message boundaries");

    check(read(sv[0], b, sizeof(b)) == 0, "EOF after peer exit");
    pf.revents = 0;
    check(poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP), "POLLHUP after peer exit");
    close(sv[0]);
    if (fd >= 0) close(fd);
}

/* ---- 2: abstract-name stream listen/connect/accept ---- */
static void test_listen_accept(void) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    const char name[] = "\0wynland-afunix-test";
    memcpy(a.sun_path, name, sizeof(name) - 1);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(name) - 1);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    check(ls >= 0 && bind(ls, (struct sockaddr *)&a, alen) == 0 && listen(ls, 4) == 0, "bind+listen abstract");
    int dup_ls = socket(AF_UNIX, SOCK_STREAM, 0);
    check(bind(dup_ls, (struct sockaddr *)&a, alen) < 0 && errno == EADDRINUSE, "bind twice -> EADDRINUSE");
    close(dup_ls);

    struct sockaddr_un missing = a;
    missing.sun_path[1] = 'X';
    int cs0 = socket(AF_UNIX, SOCK_STREAM, 0);
    check(connect(cs0, (struct sockaddr *)&missing, alen) < 0 && errno == ECONNREFUSED, "connect missing -> ECONNREFUSED");
    close(cs0);

    pid_t pid = fork();
    if (pid == 0) {
        close(ls);
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, alen) != 0) _exit(1);
        write(c, "ping", 4);
        char r[8] = {0};
        read(c, r, 4);
        write(c, memcmp(r, "pong", 4) == 0 ? "Y" : "N", 1);
        _exit(0);
    }

    int s = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    check(s >= 0, "accept4()");
    struct ucred cr; socklen_t crl = sizeof(cr);
    check(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0 && cr.pid == pid, "SO_PEERCRED pid");
    char b[8] = {0};
    check(read(s, b, 4) == 4 && memcmp(b, "ping", 4) == 0, "stream read ping");
    write(s, "pong", 4);
    check(read(s, b, 1) == 1 && b[0] == 'Y', "client got pong");
    check(read(s, b, 1) == 0, "stream EOF after client exit");
    int sig_ok = 1;
    signal(SIGPIPE, SIG_IGN);
    sig_ok = send(s, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE;
    check(sig_ok, "send after peer gone -> EPIPE");
    close(s);
    close(ls);
}

/* ---- 3: refcounting across dup(): EOF only after the LAST close ---- */
static void test_dup_lifetime(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int d = dup(sv[0]);
    close(sv[0]);
    check(write(sv[1], "z", 1) == 1, "peer alive while a dup remains");
    char c;
    check(read(d, &c, 1) == 1 && c == 'z', "read through the dup");
    close(d);
    check(send(sv[1], "z", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "EPIPE after last dup closed");
    close(sv[1]);

    int nb[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, nb);
    check(read(nb[0], &c, 1) < 0 && errno == EAGAIN, "SOCK_NONBLOCK read -> EAGAIN");
    close(nb[0]); close(nb[1]);
}

/* ---- 4: small syscalls ---- */
static void test_misc(void) {
    struct utsname u;
    check(uname(&u) == 0 && strcmp(u.sysname, "Linux") == 0, "uname()");
    struct timeval tv;
    check(syscall(SYS_gettimeofday, &tv, NULL) == 0 && tv.tv_sec > 1600000000, "gettimeofday()");
    stack_t ss;
    check(sigaltstack(NULL, &ss) == 0, "sigaltstack()");
    /* (fds 0-2 aren't real table entries on this kernel's console, so dup
       a real one) */
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    int d3 = dup3(sp[0], 50, O_CLOEXEC);
    check(d3 == 50 && (fcntl(50, F_GETFD) & FD_CLOEXEC), "dup3(O_CLOEXEC)");
    int dc = fcntl(sp[0], F_DUPFD_CLOEXEC, 60);
    check(dc >= 60 && (fcntl(dc, F_GETFD) & FD_CLOEXEC), "F_DUPFD_CLOEXEC");
    close(sp[0]); close(50);
    check(write(sp[1], "k", 1) == 1, "socket alive via F_DUPFD copy");
    char kc;
    check(dc >= 0 && read(dc, &kc, 1) == 1 && kc == 'k', "read through F_DUPFD copy");
    if (dc >= 0) close(dc);
    close(sp[1]);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 0 }, { 0, 30 * 1000000 } };
    check(tfd >= 0 && timerfd_settime(tfd, 0, &its, NULL) == 0, "timerfd_create/settime");
    struct pollfd pf = { tfd, POLLIN, 0 };
    int pr = poll(&pf, 1, 2000);
    uint64_t cnt = 0;
    check(pr == 1 && read(tfd, &cnt, 8) == 8 && cnt >= 1, "timerfd fires via poll()");
    close(tfd);

    /* a big realloc exercises mremap(MREMAP_MAYMOVE) on glibc's mmap'd chunk */
    char *m = malloc(1 << 20);
    if (m) { m[0] = 'q'; m = realloc(m, 4 << 20); }
    check(m && m[0] == 'q', "realloc() of an mmap'd chunk");
    free(m);
}

int main(void) {
    out("AFUNIX-TEST: start pid=%d\n", (int)getpid());
    test_seqpacket_memfd();
    test_listen_accept();
    test_dup_lifetime();
    test_misc();
    out("AFUNIX-TEST: DONE pass=%d fail=%d\n", g_pass, g_fail);
    return 0;
}
