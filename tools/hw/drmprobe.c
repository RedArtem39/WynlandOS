/*
 * WynlandOS - drmprobe: what libdrm reads in sysfs for a DRM node, step by
 * step (drmGetDevice2 with and without DRM_DEVICE_GET_PCI_REVISION, as
 * Mesa's Intel device info asks) -- which file fails, and why.
 * gcc -O2 -I/usr/include/libdrm -o drmprobe drmprobe.c -ldrm
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

static void show(const char *dir, const char *attr)
{
    char path[PATH_MAX], buf[256] = { 0 };
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f) { printf("  %-50s open: %s\n", path, strerror(errno)); return; }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '|';
    printf("  %-50s %zu bytes: %s\n", path, n, buf);
}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror(node); return 1; }
    drmVersionPtr v = drmGetVersion(fd);
    printf("%s: driver %s %d.%d (%s)\n", node, v ? v->name : "?", v ? v->version_major : 0,
           v ? v->version_minor : 0, v ? v->desc : "?");
    if (v) drmFreeVersion(v);
    struct stat st;
    fstat(fd, &st);
    char dev[64], real[PATH_MAX] = { 0 }, sub[PATH_MAX] = { 0 }, s2[PATH_MAX];
    snprintf(dev, sizeof dev, "/sys/dev/char/%u:%u/device", major(st.st_rdev), minor(st.st_rdev));
    printf("node %u:%u\n", major(st.st_rdev), minor(st.st_rdev));
    printf("realpath(%s) = %s\n", dev, realpath(dev, real) ? real : strerror(errno));
    snprintf(s2, sizeof s2, "%s/subsystem", dev);
    printf("realpath(%s) = %s\n", s2, realpath(s2, sub) ? sub : strerror(errno));
    const char *attrs[] = { "revision", "vendor", "device", "subsystem_vendor", "subsystem_device", "uevent", "config" };
    for (int i = 0; i < 7; i++) show(dev, attrs[i]);
    if (real[0]) for (int i = 0; i < 7; i++) show(real, attrs[i]);

    for (int flags = 0; flags < 2; flags++) {
        drmDevicePtr d = NULL;
        int r = drmGetDevice2(fd, flags ? DRM_DEVICE_GET_PCI_REVISION : 0, &d);
        if (r) { printf("drmGetDevice2(flags %d) = %d (%s)\n", flags, r, strerror(-r)); continue; }
        printf("drmGetDevice2(flags %d) = 0: bus %d", flags, d->bustype);
        if (d->bustype == DRM_BUS_PCI)
            printf(", %04x:%04x rev %02x sub %04x:%04x, slot %04x:%02x:%02x.%u", d->deviceinfo.pci->vendor_id,
                   d->deviceinfo.pci->device_id, d->deviceinfo.pci->revision_id, d->deviceinfo.pci->subvendor_id,
                   d->deviceinfo.pci->subdevice_id, d->businfo.pci->domain, d->businfo.pci->bus,
                   d->businfo.pci->dev, d->businfo.pci->func);
        printf("\n");
        drmFreeDevice(&d);
    }
    return 0;
}
