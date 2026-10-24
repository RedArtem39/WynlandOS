/*
 * WynlandOS - envexec: stands in for a program with extra environment
 * variables (test-wyn's desktop test: Zerp 2 with Mesa's INTEL_DEBUG=...).
 * Our spawn runs ELF files only, not "#!" scripts: so a binary.
 * Installed as /usr/bin/zerp2, the real one renamed /usr/bin/zerp2.real;
 * the variables a line each in /tmp/envexec.env.
 * gcc -O2 -static -o envexec envexec.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    FILE *f = fopen("/tmp/envexec.env", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strchr(line, '=')) putenv(strdup(line));
    }
    if (f) fclose(f);
    char real[512];
    snprintf(real, sizeof real, "%s.real", argv[0][0] == '/' ? argv[0] : "/usr/bin/zerp2");
    execv(real, argv);
    perror(real);
    return 127;
}
