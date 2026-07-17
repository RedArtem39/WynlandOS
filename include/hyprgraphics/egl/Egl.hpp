#ifndef HYPRGRAPHICS_EGL_EGL_HPP
#define HYPRGRAPHICS_EGL_EGL_HPP

namespace Hyprgraphics {
namespace Egl {

class CEGL {
public:
    CEGL() {}
};

inline int minStride(int format, int width) { return width * 4; }

} // namespace Egl
} // namespace Hyprgraphics

#include <optional>

#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1

#define SWIZZLE_RGBA 1
#define SWIZZLE_BGRA 2

struct SPixelFormat {
    int glFormat = 0;
    int glType = 0;
    int glInternalFormat = 0;
    std::optional<int> swizzle;
};

inline SPixelFormat* getPixelFormatFromDRM(uint32_t drm) {
    static SPixelFormat pf;
    return &pf;
}

using CEGL = Hyprgraphics::Egl::CEGL;

#endif /* HYPRGRAPHICS_EGL_EGL_HPP */
