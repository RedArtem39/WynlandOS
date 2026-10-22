// WynlandOS - reboot, poweroff, halt (one program, by its name): the
// disks synced, then the kernel's reboot(2). Root's (CAP_SYS_BOOT), or a
// person's own account at the machine (kernel/syscall.c, SYS_reboot).
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    (void)argc;
    int cmd = !strcmp(me, "reboot") ? RB_AUTOBOOT : RB_POWER_OFF;
    sync();
    reboot(cmd);
    fprintf(stderr, "%s: %s\n", me, strerror(errno));
    return 1;
}
