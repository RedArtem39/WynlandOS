#pragma once
#include <stdint.h>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t XcursorPixel;

typedef struct _XcursorImage {
    uint32_t version;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t xhot;
    uint32_t yhot;
    uint32_t delay;
    XcursorPixel *pixels;
} XcursorImage;

typedef struct _XcursorImages {
    int nimage;
    XcursorImage **images;
    char *name;
} XcursorImages;

static inline XcursorImages* XcursorLibraryLoadImages(const char* library, const char* theme, int size) { return NULL; }
static inline XcursorImages* XcursorShapeLoadImages(int shape, const char* theme, int size) { return NULL; }
static inline XcursorImages* XcursorFileLoadImages(void* file, int size) { return NULL; }
inline void XcursorImagesDestroy(XcursorImages *images) {}
inline void XcursorImageDestroy(XcursorImage *image) {}
inline const char *XcursorLibraryPath(void) { return ""; }
inline int XcursorGetDefaultSize(void *dpy) { return 24; }
inline char *XcursorGetTheme(void *dpy) { return nullptr; }

#ifdef __cplusplus
}
#endif
