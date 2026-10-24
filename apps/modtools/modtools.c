// WynlandOS - insmod / rmmod / lsmod (one binary, picked by the name it
// runs under): kernel modules (.ko) into the running kernel and out
// (kernel/module.c). Root's, as Linux's.
//
//   insmod FILE.ko [NAME]   load it (its name: NAME, else the file's)
//   rmmod NAME              its cleanup_module(), then gone
//   lsmod                   the loaded ones: name, where, size

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_LIST_MODULES 420                          // ours

static const char *errname(long r)
{
    switch (-r) {
    case 1: return "not allowed (root only)";
    case 2: return "no such module";
    case 8: return "not a module (an x86-64 relocatable ELF), or it needs what the kernel lacks -- see dmesg";
    case 12: return "out of memory";
    case 17: return "already loaded";
    case 27: return "too big";
    default: return strerror((int)-r);
    }
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(me, "lsmod")) {
        char buf[4096];
        long n = syscall(SYS_LIST_MODULES, buf, sizeof buf);
        if (n < 0) { fprintf(stderr, "lsmod: %s\n", errname(n)); return 1; }
        printf("Module                          Address             Size\n");
        char *line = buf, *end = buf + n;
        while (line < end) {
            char *nl = memchr(line, '\n', (size_t)(end - line));
            if (!nl) break;
            *nl = 0;
            char name[64], at[32], size[32];
            if (sscanf(line, "%63s %31s %31s", name, at, size) == 3)
                printf("%-31s %-19s %s\n", name, at, size);
            line = nl + 1;
        }
        return 0;
    }
    if (!strcmp(me, "rmmod")) {
        if (argc < 2) { fprintf(stderr, "usage: rmmod NAME\n"); return 2; }
        long r = syscall(176, argv[1], 0);
        if (r < 0) { fprintf(stderr, "rmmod: %s: %s\n", argv[1], errname(r)); return 1; }
        return 0;
    }
    if (argc < 2) { fprintf(stderr, "usage: insmod FILE.ko [NAME]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *img = malloc(len > 0 ? (size_t)len : 1);
    if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);
    char name[32];
    if (argc > 2) snprintf(name, sizeof name, "%s", argv[2]);
    else {
        const char *b = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
        snprintf(name, sizeof name, "%s", b);
        char *dot = strstr(name, ".ko");
        if (dot) *dot = 0;
    }
    long r = syscall(175, img, len, name);
    if (r < 0) { fprintf(stderr, "insmod: %s: %s\n", argv[1], errname(r)); return 1; }
    printf("insmod: %s loaded\n", name);
    return 0;
}
