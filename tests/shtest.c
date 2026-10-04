/*
 * WynlandOS - shell + coreutils check (glibc, host-built)
 * ============================================================
 * Runs Ubuntu's fish, dash and GNU coreutils on WynlandOS: each case is a
 * script given to `<shell> -c`, its stdout compared with what Linux
 * prints. Then fish once interactively on a PTY: a prompt comes up, a
 * command typed into it runs, Tab completes a command name.
 * One "[shtest] PASS|FAIL" line per check.
 *
 * Build: gcc -O2 -o build/shtest.elf tests/shtest.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;
static void check(int ok, const char *what)
{
    fprintf(stderr, "[shtest] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *const g_env[] = {
    "HOME=/tmp/shhome", "PATH=/usr/local/bin:/usr/bin:/usr/sbin:/sbin", "TERM=xterm-256color",
    "LANG=C.UTF-8", "USER=root", "LD_LIBRARY_PATH=/lib64", NULL
};

/* run argv with stdout+stderr into buf; returns the exit status, -1 if it
   did not run, -2 if it took longer than timeout_ms (then killed) */
static int capture(char *const argv[], char *buf, size_t cap, int timeout_ms)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(p[1], 1); dup2(p[1], 2);
        close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        chdir("/tmp/shhome");
        execve(argv[0], argv, g_env);
        _exit(127);
    }
    close(p[1]);
    size_t len = 0;
    long t0 = now_ms();
    int timed_out = 0;
    for (;;) {
        long left = timeout_ms - (now_ms() - t0);
        if (left <= 0) { timed_out = 1; kill(pid, SIGKILL); break; }
        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        ssize_t n = read(p[0], buf + len, cap - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= cap - 1) break;
    }
    buf[len] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (timed_out) return -2;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

struct sh_case { const char *shell, *name, *script, *want; };

static const struct sh_case CASES[] = {
    /* coreutils through dash (/bin/sh) */
    { "/bin/sh", "sh: echo + /bin symlink", "echo hi; ls -d /bin/ls /usr/bin/ls", "hi\n/bin/ls\n/usr/bin/ls\n" },
    { "/bin/sh", "coreutils: mkdir -p, touch, ls", "mkdir -p a/b/c && touch a/b/c/f a/x && ls a a/b/c", "a:\nb\nx\n\na/b/c:\nf\n" },
    { "/bin/sh", "coreutils: cp -r, mv, rm -r, find",
      "mkdir -p s/d && echo one > s/d/f && cp -r s t && mv t/d/f t/d/g && rm -r s && find t | sort",
      "t\nt/d\nt/d/g\n" },
    { "/bin/sh", "coreutils: cat, wc, sort, uniq, head, tail, tr, cut",
      "printf 'b\\na\\nc\\na\\n' > w; sort w | uniq | tr a-z A-Z | head -2; wc -l < w; tail -1 w; echo x:y:z | cut -d: -f2",
      "A\nB\n4\na\ny\n" },
    { "/bin/sh", "coreutils: stat, chmod, ln -s, readlink, realpath",
      "echo z > f; chmod 640 f; stat -c '%a %s' f; ln -s f l; readlink l; realpath l | sed 's|.*/||'",
      "640 2\nf\nf\n" },
    { "/bin/sh", "coreutils: seq, expr, date, sha256sum, base64",
      "seq 3 | paste -sd+; expr 6 \\* 7; date -d @0 -u +%Y; printf abc | sha256sum | cut -c1-16; printf hi | base64",
      "1+2+3\n42\n1970\nba7816bf8f01cfea\naGk=\n" },
    { "/bin/sh", "coreutils: df, du, dd, truncate",
      "dd if=/dev/zero of=z bs=1024 count=8 2>/dev/null; du -k z | cut -f1; truncate -s 3 z; wc -c < z; df / >/dev/null && echo df",
      "8\n3\ndf\n" },
    { "/bin/sh", "grep, sed, xargs, tar, gzip, xz",
      "mkdir -p q; echo hello > q/h; echo world > q/w; tar czf q.tgz q && rm -r q && tar xzf q.tgz && grep -l o q/* | xargs cat | sed s/o/0/g;"
      " echo abc | xz | xz -d; echo def | gzip | zcat",
      "hell0\nw0rld\nabc\ndef\n" },
    { "/bin/sh", "sh: pipes, subshells, exit codes, here-docs",
      "x=$(echo a | (read v; echo $v$v)); echo $x; false || echo or; cat <<E\nhere $x\nE\n( exit 3 ); echo $?",
      "aa\nor\nhere aa\n3\n" },
    { "/bin/sh", "sh: background jobs + wait, kill, sleep", "sleep 1 & p=$!; wait $p; echo $?; sleep 9 & kill $!; wait $! ; echo $?", "0\nTerminated\n143\n" },
    /* fish */
    { "/usr/bin/fish", "fish: runs", "echo fish $version | string replace -r '[0-9.]+.*' V", "fish V\n" },
    { "/usr/bin/fish", "fish: variables, lists, math, string",
      "set l a b c; echo (count $l) $l[2] (math 6 \\* 7) (string upper wyn)", "3 b 42 WYN\n" },
    { "/usr/bin/fish", "fish: functions, for, if, test",
      "function sq; math $argv[1] \\* $argv[1]; end; for i in 2 3; if test $i -gt 2; sq $i; else; echo small; end; end",
      "small\n9\n" },
    { "/usr/bin/fish", "fish: pipes, command substitution, status",
      "echo (seq 3 | string join ,); false; echo $status; ls /usr/bin/ls | wc -l", "1,2,3\n1\n1\n" },
    { "/usr/bin/fish", "fish: completions for ls", "complete -C 'ls --colo' | head -1 | string split \\t -f1", "--color\n" },
    { "/usr/bin/fish", "fish: background job + wait", "sleep 1 &; wait; echo done", "done\n" },
    { "/usr/bin/fish", "fish: universal variables (~/.config/fish)", "set -U wyn 1; and echo ok", "ok\n" },
    /* Python 3 */
    { "/usr/bin/python3", "python3: runs, json, os",
      "import sys, json, os; print(sys.version_info[:2], json.dumps({'a': [1, 2]}), os.getcwd())",
      "(3, 14) {\"a\": [1, 2]} /tmp/shhome\n" },
    { "/usr/bin/python3", "python3: pathlib, sqlite3, zlib, hashlib",
      "import pathlib, sqlite3, zlib, hashlib\n"
      "p = pathlib.Path('py'); p.mkdir(exist_ok=True); (p / 't.txt').write_text('hi')\n"
      "db = sqlite3.connect('py/d.db'); db.execute('create table t(x)'); db.execute('insert into t values (42)'); db.commit()\n"
      "print(db.execute('select x from t').fetchone()[0], zlib.decompress(zlib.compress(b'ok')).decode(),\n"
      "      hashlib.sha256(b'abc').hexdigest()[:8], sorted(x.name for x in p.iterdir()))",
      "42 ok ba7816bf ['d.db', 't.txt']\n" },
    { "/usr/bin/python3", "python3: subprocess, threads, asyncio",
      "import subprocess, threading, asyncio\n"
      "r = subprocess.run(['ls', '/bin/ls'], capture_output=True, text=True).stdout.strip()\n"
      "out = []; t = threading.Thread(target=lambda: out.append(7)); t.start(); t.join()\n"
      "async def f():\n    await asyncio.sleep(0.05); return 'aio'\n"
      "print(r, out[0], asyncio.run(f()))",
      "/bin/ls 7 aio\n" },
    { "/usr/bin/python3", "python3: multiprocessing pool",
      "import multiprocessing as mp\n"
      "if __name__ == '__main__':\n    with mp.Pool(2) as p: print(p.map(abs, [-1, -4, 9]))",
      "[1, 4, 9]\n" },
    { "/usr/bin/python3", "python3: https (ssl, CA bundle)",
      "import urllib.request; print(urllib.request.urlopen('https://example.com', timeout=30).status)", "200\n" },
    /* leaf, the package manager: Ubuntu's archive, signed index, real installs */
    { "/bin/sh", "ary: superuser, root for one command",
      "printf 'wyn\\nwyn\\n' | ary -S login >/dev/null; echo wrong | ary -S id -u 2>&1; echo wyn | ary -S id -u; id -u",
      "ary: incorrect password\n0\n1000\n" },
    { "/bin/sh", "leaf: update (InRelease checked by gpgv)",
      "echo wyn | ary -S leaf update > /tmp/leaf-update.log 2>&1; echo $?; ls /var/lib/leaf/lists | wc -l", "0\n6\n" },
    { "/bin/sh", "leaf: install tree + jq from Ubuntu",
      "echo wyn | ary -S leaf install tree jq > /tmp/leaf-install.log 2>&1; echo $?; tree -d /etc/leaf | tail -1; echo '{\"a\":[1,2]}' | jq -c '.a|add'",
      "0\n0 directories\n3\n" },
    { "/bin/sh", "leaf: remove",
      "echo wyn | ary -S leaf remove tree > /dev/null 2>&1; command -v tree || echo gone; leaf list | cut -d' ' -f1 | tr '\\n' ' '",
      "gone\njq libjq1 libonig5 " },
    { "/bin/sh", "python3: venv, pip",
      "python3 -m venv --without-pip ve && ve/bin/python -c 'import sys; print(sys.prefix)' && python3 -m pip --version | cut -c1-4",
      "/tmp/shhome/ve\npip \n" },
};

static void run_cases(void)
{
    static char out[16384];
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const struct sh_case *c = &CASES[i];
        char *argv[] = { (char *)c->shell, "-c", (char *)c->script, NULL };
        long t0 = now_ms();
        int st = capture(argv, out, sizeof out, 300000);
        long ms = now_ms() - t0;
        int ok = st == 0 && strcmp(out, c->want) == 0;
        char what[160];
        snprintf(what, sizeof what, "%s (%ld ms)", c->name, ms);
        check(ok, what);
        if (!ok) fprintf(stderr, "[shtest]   status %d, got:\n%s\n[shtest]   want:\n%s", st, out, c->want);
    }
}

/* ---------------------------------------------------------------- PTY */

/* answer the queries a terminal answers (fish waits for the Primary
   Device Attributes reply): what was not looked at yet starts at *seen */
static void pty_answer(int m, const char *buf, size_t len, size_t *seen)
{
    for (size_t i = *seen; i + 2 < len; i++) {
        if (buf[i] != 0x1b || buf[i + 1] != '[') continue;
        const char *q = buf + i + 2;
        if (!strncmp(q, "0c", 2) || q[0] == 'c') write(m, "\x1b[?62;22c", 9);     /* DA1: a VT220 */
        else if (!strncmp(q, "6n", 2)) write(m, "\x1b[1;1R", 6);                   /* cursor position */
    }
    *seen = len > 3 ? len - 3 : 0;
}

static int pty_read_until(int m, char *buf, size_t cap, size_t *len, const char *needle, int timeout_ms)
{
    static size_t seen;
    long t0 = now_ms();
    while (!strstr(buf, needle)) {
        long left = timeout_ms - (now_ms() - t0);
        if (left <= 0) return 0;
        struct pollfd pf = { m, POLLIN, 0 };
        if (poll(&pf, 1, (int)left) <= 0) continue;
        ssize_t n = read(m, buf + *len, cap - 1 - *len);
        if (n <= 0) return 0;
        /* drop NULs so strstr sees everything */
        for (ssize_t k = 0; k < n; k++) if (!buf[*len + k]) buf[*len + k] = ' ';
        *len += (size_t)n;
        buf[*len] = 0;
        pty_answer(m, buf, *len, &seen);
        if (*len >= cap - 1) return 0;
    }
    return 1;
}

static void dump_tail(const char *buf, size_t len)
{
    size_t from = len > 600 ? len - 600 : 0;
    fprintf(stderr, "[shtest]   pty output (tail): ");
    for (size_t i = from; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 27) fputs("\\e", stderr);
        else if (ch == '\r') fputs("\\r", stderr);
        else if (ch == '\n') fputs("\\n", stderr);
        else if (ch < 32 || ch > 126) fprintf(stderr, "\\x%02x", ch);
        else fputc(ch, stderr);
    }
    fputc('\n', stderr);
}

static void run_pty(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) { check(0, "pty: posix_openpt"); return; }
    char *sname = ptsname(m);
    struct winsize ws = { 30, 100, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(sname, O_RDWR);
        if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        chdir("/tmp/shhome");
        char *argv[] = { "/usr/bin/fish", "-i", NULL };
        execve(argv[0], argv, g_env);
        _exit(127);
    }
    static char buf[65536];
    size_t len = 0;
    buf[0] = 0;
    long t0 = now_ms();
    int up = pty_read_until(m, buf, sizeof buf, &len, "# ", 30000);   /* root@wynland ~# */
    char what[96];
    snprintf(what, sizeof what, "fish -i: prompt on a PTY (%ld ms)", now_ms() - t0);
    check(up, what);
    if (!up) { dump_tail(buf, len); kill(pid, SIGKILL); waitpid(pid, NULL, 0); close(m); return; }

    const char *cmd = "echo wyn(math 40 + 2)\r";
    write(m, cmd, strlen(cmd));
    int ran = pty_read_until(m, buf, sizeof buf, &len, "wyn42", 10000);
    check(ran, "fish -i: a typed command runs");
    if (!ran) dump_tail(buf, len);

    /* Python's REPL (3.14's own line editor) inside it, and back */
    write(m, "python3\r", 8);
    int py = pty_read_until(m, buf, sizeof buf, &len, ">>> ", 15000);
    if (py) {
        write(m, "6 * 7 + 1000\r", 13);
        py = pty_read_until(m, buf, sizeof buf, &len, "1042", 10000);
        write(m, "exit()\r", 7);
        usleep(1000000);   /* back in fish (the REPL colours what it echoes: no plain "exit()" to wait for) */
    }
    check(py, "python3 REPL on the PTY");
    if (!py) dump_tail(buf, len);

    /* "sha256s" + Tab completes to sha256sum */
    write(m, "sha256s\t", 8);
    int comp = pty_read_until(m, buf, sizeof buf, &len, "sha256sum", 10000);
    check(comp, "fish -i: Tab completes a command");
    if (!comp) dump_tail(buf, len);
    write(m, "\x15" "exit\r", 6);   /* ^U: clear the line ("\x15e" would be one hex escape) */

    long t1 = now_ms();
    int st = -1;
    while (now_ms() - t1 < 10000) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        struct pollfd pf = { m, POLLIN, 0 };
        if (poll(&pf, 1, 100) > 0 && len < sizeof buf - 1) {
            ssize_t n = read(m, buf + len, sizeof buf - 1 - len);
            if (n > 0) { len += (size_t)n; buf[len] = 0; }
            else usleep(10000);
        }
        st = -1;
    }
    check(st == 0, "fish -i: exit");
    if (st != 0) dump_tail(buf, len);
    if (st != 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    close(m);
}

int main(void)
{
    mkdir("/tmp/shhome", 0755);
    run_cases();
    run_pty();
    fprintf(stderr, "[shtest] %d passed, %d failed\n", g_pass, g_fail);
    printf("[shtest] DONE %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
