/* Diagnostic: which pthread symbols can dlsym() resolve from the global
   scope (what libglvnd's glvndSetupPthreads() does)? Host-built, glibc. */
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(void)
{
    static const char *names[] = {
        "pthread_create", "pthread_join", "pthread_self", "pthread_equal",
        "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock",
        "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_mutexattr_init",
        "pthread_mutexattr_destroy", "pthread_mutexattr_settype",
        "pthread_rwlock_init", "pthread_rwlock_destroy", "pthread_rwlock_rdlock",
        "pthread_rwlock_wrlock", "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
        "pthread_rwlock_unlock", "pthread_once", "pthread_key_create",
        "pthread_key_delete", "pthread_setspecific", "pthread_getspecific",
        "malloc", "printf",
    };
    void *h = dlopen(NULL, RTLD_LAZY);
    fprintf(stderr, "[dlsymtest] dlopen(NULL)=%p\n", h);
    int bad = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *p = dlsym(h, names[i]);
        if (!p) { bad++; fprintf(stderr, "[dlsymtest] MISSING %s (%s)\n", names[i], dlerror()); }
    }
    Dl_info info;
    void *pc = dlsym(h, "pthread_create");
    if (pc && dladdr(pc, &info)) fprintf(stderr, "[dlsymtest] pthread_create from %s\n", info.dli_fname);
    fprintf(stderr, "[dlsymtest] done, %d missing\n", bad);

    /* Big-file read check: libgallium's .dynamic sits ~45 MB into the file.
       Expected bytes: 01 00 .. d7 60 (first DT_NEEDED entry). */
    int fd = open("/lib64/libgallium-26.0.3-1ubuntu1.so", O_RDONLY);
    unsigned char b[16] = {0};
    ssize_t n = pread(fd, b, 16, 0x2b56ce8);
    fprintf(stderr, "[dlsymtest] pread@0x2b56ce8 n=%zd: %02x %02x .. %02x %02x\n", n, b[0], b[1], b[8], b[9]);
    unsigned char *m = mmap(NULL, 0x2000, PROT_READ, MAP_PRIVATE, fd, 0x2b56000);
    if (m != MAP_FAILED)
        fprintf(stderr, "[dlsymtest] mmap@0x2b56000+0xce8: %02x %02x .. %02x %02x\n",
                m[0xce8], m[0xce9], m[0xcf0], m[0xcf1]);
    else
        fprintf(stderr, "[dlsymtest] mmap failed\n");
    unsigned char *w = mmap(NULL, 0x2d4c198, PROT_READ, MAP_PRIVATE, fd, 0);
    if (w != MAP_FAILED)
        fprintf(stderr, "[dlsymtest] whole-file mmap @0x2b56ce8: %02x %02x .. %02x %02x\n",
                w[0x2b56ce8], w[0x2b56ce9], w[0x2b56cf0], w[0x2b56cf1]);
    else
        fprintf(stderr, "[dlsymtest] whole-file mmap failed\n");
    close(fd);
    return 0;
}
