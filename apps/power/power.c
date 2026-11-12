// WynlandOS - reboot, poweroff, halt (one program, by its name): what a
// live stick keeps saved first (apps/persist: persistd, asked over its
// socket, waited for), the disks synced, then the kernel's reboot(2).
// Root's (CAP_SYS_BOOT), or a person's own account at the machine
// (kernel/syscall.c, SYS_reboot).
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// persistd's "save": nothing to do when it is not running (not a live
// stick); at most 3 minutes -- a stick that hangs must not keep the
// machine from turning off
static void save_first(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    static const char name[] = "\0persistd";
    memcpy(a.sun_path, name, sizeof name - 1);
    if (connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof name - 1)) < 0) {
        close(s);
        return;
    }
    struct timeval tv = { 180, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    fprintf(stderr, "saving to the stick...\n");
    if (write(s, "save", 4) == 4) {
        char buf[256];
        ssize_t n = read(s, buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; fprintf(stderr, "persist: %s", buf); }
    }
    close(s);
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    (void)argc;
    int cmd = !strcmp(me, "reboot") ? RB_AUTOBOOT : RB_POWER_OFF;
    save_first();
    sync();
    reboot(cmd);
    fprintf(stderr, "%s: %s\n", me, strerror(errno));
    return 1;
}
