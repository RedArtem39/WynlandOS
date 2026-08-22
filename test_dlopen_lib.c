/*
 * WynlandOS - dlopen() verification: the shared library side.
 * A trivial exported function that test_dlopen.elf loads at RUNTIME via
 * dlopen()/dlsym() -- not via DT_NEEDED at exec time, which is the only
 * kind of dynamic loading this OS has ever had proven working (Qt6's
 * libQt6Core.so etc. are all DT_NEEDED). Real runtime dlopen is a
 * genuinely different code path in musl's ld.so.
 * Build:
 *   x86_64-linux-musl-gcc -shared -fPIC -O2 \
 *     -o test_dlopen_lib.so test_dlopen_lib.c
 */
int wynland_dlopen_test_answer(void) {
    return 42;
}
