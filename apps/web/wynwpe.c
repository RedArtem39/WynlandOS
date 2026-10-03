/*
 * WynlandOS - a WPEPlatform for WPE WebKit inside a Qt Quick app.
 * See wynwpe.h.
 */
#include "wynwpe.h"

#include <wpe/WPEKeymapXKB.h>

/* ---------------------------------------------------------------- screen */

#define WYN_TYPE_WPE_SCREEN (wyn_wpe_screen_get_type())
G_DECLARE_FINAL_TYPE(WynWpeScreen, wyn_wpe_screen, WYN, WPE_SCREEN, WPEScreen)
struct _WynWpeScreen { WPEScreen parent; };
G_DEFINE_FINAL_TYPE(WynWpeScreen, wyn_wpe_screen, WPE_TYPE_SCREEN)
static void wyn_wpe_screen_class_init(WynWpeScreenClass *klass) { (void)klass; }
static void wyn_wpe_screen_init(WynWpeScreen *self) { (void)self; }

/* ---------------------------------------------------------------- toplevel */

#define WYN_TYPE_WPE_TOPLEVEL (wyn_wpe_toplevel_get_type())
G_DECLARE_FINAL_TYPE(WynWpeToplevel, wyn_wpe_toplevel, WYN, WPE_TOPLEVEL, WPEToplevel)
struct _WynWpeToplevel { WPEToplevel parent; };
G_DEFINE_FINAL_TYPE(WynWpeToplevel, wyn_wpe_toplevel, WPE_TYPE_TOPLEVEL)

/* the Qt item decides the size: whatever is asked is granted */
static gboolean toplevel_resize(WPEToplevel *toplevel, int width, int height)
{
    wpe_toplevel_resized(toplevel, width, height);
    return TRUE;
}

static WPEScreen *toplevel_get_screen(WPEToplevel *toplevel)
{
    WPEDisplay *display = wpe_toplevel_get_display(toplevel);
    return display ? wpe_display_get_screen(display, 0) : NULL;
}

static void wyn_wpe_toplevel_class_init(WynWpeToplevelClass *klass)
{
    WPEToplevelClass *tc = WPE_TOPLEVEL_CLASS(klass);
    tc->resize = toplevel_resize;
    tc->get_screen = toplevel_get_screen;
}
static void wyn_wpe_toplevel_init(WynWpeToplevel *self) { (void)self; }

/* ---------------------------------------------------------------- view */

#define WYN_TYPE_WPE_VIEW (wyn_wpe_view_get_type())
G_DECLARE_FINAL_TYPE(WynWpeView, wyn_wpe_view, WYN, WPE_VIEW, WPEView)
struct _WynWpeView {
    WPEView parent;
    WynWpeFrameFunc func;
    gpointer user;
    WPEBuffer *shown;     /* handed over, "on screen" at the next tick */
    guint tick;
};
G_DEFINE_FINAL_TYPE(WynWpeView, wyn_wpe_view, WPE_TYPE_VIEW)

/* 60 Hz: the frame we got is now "on screen" -- WebKit draws the next */
static gboolean view_tick(gpointer data)
{
    WynWpeView *self = data;
    self->tick = 0;
    WPEBuffer *buffer = self->shown;
    self->shown = NULL;
    if (buffer) {
        wpe_view_buffer_rendered(WPE_VIEW(self), buffer);
        wpe_view_buffer_released(WPE_VIEW(self), buffer);   /* copied: WebKit may reuse it */
        g_object_unref(buffer);
    }
    return G_SOURCE_REMOVE;
}

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage,
                                   guint n_damage, GError **error)
{
    (void)damage; (void)n_damage;
    WynWpeView *self = WYN_WPE_VIEW(view);
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    if (WPE_IS_BUFFER_SHM(buffer)) {
        WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
        gsize len = 0;
        const guint8 *px = g_bytes_get_data(wpe_buffer_shm_get_data(shm), &len);
        if (self->func && px) self->func(self->user, px, w, h, wpe_buffer_shm_get_stride(shm));
    } else {
        /* a GPU buffer (DMA-BUF): read back to pixels */
        GBytes *bytes = wpe_buffer_import_to_pixels(buffer, error);
        if (!bytes) return FALSE;
        gsize len = 0;
        const guint8 *px = g_bytes_get_data(bytes, &len);
        if (self->func && px) self->func(self->user, px, w, h, (guint)w * 4);
        g_bytes_unref(bytes);
    }
    if (self->shown) {   /* not expected: WebKit waits for "rendered" */
        wpe_view_buffer_rendered(view, self->shown);
        wpe_view_buffer_released(view, self->shown);
        g_object_unref(self->shown);
    }
    self->shown = g_object_ref(buffer);
    if (!self->tick) self->tick = g_timeout_add(16, view_tick, self);
    return TRUE;
}

static gboolean view_can_be_mapped(WPEView *view) { (void)view; return TRUE; }

static void view_dispose(GObject *object)
{
    WynWpeView *self = WYN_WPE_VIEW(object);
    if (self->tick) { g_source_remove(self->tick); self->tick = 0; }
    g_clear_object(&self->shown);
    G_OBJECT_CLASS(wyn_wpe_view_parent_class)->dispose(object);
}

static void wyn_wpe_view_class_init(WynWpeViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = view_dispose;
    WPEViewClass *vc = WPE_VIEW_CLASS(klass);
    vc->render_buffer = view_render_buffer;
    vc->can_be_mapped = view_can_be_mapped;
}
static void wyn_wpe_view_init(WynWpeView *self) { (void)self; }

void wyn_wpe_view_set_frame_func(WPEView *view, WynWpeFrameFunc func, gpointer user)
{
    g_return_if_fail(WYN_IS_WPE_VIEW(view));
    WYN_WPE_VIEW(view)->func = func;
    WYN_WPE_VIEW(view)->user = user;
}

/* ---------------------------------------------------------------- display */

#define WYN_TYPE_WPE_DISPLAY (wyn_wpe_display_get_type())
G_DECLARE_FINAL_TYPE(WynWpeDisplay, wyn_wpe_display, WYN, WPE_DISPLAY, WPEDisplay)
struct _WynWpeDisplay {
    WPEDisplay parent;
    WPEScreen *screen;
    WPEKeymap *keymap;
};
G_DEFINE_FINAL_TYPE(WynWpeDisplay, wyn_wpe_display, WPE_TYPE_DISPLAY)

static gboolean display_connect(WPEDisplay *display, GError **error) { (void)display; (void)error; return TRUE; }

static WPEView *display_create_view(WPEDisplay *display)
{
    WPEView *view = WPE_VIEW(g_object_new(WYN_TYPE_WPE_VIEW, "display", display, NULL));
    WPEToplevel *toplevel = WPE_TOPLEVEL(g_object_new(WYN_TYPE_WPE_TOPLEVEL, "display", display, "max-views", 1, NULL));
    wpe_view_set_toplevel(view, toplevel);
    g_object_unref(toplevel);
    return view;
}

static WPEToplevel *display_create_toplevel(WPEDisplay *display, guint max_views)
{
    return WPE_TOPLEVEL(g_object_new(WYN_TYPE_WPE_TOPLEVEL, "display", display, "max-views", max_views, NULL));
}

static WPEKeymap *display_get_keymap(WPEDisplay *display)
{
    WynWpeDisplay *self = WYN_WPE_DISPLAY(display);
    if (!self->keymap) self->keymap = wpe_keymap_xkb_new();
    return self->keymap;
}

static guint display_get_n_screens(WPEDisplay *display) { (void)display; return 1; }

static WPEScreen *display_get_screen(WPEDisplay *display, guint index)
{
    return index == 0 ? WYN_WPE_DISPLAY(display)->screen : NULL;
}

/* no DRM device: WebKit renders into shared memory */
static WPEDRMDevice *display_get_drm_device(WPEDisplay *display) { (void)display; return NULL; }

static void display_dispose(GObject *object)
{
    WynWpeDisplay *self = WYN_WPE_DISPLAY(object);
    g_clear_object(&self->screen);
    g_clear_object(&self->keymap);
    G_OBJECT_CLASS(wyn_wpe_display_parent_class)->dispose(object);
}

static void wyn_wpe_display_class_init(WynWpeDisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = display_dispose;
    WPEDisplayClass *dc = WPE_DISPLAY_CLASS(klass);
    dc->connect = display_connect;
    dc->create_view = display_create_view;
    dc->create_toplevel = display_create_toplevel;
    dc->get_keymap = display_get_keymap;
    dc->get_n_screens = display_get_n_screens;
    dc->get_screen = display_get_screen;
    dc->get_drm_device = display_get_drm_device;
}
static void wyn_wpe_display_init(WynWpeDisplay *self) { (void)self; }

WPEDisplay *wyn_wpe_display_new(int screen_width, int screen_height)
{
    WynWpeDisplay *self = g_object_new(WYN_TYPE_WPE_DISPLAY, NULL);
    self->screen = WPE_SCREEN(g_object_new(WYN_TYPE_WPE_SCREEN, "id", 1, NULL));
    wpe_screen_set_size(self->screen, screen_width, screen_height);
    wpe_screen_set_scale(self->screen, 1.0);
    wpe_screen_set_refresh_rate(self->screen, 60000);   /* mHz */
    GError *error = NULL;
    if (!wpe_display_connect(WPE_DISPLAY(self), &error)) {
        g_warning("WynWpeDisplay: %s", error ? error->message : "?");
        g_clear_error(&error);
    }
    return WPE_DISPLAY(self);
}
