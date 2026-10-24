/*
 * WynlandOS - irisgl: Mesa's iris (OpenGL ES) on the Intel GPU through the
 * kernel's i915 interface (drivers/gpu/i915_uapi.c) -- tests/gltest.c made
 * for real hardware: Mesa's loader and EGL say what they decide (debug
 * logs), the renderer must be Intel's, the pixels right.
 *
 * Built on the developer's machine (glibc, the image's Mesa ABI):
 *   gcc -O2 -I/usr/include/libdrm -o irisgl irisgl.c -lEGL -lGLESv2 -lgbm -ldrm
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>

#define W 64
#define H 64

static int fail(const char *what)
{
    fprintf(stderr, "[irisgl] FAIL: %s (egl err 0x%x)\n", what, eglGetError());
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
        fprintf(stderr, "[irisgl] shader compile failed: %s\n", log);
    }
    return s;
}

int main(void)
{
    setenv("EGL_LOG_LEVEL", "debug", 0);
    setenv("LIBGL_DEBUG", "verbose", 0);
    setenv("MESA_DEBUG", "1", 0);
    fprintf(stderr, "[irisgl] start\n");

    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "[irisgl] FAIL: open renderD128\n"); return 1; }
    fprintf(stderr, "[irisgl] render node fd=%d\n", fd);

    /* what Mesa asks libdrm before it trusts the node: the device behind
       the fd (PCI ids, the loader picks the driver by them) and, from a
       device number, its render node (a Wayland client given the
       compositor's GPU in linux-dmabuf feedback; Vulkan's device list) */
    {
        drmDevicePtr dev = NULL;
        int r = drmGetDevice2(fd, 0, &dev);
        if (r == 0) {
            fprintf(stderr, "[irisgl] drm device: bus %d, pci %04x:%04x\n", dev->bustype,
                    dev->bustype == DRM_BUS_PCI ? dev->deviceinfo.pci->vendor_id : 0,
                    dev->bustype == DRM_BUS_PCI ? dev->deviceinfo.pci->device_id : 0);
            drmFreeDevice(&dev);
        } else fprintf(stderr, "[irisgl] FAIL: drmGetDevice2: %d\n", r);
        struct stat st;
        fstat(fd, &st);
        dev = NULL;
        r = drmGetDeviceFromDevId(st.st_rdev, 0, &dev);
        if (r == 0 && (dev->available_nodes & (1 << DRM_NODE_RENDER)))
            fprintf(stderr, "[irisgl] drm device of %u:%u: render node %s\n", major(st.st_rdev),
                    minor(st.st_rdev), dev->nodes[DRM_NODE_RENDER]);
        else fprintf(stderr, "[irisgl] FAIL: drmGetDeviceFromDevId(%u:%u): %d\n",
                     major(st.st_rdev), minor(st.st_rdev), r);
        if (dev) drmFreeDevice(&dev);
        char *name = drmGetDeviceNameFromFd2(fd);
        fprintf(stderr, "[irisgl] %s: drm node name %s\n", name ? "ok" : "FAIL", name ? name : "(none)");
        free(name);
    }

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "[irisgl] FAIL: gbm_create_device\n"); return 1; }
    fprintf(stderr, "[irisgl] gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_dpy ? get_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY) return fail("eglGetPlatformDisplayEXT");
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) return fail("eglInitialize");
    fprintf(stderr, "[irisgl] EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

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
    fprintf(stderr, "[irisgl] GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
    fprintf(stderr, "[irisgl] GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[irisgl] FAIL: framebuffer incomplete\n");
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
    fprintf(stderr, "[irisgl] glError=0x%x inside=(%u,%u,%u,%u) outside=(%u,%u,%u,%u)\n", err,
           in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);

    const char *rend = (const char *)glGetString(GL_RENDERER);
    int is_intel = rend && strstr(rend, "Intel") != NULL;
    int ok = is_intel && in[0] > 240 && in[1] < 15 && in[2] < 15 &&
             out[0] > 40 && out[0] < 62 && out[1] > 92 && out[1] < 112 && out[2] > 194 && out[2] < 214;
    fprintf(stderr, "[irisgl] %s\n", ok ? "PASS: IRIS RENDERED ON THE INTEL GPU" : (is_intel ? "FAIL: unexpected pixels" : "FAIL: not Intel (software fallback?)"));

    /* 3. a texture from the CPU (what Qt's images and glyphs are): a
       known pattern uploaded, drawn over the whole target, read back.
       Texel (x, y) = (x * 4, y * 4, 0x80, 0xFF) of a TS x TS texture. */
    enum { TS = 256 };
    unsigned char *img = malloc(TS * TS * 4);
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) {
            unsigned char *t = &img[(y * TS + x) * 4];
            t[0] = (unsigned char)(x * 4); t[1] = (unsigned char)(y * 4); t[2] = 0x80; t[3] = 0xFF;
        }
    GLuint src;
    glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TS, TS, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    GLuint tprog = glCreateProgram();
    glAttachShader(tprog, shader(GL_VERTEX_SHADER,
        "attribute vec2 p; varying vec2 uv; void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(tprog, shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform sampler2D t; void main() { gl_FragColor = texture2D(t, uv); }"));
    glBindAttribLocation(tprog, 0, "p");
    glLinkProgram(tprog);
    glUseProgram(tprog);
    glUniform1i(glGetUniformLocation(tprog, "t"), 0);
    static const GLfloat quad[] = { -1, -1,  1, -1,  -1, 1,  1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    memset(px, 0, sizeof(px));
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    err = glGetError();
    /* pixel (x, y) of the W x H target samples texel (x * TS / W, y * TS / H) */
    int tok = 1;
    static const int at[3][2] = { { 1, 1 }, { 32, 16 }, { 60, 50 } };
    for (int i = 0; i < 3; i++) {
        int x = at[i][0], y = at[i][1];
        unsigned char *p = &px[(y * W + x) * 4];
        int tx = x * TS / W, ty = y * TS / H;
        unsigned char want[4] = { (unsigned char)(tx * 4), (unsigned char)(ty * 4), 0x80, 0xFF };
        fprintf(stderr, "[irisgl] texture: pixel (%d,%d) = (%u,%u,%u,%u), want (%u,%u,%u,%u)\n", x, y,
                p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
        for (int c = 0; c < 4; c++) if (p[c] + 2 < want[c] || p[c] > want[c] + 2) tok = 0;
    }
    fprintf(stderr, "[irisgl] glError=0x%x -- %s\n", err,
            tok ? "PASS: A TEXTURE FROM THE CPU SAMPLED ON THE GPU" : "FAIL: the texture came out wrong");
    ok = ok && tok;
    free(img);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return ok ? 0 : 1;
}
