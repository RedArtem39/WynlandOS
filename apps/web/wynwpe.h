/*
 * WynlandOS - a WPEPlatform for WPE WebKit inside a Qt Quick app.
 *
 * WebKit renders a view into buffers it hands to its platform. This one
 * hands every frame to a callback (the Web app copies it into the item it
 * paints) and paces the next frame at 60 Hz. Without a DRM device WebKit
 * uses shared-memory buffers: plain pixels, no GPU import needed.
 */
#pragma once

#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

/* One frame: premultiplied ARGB32 (B,G,R,A bytes), `stride` bytes a row.
   The pixels are only valid during the call. */
typedef void (*WynWpeFrameFunc)(gpointer user, const guint8 *pixels, int width, int height, guint stride);

/* The display, with one screen of the given size. */
WPEDisplay *wyn_wpe_display_new(int screen_width, int screen_height);

/* Where a view's frames go (a view of this display: webkit_web_view_get_wpe_view()). */
void wyn_wpe_view_set_frame_func(WPEView *view, WynWpeFrameFunc func, gpointer user);

G_END_DECLS
