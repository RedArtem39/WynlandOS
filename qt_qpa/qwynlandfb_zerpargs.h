#ifndef QWYNLANDFB_ZERPARGS_H
#define QWYNLANDFB_ZERPARGS_H

/*
 * Zerp hands every client its c2s/s2c/shm fd numbers positionally via argv
 * (argv[1..3], see zerp.c's spawn_client()) -- the same convention every
 * freestanding Zerp client (zerp_files.elf, zerp_term.elf, ...) already
 * uses via zerp_client.h's zerp_connect(argc, argv, ...).
 *
 * A Qt app can't reuse zerp_connect() directly (it's written against the
 * freestanding zerp_syscalls.h raw-syscall wrappers, not real libc), and
 * the QPA plugin -- not the app -- is what actually needs these fd numbers
 * (inside QPlatformIntegration::initialize()), by which point Qt's own
 * argument parsing may already have touched argv. So: every Zerp-aware Qt
 * app's main() must call zerp_qpa_capture_args(argc, argv) as its FIRST
 * statement, before constructing QApplication -- mirrors zerp_connect()
 * being the first line of every non-Qt Zerp client's main().
 */

extern "C" {
void zerp_qpa_capture_args(int argc, char **argv);
int  zerp_qpa_c2s_fd();
int  zerp_qpa_s2c_fd();
int  zerp_qpa_shm_fd();
}

#endif // QWYNLANDFB_ZERPARGS_H
