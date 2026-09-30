/*
 * WynlandOS - KMS + GPU presentation test (glibc, host-built)
 * ============================================================
 * 1. KMS discovery (resources, connector, mode, CRTC).
 * 2. A CPU-drawn dumb buffer on screen (ADDFB + SETCRTC).
 * 3. GBM + EGL on /dev/dri/card0: GLES frames rendered by virgl on the
 *    host GPU, shown with PAGE_FLIP + flip-complete events -- no CPU copy
 *    of the pixels anywhere. Reports frames per second.
 * 4. PRIME: export a frame's buffer as a dma-buf fd and import it back.
 * 5. Holds a recognisable final frame (dark blue, green triangle, white
 *    bar at the top) for a screenshot, then gives the screen back.
 * One "[kmstest] ..." line per step.
 *
 * Build: gcc -O2 -I/usr/include/libdrm -o build/kmstest.elf tests/kmstest.c \
 *            -ldrm -lgbm -lEGL -lGLESv2
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define LOG(...) fprintf(stderr, "[kmstest] " __VA_ARGS__)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int g_fd;
static uint32_t g_crtc, g_conn;
static drmModeModeInfo g_mode;
static int g_flip_pending;

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    g_flip_pending = 0;
}

static int wait_flip(void)
{
    drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
    while (g_flip_pending) {
        struct pollfd p = { .fd = g_fd, .events = POLLIN };
        int r = poll(&p, 1, 2000);
        if (r <= 0) { LOG("FAIL: no flip event (poll=%d errno=%d)\n", r, errno); return -1; }
        drmHandleEvent(g_fd, &ev);
    }
    return 0;
}

static uint32_t fb_for_bo(struct gbm_bo *bo)
{
    uint32_t *cached = gbm_bo_get_user_data(bo);
    if (cached) return *cached;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 }, pitches[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { 0 }, id = 0;
    if (drmModeAddFB2(g_fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                      gbm_bo_get_format(bo), handles, pitches, offsets, &id, 0)) {
        LOG("FAIL: AddFB2 (errno %d)\n", errno);
        return 0;
    }
    uint32_t *p = malloc(sizeof(*p));
    *p = id;
    gbm_bo_set_user_data(bo, p, NULL);
    return id;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(void)
{
    LOG("start\n");
    g_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) { LOG("FAIL: open card0\n"); return 1; }

    /* 1. discovery */
    drmModeRes *res = drmModeGetResources(g_fd);
    if (!res || res->count_connectors < 1 || res->count_crtcs < 1) { LOG("FAIL: GetResources\n"); return 1; }
    drmModeConnector *conn = drmModeGetConnector(g_fd, res->connectors[0]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes < 1) { LOG("FAIL: GetConnector\n"); return 1; }
    g_conn = conn->connector_id;
    g_crtc = res->crtcs[0];
    g_mode = conn->modes[0];
    int W = g_mode.hdisplay, H = g_mode.vdisplay;
    LOG("PASS discovery: connector %u, crtc %u, mode %s @%uHz\n", g_conn, g_crtc, g_mode.name, g_mode.vrefresh);

    /* 2. dumb buffer, drawn by the CPU */
    struct drm_mode_create_dumb cd = { .width = W, .height = H, .bpp = 32 };
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { LOG("FAIL: CREATE_DUMB errno %d\n", errno); return 1; }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, md.offset);
    if (px == MAP_FAILED) { LOG("FAIL: mmap dumb\n"); return 1; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * (cd.pitch / 4) + x] = ((x * 255 / W) << 16) | ((y * 255 / H) << 8) | 0x40;
    uint32_t dumb_fb;
    if (drmModeAddFB(g_fd, W, H, 24, 32, cd.pitch, cd.handle, &dumb_fb)) { LOG("FAIL: AddFB dumb\n"); return 1; }
    if (drmModeSetCrtc(g_fd, g_crtc, dumb_fb, 0, 0, &g_conn, 1, &g_mode)) { LOG("FAIL: SetCrtc dumb errno %d\n", errno); return 1; }
    LOG("PASS dumb buffer on screen (%ux%u, pitch %u)\n", W, H, cd.pitch);
    usleep(1500 * 1000);

    /* 3. GBM + EGL + GLES, presented by page flips */
    struct gbm_device *gbm = gbm_create_device(g_fd);
    struct gbm_surface *gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gbm || !gs) { LOG("FAIL: gbm device/surface\n"); return 1; }
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!eglInitialize(dpy, NULL, NULL)) { LOG("FAIL: eglInitialize 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfgs[64], cfg = NULL;
    EGLint n = 0;
    eglChooseConfig(dpy, ca, cfgs, 64, &n);
    for (int i = 0; i < n; i++) {
        EGLint vid;
        eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
        if (vid == GBM_FORMAT_XRGB8888) { cfg = cfgs[i]; break; }
    }
    if (!cfg) { LOG("FAIL: no XRGB8888 EGL config (%d configs)\n", n); return 1; }
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOG("FAIL: EGL context/surface 0x%x\n", eglGetError());
        return 1;
    }
    LOG("renderer: %s\n", (const char *)glGetString(GL_RENDERER));

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER,
        "attribute vec2 p; uniform float a;"
        "void main() { float c = cos(a), s = sin(a);"
        "  gl_Position = vec4(c * p.x - s * p.y, s * p.x + c * p.y, 0.0, 1.0); }"));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER,
        "precision mediump float; uniform vec4 col; void main() { gl_FragColor = col; }"));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);
    GLint ua = glGetUniformLocation(prog, "a"), uc = glGetUniformLocation(prog, "col");
    static const GLfloat tri[] = { 0.0f, 0.7f, -0.6f, -0.5f, 0.6f, -0.5f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);

    struct gbm_bo *prev = NULL;
    const int FRAMES = 240;
    long t0 = now_ms();
    int shown = 0;
    for (int f = 0; f <= FRAMES; f++) {
        int final = (f == FRAMES);
        if (final) {
            glClearColor(0.05f, 0.08f, 0.35f, 1.0f);           /* dark blue */
        } else {
            float k = (float)f / FRAMES;
            glClearColor(0.5f + 0.5f * k, 0.2f, 0.5f - 0.4f * k, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(ua, final ? 0.0f : f * 0.05f);
        glUniform4f(uc, final ? 0.1f : 1.0f, final ? 0.9f : 1.0f, final ? 0.2f : 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (final) { /* white bar across the top: easy to spot in a screenshot */
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, H - H / 12, W, H / 12);
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        if (!eglSwapBuffers(dpy, surf)) { LOG("FAIL: eglSwapBuffers 0x%x\n", eglGetError()); return 1; }
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
        uint32_t fb = bo ? fb_for_bo(bo) : 0;
        if (!fb) { LOG("FAIL: no fb for frame %d\n", f); return 1; }
        if (f == 0) {
            if (drmModeSetCrtc(g_fd, g_crtc, fb, 0, 0, &g_conn, 1, &g_mode)) { LOG("FAIL: SetCrtc gl\n"); return 1; }
        } else {
            g_flip_pending = 1;
            if (drmModePageFlip(g_fd, g_crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
                LOG("FAIL: PageFlip errno %d\n", errno);
                return 1;
            }
            if (wait_flip()) return 1;
        }
        shown++;
        if (prev) gbm_surface_release_buffer(gs, prev);
        prev = bo;

        if (final) {
            /* 4. PRIME round trip on the frame on screen */
            int dfd = -1;
            uint32_t back = 0;
            int e = drmPrimeHandleToFD(g_fd, gbm_bo_get_handle(bo).u32, DRM_CLOEXEC, &dfd);
            int i2 = e ? -1 : drmPrimeFDToHandle(g_fd, dfd, &back);
            if (e == 0 && i2 == 0 && back == gbm_bo_get_handle(bo).u32)
                LOG("PASS PRIME export/import (dma-buf fd %d)\n", dfd);
            else
                LOG("FAIL PRIME (export %d, import %d, handle %u vs %u)\n", e, i2, back, gbm_bo_get_handle(bo).u32);
            if (dfd >= 0) close(dfd);
        }
    }
    long dt = now_ms() - t0;
    LOG("PASS %d GPU frames page-flipped in %ld ms: %.1f fps\n", shown, dt, dt ? shown * 1000.0 / dt : 0.0);

    LOG("holding the final frame for a screenshot\n");
    sleep(20);
    drmModeSetCrtc(g_fd, g_crtc, 0, 0, 0, NULL, 0, NULL); /* the console comes back */
    LOG("DONE\n");
    return 0;
}
