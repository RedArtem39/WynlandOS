#ifndef HYPRCURSOR_HYPRCURSOR_HPP
#define HYPRCURSOR_HYPRCURSOR_HPP

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

typedef struct _cairo_surface cairo_surface_t;

enum eHyprcursorLogLevel {
    HC_LOG_TRACE = 0
};

namespace Hyprcursor {

struct SCursorImageData {
    int32_t width = 0;
    int32_t height = 0;
    int32_t hotspotX = 0;
    int32_t hotspotY = 0;
    void*   data = nullptr;
    cairo_surface_t* surface = nullptr;
    uint32_t size = 0;
    uint32_t delay = 0;
};

struct SCursorStyleInfo {
    uint32_t size = 24;
};

struct SCursorShapeData {
    uint32_t shape = 1;
    std::vector<SCursorImageData> images;
};

struct SManagerOptions {
    void* logger = nullptr;
    void (*logFn)(eHyprcursorLogLevel, char*) = nullptr;
    bool allowDefaultFallback = true;
};

class CHyprcursorManager {
public:
    CHyprcursorManager(const char* theme, void (*callback)(eHyprcursorLogLevel, char*) = nullptr) {}
    CHyprcursorManager(const char* theme, const SManagerOptions& options) {}
    bool valid() const { return true; }
    SCursorShapeData getShape(const char* name, const SCursorStyleInfo& info) { return SCursorShapeData(); }
    void cursorSurfaceStyleDone(const SCursorStyleInfo& info) {}
    void loadThemeStyle(const SCursorStyleInfo& info) {}
};

}

using SCursorImageData = Hyprcursor::SCursorImageData;

#endif /* HYPRCURSOR_HYPRCURSOR_HPP */
