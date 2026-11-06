/*
 * WynlandOS - i915stress: the Intel GPU from several programs and threads
 * at once, the kernel's fast path (drivers/gpu/i915_uapi.c, outside the
 * big kernel lock) on group by group -- and the USB watched meanwhile.
 *
 * With the fast path on, Zerp and the shell on the GPU at once, the
 * laptop's whole USB died (mouse, the stick). Each phase here turns one
 * group on (/proc/sysrq-trigger "G<mask>"), runs WORKERS processes of two
 * GL threads each (their own contexts: draws checked by their pixels,
 * textures made and dropped -- GEM objects created and closed), while a
 * thread reads the stick (/dev/usb0) four times a second. A read that
 * does not come back within 3 s: the USB is gone -- the fast path off
 * again at once, the phase named on the screen (the network, a phone's
 * USB tethering, may be gone with it: the screen is the report then).
 *
 *   i915stress [SECONDS_A_PHASE [WORKERS [PHASES]]]
 *   PHASES: masks, comma-separated (default 0,16,8,4,2,1,31):
 *     0 off  1 execbuffer  2 objects  4 waits  8 syncobjs  16 parameters  31 all
 *     c<mask>: a new texture every frame, read back whole -- blocks left
 *     undrawn are the laptop's "locust" on the widgets
 *
 * Built on the developer's machine (glibc, the image's Mesa ABI):
 *   gcc -O2 -pthread -o i915stress i915stress.c -lEGL -lGLESv2 -lgbm
 * Root's (the switch and /dev/usb0): test-wyn runs it so.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define W 128
#define H 128

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *group_name(int m)
{
    switch (m) {
    case 0: return "off (all under the big lock)";
    case 1: return "execbuffer";
    case 2: return "objects (create/close/mmap offset/madvise)";
    case 4: return "waits (wait/busy/set domain/throttle)";
    case 8: return "syncobjs";
    case 16: return "parameters (getparam/tiling/caching/reg read)";
    case 31: return "ALL";
    default: return "a mix";
    }
}

static int set_fast(int mask)
{
    int fd = open("/proc/sysrq-trigger", O_WRONLY);
    if (fd < 0) { printf("[i915stress] cannot open /proc/sysrq-trigger (%s): run as root\n", strerror(errno)); return -1; }
    char b[16];
    int n = mask ? snprintf(b, sizeof b, "G%d", mask) : snprintf(b, sizeof b, "g");
    int r = (int)write(fd, b, (size_t)n);
    close(fd);
    return r == n ? 0 : -1;
}

/* ---- a worker: one process, two GL threads ------------------------------ */

struct shared {                     /* in a MAP_SHARED page: the parent reads it */
    volatile long frames[64][2];
    volatile long bad[64][2];
    volatile long objs[64][2];
    volatile int  dead[64][2];      /* a GL step failed */
};
static struct shared *g_sh;
static volatile int g_stop;
static int g_churn;                 /* this phase: a new texture every frame */
static EGLDisplay g_dpy;
static int g_worker;

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static void *gl_thread(void *arg)
{
    int t = (int)(long)arg, w = g_worker;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        g_sh->dead[w][t] = 1;
        return NULL;
    }
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { g_sh->dead[w][t] = 2; return NULL; }
    glViewport(0, 0, W, H);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }"));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    GLint uc = glGetUniformLocation(prog, "c");
    static const GLfloat tri[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);

    unsigned char *px = malloc(W * H * 4), *junk = malloc(256 * 256 * 4);
    memset(junk, 0x5A, 256 * 256 * 4);
    unsigned seed = (unsigned)(w * 977 + t * 131 + 7);
    for (long f = 0; !g_stop; f++) {
        if (g_churn) {
            /* "locust" on the laptop's widgets: whole pages of a buffer
               left transparent -- the GPU's writes went elsewhere (a stale
               GPU address mapping?). A new texture each frame, of a size
               that changes (its GPU addresses reused), cleared all over,
               read back: every pixel of it must be drawn. */
            int tw = 64 + (int)((f * 37 + t * 11 + w * 5) % 9) * 32, th = 64 + (int)((f * 53 + w) % 7) * 32;
            GLuint ct, cf;
            glGenTextures(1, &ct);
            glBindTexture(GL_TEXTURE_2D, ct);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glGenFramebuffers(1, &cf);
            glBindFramebuffer(GL_FRAMEBUFFER, cf);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, 0);
            glViewport(0, 0, tw, th);
            glClearColor(0.0f, 0.5f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            unsigned char *cp = calloc((size_t)tw * th, 4);
            glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, cp);
            long holes = 0;
            for (int i = 0; i < tw * th; i++)
                if (cp[i * 4 + 3] < 250 || cp[i * 4 + 2] < 250) holes++;
            if (holes) {
                g_sh->bad[w][t]++;
                if (g_sh->bad[w][t] <= 3)
                    printf("[i915stress]   program %d thread %d frame %ld: %ld of %d pixels NOT DRAWN in a %dx%d texture (%.0f%%)\n",
                           w, t, f, holes, tw * th, tw, th, 100.0 * holes / (tw * th));
            }
            free(cp);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glDeleteFramebuffers(1, &cf);
            glDeleteTextures(1, &ct);
            glBindTexture(GL_TEXTURE_2D, tex);
            glViewport(0, 0, W, H);
            if (glGetError() != GL_NO_ERROR) { g_sh->dead[w][t] = 4; break; }
            g_sh->objs[w][t]++;
            g_sh->frames[w][t]++;
            continue;
        }
        /* a clear and a triangle of colours of this frame's own */
        seed = seed * 1103515245u + 12345u;
        unsigned char bg = (unsigned char)(seed >> 16), fg = (unsigned char)(seed >> 8);
        glClearColor(bg / 255.0f, 0.25f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(uc, fg / 255.0f, 0.75f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        unsigned char *in = &px[(4 * W + 4) * 4], *out = &px[((H - 5) * W + (W - 5)) * 4];
        if (abs(in[0] - fg) > 2 || abs(in[1] - 191) > 2 || abs(out[0] - bg) > 2 || abs(out[2] - 128) > 2)
            g_sh->bad[w][t]++;
        if (glGetError() != GL_NO_ERROR) { g_sh->dead[w][t] = 3; break; }
        /* GEM objects made and dropped (what windows coming and going do) */
        if (f % 4 == 0) {
            GLuint x;
            glGenTextures(1, &x);
            glBindTexture(GL_TEXTURE_2D, x);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, junk);
            glDeleteTextures(1, &x);
            glBindTexture(GL_TEXTURE_2D, tex);
            g_sh->objs[w][t]++;
        }
        g_sh->frames[w][t]++;
    }
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, ctx);
    free(px); free(junk);
    return NULL;
}

static void on_term(int s) { (void)s; g_stop = 1; }

static int worker(int w)
{
    g_worker = w;
    signal(SIGTERM, on_term);
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { g_sh->dead[w][0] = g_sh->dead[w][1] = 10; return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_dpy = gbm && get_dpy ? get_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    EGLint maj, min;
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &maj, &min)) { g_sh->dead[w][0] = g_sh->dead[w][1] = 11; return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    pthread_t th[2];
    for (long t = 0; t < 2; t++) pthread_create(&th[t], NULL, gl_thread, (void *)t);
    for (int t = 0; t < 2; t++) pthread_join(th[t], NULL);
    eglTerminate(g_dpy);
    if (gbm) gbm_device_destroy(gbm);
    close(fd);
    return 0;
}

/* ---- the USB watch -------------------------------------------------------- */

static volatile double g_usb_asked;   /* a read under way since (0: none) */
static volatile double g_usb_worst;
static volatile long g_usb_reads, g_usb_errs;

static void *usb_watch(void *arg)
{
    (void)arg;
    int fd = open("/dev/usb0", O_RDONLY);
    if (fd < 0) { printf("[i915stress] no /dev/usb0 (%s): the USB is not watched\n", strerror(errno)); return NULL; }
    static char buf[65536];
    for (long i = 0;; i++) {
        off_t at = (off_t)((i * 7919) % 4096) * 65536;   /* here and there in the first 256 MB */
        double t0 = now_s();
        g_usb_asked = t0;
        ssize_t r = pread(fd, buf, sizeof buf, at);
        double d = now_s() - t0;
        g_usb_asked = 0;
        if (r != (ssize_t)sizeof buf) g_usb_errs++;
        else g_usb_reads++;
        if (d > g_usb_worst) g_usb_worst = d;
        usleep(250 * 1000);
    }
    return NULL;
}

/* ---- the phases ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    int workers = argc > 2 ? atoi(argv[2]) : 3;
    const char *phases = argc > 3 ? argv[3] : "0,16,8,4,2,1,31";
    if (secs < 3) secs = 3;
    if (workers < 1) workers = 1;
    if (workers > 64) workers = 64;
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* a memfd, not MAP_SHARED|MAP_ANONYMOUS: on WynlandOS that one is
       still copied at fork() (the children's counts never came back) */
    int mfd = memfd_create("i915stress", 0);
    if (mfd < 0 || ftruncate(mfd, sizeof *g_sh) < 0) { printf("[i915stress] memfd failed\n"); return 1; }
    g_sh = mmap(NULL, sizeof *g_sh, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (g_sh == MAP_FAILED) { printf("[i915stress] mmap failed\n"); return 1; }
    printf("[i915stress] %d s a phase, %d programs x 2 GL threads, phases %s\n", secs, workers, phases);
    if (set_fast(0) < 0) return 1;
    pthread_t uw;
    pthread_create(&uw, NULL, usb_watch, NULL);

    int failed = 0;
    char list[256];
    snprintf(list, sizeof list, "%s", phases);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        g_churn = tok[0] == 'c';                     /* "c<mask>": the texture-churn phase */
        int mask = atoi(tok + g_churn);
        memset((void *)g_sh, 0, sizeof *g_sh);
        g_usb_worst = 0; g_usb_reads = g_usb_errs = 0;
        printf("\n[i915stress] === phase: fast path %d -- %s%s\n", mask, group_name(mask),
               g_churn ? "; a new texture every frame, each checked whole" : "");
        if (set_fast(mask) < 0) { failed = 1; break; }
        pid_t pid[64];
        for (int w = 0; w < workers; w++) {
            pid[w] = fork();
            if (pid[w] == 0) _exit(worker(w));
        }
        double t0 = now_s(), last = t0;
        int usb_dead = 0;
        while (now_s() - t0 < secs) {
            usleep(200 * 1000);
            double a = g_usb_asked;
            if (a && now_s() - a > 3.0) { usb_dead = 1; break; }
            if (now_s() - last >= 5) {
                last = now_s();
                long fr = 0, bad = 0;
                for (int w = 0; w < workers; w++) for (int t = 0; t < 2; t++) { fr += g_sh->frames[w][t]; bad += g_sh->bad[w][t]; }
                printf("[i915stress]   %3.0f s: %ld frames, %ld wrong, USB reads %ld (worst %.0f ms)\n",
                       now_s() - t0, fr, bad, g_usb_reads, g_usb_worst * 1000);
            }
        }
        if (usb_dead) {
            set_fast(0);    /* (it may be too late: the next phases are not run) */
            printf("\n[i915stress] !!! THE USB STOPPED in phase %d (%s): a read of the stick %.1f s without an answer\n",
                   mask, group_name(mask), now_s() - g_usb_asked);
            printf("[i915stress] !!! the fast path is off again; photograph this screen\n");
            for (int w = 0; w < workers; w++) kill(pid[w], SIGKILL);
            failed = 2;
            break;
        }
        for (int w = 0; w < workers; w++) kill(pid[w], SIGTERM);
        double tw = now_s();
        int hung = 0;
        for (int w = 0; w < workers; w++) {
            int st;
            while (waitpid(pid[w], &st, WNOHANG) == 0) {
                if (now_s() - tw > 10) { hung = 1; kill(pid[w], SIGKILL); waitpid(pid[w], &st, 0); break; }
                usleep(50 * 1000);
            }
        }
        long fr = 0, bad = 0, objs = 0;
        int dead = 0;
        for (int w = 0; w < workers; w++)
            for (int t = 0; t < 2; t++) {
                fr += g_sh->frames[w][t]; bad += g_sh->bad[w][t]; objs += g_sh->objs[w][t];
                if (g_sh->dead[w][t]) { dead++; printf("[i915stress]   program %d thread %d: GL failed (step %d)\n", w, t, g_sh->dead[w][t]); }
            }
        int ok = !bad && !dead && !hung && fr > 0 && !g_usb_errs && g_usb_worst < 1.0;
        printf("[i915stress] %s phase %d: %ld frames (%.0f/s), %ld wrong, %ld objects made+dropped, %d threads failed%s; USB %ld reads, %ld errors, worst %.0f ms\n",
               ok ? "PASS" : "FAIL", mask, fr, fr / (double)secs, bad, objs, dead, hung ? ", a program would not end" : "",
               g_usb_reads, g_usb_errs, g_usb_worst * 1000);
        if (!ok) failed = 1;
    }
    set_fast(0);
    printf("\n[i915stress] fast path off again. %s\n", failed == 2 ? "THE USB DIED (see above)" : failed ? "SOME PHASES FAILED" : "ALL PHASES PASSED");
    return failed;
}
