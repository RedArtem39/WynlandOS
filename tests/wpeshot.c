/*
 * WynlandOS - WPE WebKit headless check (glibc, host-built)
 * ============================================================
 * wpeshot <url> [name] [settle-ms] [timeout-ms] [action-js] [status-js]
 *
 * Loads <url> in a WPE WebKit view on WPEPlatform's headless display,
 * waits for the load to finish (plus settle-ms for scripts to draw),
 * takes a snapshot of the visible area and writes it as a PNG to the
 * file: $WPESHOT_DIR/<name>.png (default /tmp).
 * Progress, console messages and failures go to stderr as
 * "[wpeshot] ..." lines. action-js runs once the load finished (e.g.
 * start a video), status-js is evaluated and printed right before the
 * snapshot. Exit 0 when a snapshot was written.
 *
 * Build: gcc -O2 $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0) -lz
 */
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <zlib.h>

#define VIEW_W 1280
#define VIEW_H 720

static GMainLoop *g_loop;
static WebKitWebView *g_view;
static const char *g_name = "page";
static guint g_settle_ms = 3000;
static int g_rc = 1;
static gint64 g_t0;
static gboolean g_shot_started;
static const char *g_action_js, *g_status_js;
static gboolean g_status_failed;   /* status-js said "FAIL..." */

static long ms(void) { return (long)((g_get_monotonic_time() - g_t0) / 1000); }

/* ---- PNG (RGBA, zlib) ---------------------------------------------------- */

static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(GByteArray *png, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    g_byte_array_append(png, hdr, 8);
    if (len) g_byte_array_append(png, data, len);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    unsigned char c[4];
    be32(c, (uint32_t)crc);
    g_byte_array_append(png, c, 4);
}

/* WebKit's snapshot is premultiplied BGRA (little-endian ARGB32) */
static GByteArray *encode_png(const unsigned char *px, int w, int h, unsigned stride)
{
    size_t rawlen = (size_t)h * (1 + (size_t)w * 4);
    unsigned char *raw = g_malloc(rawlen);
    for (int y = 0; y < h; y++) {
        unsigned char *o = raw + (size_t)y * (1 + (size_t)w * 4);
        const unsigned char *i = px + (size_t)y * stride;
        *o++ = 0; /* filter: none */
        for (int x = 0; x < w; x++, i += 4) {
            unsigned a = i[3];
            unsigned r = i[2], g = i[1], b = i[0];
            if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
            *o++ = r > 255 ? 255 : r; *o++ = g > 255 ? 255 : g; *o++ = b > 255 ? 255 : b; *o++ = a;
        }
    }
    uLongf zlen = compressBound(rawlen);
    unsigned char *z = g_malloc(zlen);
    compress2(z, &zlen, raw, rawlen, 6);
    g_free(raw);

    GByteArray *png = g_byte_array_new();
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    g_byte_array_append(png, sig, 8);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; /* 8-bit RGBA */
    chunk(png, "IHDR", ihdr, 13);
    chunk(png, "IDAT", z, (uint32_t)zlen);
    chunk(png, "IEND", NULL, 0);
    g_free(z);
    return png;
}

/* $WPESHOT_DIR/<name>.png (default /tmp): the host reads it out of the
   disk image afterwards (the serial log loses lines when processes
   write at once) */
static void emit_png(GByteArray *png)
{
    const char *dir = g_getenv("WPESHOT_DIR");
    gchar *path = g_strdup_printf("%s/%s.png", dir ? dir : "/tmp", g_name);
    GError *err = NULL;
    if (g_file_set_contents(path, (const char *)png->data, png->len, &err))
        fprintf(stderr, "[wpeshot] %s: wrote %s (%u bytes)\n", g_name, path, png->len);
    else
        fprintf(stderr, "[wpeshot] %s: writing %s: %s\n", g_name, path, err->message);
    g_clear_error(&err);
    g_free(path);
    sync();
}

/* ---- WebKit ---------------------------------------------------------------- */

static void snapshot_done(GObject *obj, GAsyncResult *res, gpointer data)
{
    (void)data;
    GError *err = NULL;
    WebKitImage *img = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!img) {
        fprintf(stderr, "[wpeshot] snapshot failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        g_main_loop_quit(g_loop);
        return;
    }
    int w = webkit_image_get_width(img), h = webkit_image_get_height(img);
    GBytes *bytes = webkit_image_as_bytes(img);
    gsize len = 0;
    const unsigned char *px = g_bytes_get_data(bytes, &len);
    unsigned stride = webkit_image_get_stride(img);
    /* how much of the picture is not plain white: a blank page is a failure */
    long inked = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const unsigned char *p = px + (size_t)y * stride + (size_t)x * 4;
            if (p[0] < 240 || p[1] < 240 || p[2] < 240) inked++;
        }
    long cells = (long)((h + 3) / 4) * ((w + 3) / 4);
    fprintf(stderr, "[wpeshot] %s: snapshot %dx%d at %ld ms, %ld%% not white\n",
            g_name, w, h, ms(), cells ? inked * 100 / cells : 0);
    GByteArray *png = encode_png(px, w, h, stride);
    emit_png(png);
    g_byte_array_unref(png);
    g_object_unref(img);
    g_rc = !inked ? 2 : g_status_failed ? 3 : 0;
    g_main_loop_quit(g_loop);
}

static void start_snapshot(void)
{
    fprintf(stderr, "[wpeshot] %s: taking the snapshot (%ld ms)\n", g_name, ms());
    webkit_web_view_get_snapshot(g_view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                 NULL, snapshot_done, NULL);
}

/* data: "action" or "status"; after the status script the snapshot */
static void js_done(GObject *obj, GAsyncResult *res, gpointer data)
{
    const char *what = data;
    gboolean status = strcmp(what, "status") == 0;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) {
        fprintf(stderr, "[wpeshot] %s: %s script failed: %s\n", g_name, what, err ? err->message : "?");
        g_clear_error(&err);
        if (status) { g_status_failed = TRUE; start_snapshot(); }
        return;
    }
    char *s = jsc_value_to_string(v);
    fprintf(stderr, "[wpeshot] %s: %s -> %s (%ld ms)\n", g_name, what, s ? s : "?", ms());
    if (status && s && strncmp(s, "FAIL", 4) == 0) g_status_failed = TRUE;
    g_free(s);
    g_object_unref(v);
    if (status) start_snapshot();
}

static void run_js(const char *js, const char *what)
{
    webkit_web_view_evaluate_javascript(g_view, js, -1, NULL, NULL, NULL, js_done, (gpointer)what);
}

static gboolean take_snapshot(gpointer data)
{
    (void)data;
    if (g_shot_started) return G_SOURCE_REMOVE;
    g_shot_started = TRUE;
    /* WPESHOT_SAMPLES=n: n thread dumps a second apart first -- where the
       page's time goes (a poor man's profiler, WynlandOS syscall 1000) */
    const char *samples = g_getenv("WPESHOT_SAMPLES");
    for (int i = 0; samples && i < atoi(samples); i++) {
        if (i) g_usleep(1000000);
        syscall(1000);
    }
    if (g_status_js) run_js(g_status_js, "status");   /* then the snapshot */
    else start_snapshot();
    return G_SOURCE_REMOVE;
}

static void load_changed(WebKitWebView *v, WebKitLoadEvent ev, gpointer data)
{
    (void)data;
    static const char *names[] = { "started", "redirected", "committed", "finished" };
    fprintf(stderr, "[wpeshot] %s: load %s (%ld ms) %s\n", g_name, names[ev], ms(), webkit_web_view_get_uri(v));
    if (ev == WEBKIT_LOAD_FINISHED) {
        if (g_action_js) run_js(g_action_js, "action");
        g_timeout_add(g_settle_ms, take_snapshot, NULL);
    }
}

static gboolean load_failed(WebKitWebView *v, WebKitLoadEvent ev, char *uri, GError *err, gpointer data)
{
    (void)v; (void)ev; (void)data;
    fprintf(stderr, "[wpeshot] %s: load failed: %s: %s\n", g_name, uri, err->message);
    return FALSE;
}

static gboolean tls_failed(WebKitWebView *v, char *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer data)
{
    (void)v; (void)cert; (void)data;
    fprintf(stderr, "[wpeshot] %s: TLS error 0x%x for %s\n", g_name, errors, uri);
    return FALSE;
}

static void process_died(WebKitWebView *v, WebKitWebProcessTerminationReason why, gpointer data)
{
    (void)v; (void)data;
    fprintf(stderr, "[wpeshot] %s: web process terminated (%s) at %ld ms\n", g_name,
            why == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" :
            why == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory limit" : "terminated by API", ms());
    g_main_loop_quit(g_loop);
}

static void progress(GObject *obj, GParamSpec *ps, gpointer data)
{
    (void)ps; (void)data;
    static int last = -1;
    int p = (int)(webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(obj)) * 10);
    if (p != last) { last = p; fprintf(stderr, "[wpeshot] %s: %d%% (%ld ms)\n", g_name, p * 10, ms()); }
}

static gboolean profile_tick(gpointer data)
{
    (void)data;
    if (g_shot_started) return G_SOURCE_REMOVE;
    syscall(1000);
    return G_SOURCE_CONTINUE;
}

static gboolean timed_out(gpointer data)
{
    (void)data;
    fprintf(stderr, "[wpeshot] %s: timeout at %ld ms, snapshot of what there is\n", g_name, ms());
    syscall(1000);   /* WynlandOS: every user thread's state and stack on the serial log */
    g_usleep(4000000);
    syscall(1000);   /* again: moving, or stuck in one place? */
    take_snapshot(NULL);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: wpeshot <url> [name] [settle-ms] [timeout-ms]\n"); return 64; }
    if (argc > 2) g_name = argv[2];
    if (argc > 3) g_settle_ms = (guint)atoi(argv[3]);
    guint timeout_ms = argc > 4 ? (guint)atoi(argv[4]) : 120000;
    if (argc > 5 && argv[5][0]) g_action_js = argv[5];   /* run once the load finished */
    if (argc > 6 && argv[6][0]) g_status_js = argv[6];   /* printed right before the snapshot */
    g_t0 = g_get_monotonic_time();

    /* WebKit starts its processes in a bubblewrap sandbox: WynlandOS has
       no bwrap and no namespaces (process isolation is its own work) */
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);

    /* WebKit first: it requires its main-thread object to be the first
       WTF thread (initializeMainThread() asserts uid == 1), and the
       display may start a WTF thread of its own when it finds a DRM
       device (it does in the VM) */
    webkit_web_context_get_default();

    GError *err = NULL;
    WPEDisplay *display = wpe_display_headless_new();
    if (!wpe_display_connect(display, &err)) {
        fprintf(stderr, "[wpeshot] headless display: %s\n", err ? err->message : "?");
        return 3;
    }
    g_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL));
    WPEView *wv = webkit_web_view_get_wpe_view(g_view);
    if (wv && wpe_view_get_toplevel(wv))
        wpe_toplevel_resize(wpe_view_get_toplevel(wv), VIEW_W, VIEW_H);

    WebKitSettings *s = webkit_web_view_get_settings(g_view);
    webkit_settings_set_enable_developer_extras(s, FALSE);
    webkit_settings_set_enable_write_console_messages_to_stdout(s, TRUE);

    g_signal_connect(g_view, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(g_view, "load-failed", G_CALLBACK(load_failed), NULL);
    g_signal_connect(g_view, "load-failed-with-tls-errors", G_CALLBACK(tls_failed), NULL);
    g_signal_connect(g_view, "web-process-terminated", G_CALLBACK(process_died), NULL);
    g_signal_connect(g_view, "notify::estimated-load-progress", G_CALLBACK(progress), NULL);

    /* WPESHOT_PROFILE=ms: a thread dump that often while the page loads
       (WynlandOS syscall 1000) -- what the web process does meanwhile */
    const char *prof = g_getenv("WPESHOT_PROFILE");
    if (prof && atoi(prof) > 0)
        g_timeout_add((guint)atoi(prof), profile_tick, NULL);

    fprintf(stderr, "[wpeshot] %s: loading %s\n", g_name, argv[1]);
    webkit_web_view_load_uri(g_view, argv[1]);
    g_timeout_add(timeout_ms, timed_out, NULL);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);
    fprintf(stderr, "[wpeshot] %s: done rc=%d (%ld ms)\n", g_name, g_rc, ms());
    return g_rc;
}
