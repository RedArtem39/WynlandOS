/*
 * WynlandOS - virgl end-to-end smoke test (glibc, host-built)
 * ============================================================
 * Opens /dev/dri/renderD128, brings up Mesa's virgl driver through GBM +
 * EGL (no window system), renders into an FBO -- a clear plus one shaded
 * triangle, so both the command stream and host-side shader compilation
 * are exercised -- and reads the pixels back. Every step prints one line
 * to stdout (the serial log), so a failure says exactly where it stopped.
 *
 * Build (host, glibc -- same ABI as the /lib64 Mesa in the image):
 *   gcc -O2 -o build/gltest.elf tests/gltest.c -lEGL -lGLESv2 -lgbm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define W 64
#define H 64

static int fail(const char *what)
{
    fprintf(stderr, "[gltest] FAIL: %s (egl err 0x%x)\n", what, eglGetError());
    fflush(stdout);
    return 1;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[gltest] shader compile failed: %s\n", log);
    }
    return s;
}

int main(int argc, char **argv)
{
    /* The kernel starts us without an environment: re-exec once with the
       dynamic loader's own tracing on. */
    if (argc > 0 && !getenv("GLTEST_REEXEC")) {
        char *envp[] = { "GLTEST_REEXEC=1", "LD_LIBRARY_PATH=/lib64", "LIBGL_DEBUG=verbose", "EGL_LOG_LEVEL=debug", "MESA_DEBUG=1", NULL };
        execve(argv[0], argv, envp);
    }

    /* No sysfs/PCI probing needed: name the Gallium driver outright. */
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "virtio_gpu", 1);
    setenv("EGL_LOG_LEVEL", "warning", 0);
    fprintf(stderr, "[gltest] start\n");

    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "[gltest] FAIL: open renderD128\n"); return 1; }
    fprintf(stderr, "[gltest] render node fd=%d\n", fd);

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "[gltest] FAIL: gbm_create_device\n"); return 1; }
    fprintf(stderr, "[gltest] gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_dpy ? get_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY) return fail("eglGetPlatformDisplayEXT");
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) return fail("eglInitialize");
    fprintf(stderr, "[gltest] EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        /* surfaceless contexts don't strictly need a config */
        cfg = EGL_NO_CONFIG_KHR;
    }
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) return fail("eglCreateContext");
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return fail("eglMakeCurrent (surfaceless)");
    fprintf(stderr, "[gltest] GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
    fprintf(stderr, "[gltest] GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gltest] FAIL: framebuffer incomplete\n");
        return 1;
    }
    glViewport(0, 0, W, H);

    /* 1. clear to a known colour */
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* 2. a red triangle covering the lower-left half */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER,
        "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
        "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    static const GLfloat tri[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    unsigned char px[W * H * 4];
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();

    /* bottom-left is inside the triangle, top-right outside it */
    unsigned char *in  = &px[(4 * W + 4) * 4];
    unsigned char *out = &px[((H - 5) * W + (W - 5)) * 4];
    fprintf(stderr, "[gltest] glError=0x%x inside=(%u,%u,%u,%u) outside=(%u,%u,%u,%u)\n", err,
           in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);

    const char *rend = (const char *)glGetString(GL_RENDERER);
    int is_virgl = rend && strstr(rend, "virgl") != NULL;
    int ok = is_virgl && in[0] > 240 && in[1] < 15 && in[2] < 15 &&
             out[0] > 40 && out[0] < 62 && out[1] > 92 && out[1] < 112 && out[2] > 194 && out[2] < 214;
    fprintf(stderr, "[gltest] %s\n", ok ? "PASS: virgl rendered on the host GPU" : (is_virgl ? "FAIL: unexpected pixels" : "FAIL: not virgl (software fallback)"));

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return ok ? 0 : 1;
}
