/*
 * WynlandOS - dlopen() verification: the loader side.
 * Calls real dlopen()/dlsym()/dlclose() at runtime against
 * /test_dlopen_lib.so -- this is the first-ever test of whether this OS's
 * musl ld.so can do real runtime dynamic loading, as opposed to the
 * boot-time/exec-time DT_NEEDED loading already proven working by Qt6.
 * Needed before pkg-config/CMake/git/curl can be trusted if any of them
 * dlopen() a plugin/module internally.
 * Build:
 *   x86_64-linux-musl-gcc -O2 -rdynamic -o test_dlopen.elf test_dlopen.c -ldl
 */
#include <stdio.h>
#include <dlfcn.h>

int main(void) {
    printf("test_dlopen: calling dlopen(\"/test_dlopen_lib.so\", RTLD_NOW)\n");
    void *handle = dlopen("/test_dlopen_lib.so", RTLD_NOW);
    if (!handle) {
        printf("FAIL: dlopen() returned NULL, dlerror()='%s'\n", dlerror());
        return 1;
    }
    printf("test_dlopen: dlopen() succeeded, handle=%p\n", handle);

    typedef int (*fn_t)(void);
    dlerror(); /* clear any existing error */
    fn_t fn = (fn_t)dlsym(handle, "wynland_dlopen_test_answer");
    const char *err = dlerror();
    if (err) {
        printf("FAIL: dlsym() error: %s\n", err);
        return 1;
    }
    if (!fn) {
        printf("FAIL: dlsym() returned NULL with no error string\n");
        return 1;
    }
    printf("test_dlopen: dlsym() succeeded, fn=%p\n", (void *)fn);

    int result = fn();
    printf("test_dlopen: called fn(), result=%d (expected 42)\n", result);

    if (dlclose(handle) != 0) {
        printf("test_dlopen: dlclose() reported an error: %s (non-fatal)\n", dlerror());
    }

    if (result == 42) {
        printf("PASS: real runtime dlopen()/dlsym() works on WynlandOS\n");
        return 0;
    }
    printf("FAIL: unexpected result value\n");
    return 1;
}
