/*
 * WynlandOS / Zerp - shared freestanding process-entry trampoline.
 * Real ELF entry gets argc/argv on the initial stack per the SysV ABI
 * (there's no musl crt0 here to parse that into a normal main() call,
 * same reason spawner.c/shm_test_child.c don't use one). Every Zerp
 * binary (compositor, demos) defines `int zerp_main(int argc, char
 * **argv)` and includes this header exactly once, at the bottom of its
 * one-file translation unit, to get a working _start for free.
 */
#ifndef ZERP_ENTRY_H
#define ZERP_ENTRY_H

int zerp_main(int argc, char **argv);

/* the environment: after argv's NULL on the initial stack */
char **zerp_envp;

void real_start(long argc, char **argv) {
    zerp_envp = argv + argc + 1;
    int code = zerp_main((int)argc, argv);
    zexit(code);
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "    movq (%rsp), %rdi\n"
    "    leaq 8(%rsp), %rsi\n"
    "    andq $-16, %rsp\n"
    "    call real_start\n"
);

#endif /* ZERP_ENTRY_H */
