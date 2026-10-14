// WynlandOS - hyprctl: Hyprland's command line, speaking to Zerp 2.0's
// Hyprland IPC (apps/zerp2/hypripc.cpp).
//   hyprctl [-j] COMMAND [ARGS...]        e.g. hyprctl -j clients
//   hyprctl dispatch workspace 2
//   hyprctl --batch "dispatch workspace 2 ; j/clients"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int json = 0, batch = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-j")) json = 1;
        else if (!strcmp(argv[i], "--batch")) batch = 1;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) i++;   /* one instance here */
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-q")) continue;
        else { fprintf(stderr, "hyprctl: unknown option %s\n", argv[i]); return 1; }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: hyprctl [-j] COMMAND [ARGS...] | hyprctl --batch \"CMD ; CMD\"\n");
        return 1;
    }
    if (!sig || !rt) { fprintf(stderr, "HYPRLAND_INSTANCE_SIGNATURE not set: is Zerp 2.0 running?\n"); return 1; }

    char req[8192];
    size_t n = 0;
    req[0] = 0;
    if (batch) n += (size_t)snprintf(req + n, sizeof req - n, "[[BATCH]]");
    else if (json) n += (size_t)snprintf(req + n, sizeof req - n, "j/");
    for (int k = i; k < argc && n < sizeof req; k++)
        n += (size_t)snprintf(req + n, sizeof req - n, "%s%s", k > i ? " " : "", argv[k]);
    if (n >= sizeof req) { fprintf(stderr, "hyprctl: request too long\n"); return 1; }

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/.socket.sock", rt, sig);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("hyprctl: connect"); return 1; }
    if (write(fd, req, n) != (ssize_t)n) { perror("hyprctl: write"); return 1; }
    char buf[65536];
    ssize_t r;
    int last_nl = 1;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, (size_t)r, stdout);
        last_nl = buf[r - 1] == '\n';
    }
    if (!last_nl) putchar('\n');
    close(fd);
    return 0;
}
