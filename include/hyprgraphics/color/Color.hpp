#ifndef HYPRGRAPHICS_COLOR_COLOR_HPP
#define HYPRGRAPHICS_COLOR_COLOR_HPP

#include <stdint.h>
#include <memory>

namespace Hyprgraphics {

struct SPoint {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const SPoint&) const = default;
};

class CMatrix3 {
public:
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    template <typename T>
    T operator*(const T& vec) const {
        return T{
            m[0] * vec.x + m[1] * vec.y + m[2] * vec.z,
            m[3] * vec.x + m[4] * vec.y + m[5] * vec.z,
            m[6] * vec.x + m[7] * vec.y + m[8] * vec.z
        };
    }
};

struct SPCPRimaries {
    SPoint red;
    SPoint green;
    SPoint blue;
    SPoint white;
    bool operator==(const SPCPRimaries&) const = default;
    
    CMatrix3 convertMatrix(const SPCPRimaries& other) const {
        return CMatrix3();
    }
    CMatrix3 toXYZ() const {
        return CMatrix3();
    }
};


namespace Color {

class CPrimaries {
public:
    float rx = 0.64f, ry = 0.33f;
    float gx = 0.30f, gy = 0.60f;
    float bx = 0.15f, by = 0.06f;
    float wx = 0.3127f, wy = 0.3290f;
};

class CColor {
public:
    struct SSRGB {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    struct SOkLab {
        float l = 0.0f;
        float a = 0.0f;
        float b = 0.0f;
    };

    struct SHSL {
        float h = 0.0f;
        float s = 0.0f;
        float l = 0.0f;
    };

    struct xy {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct XYZ {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    float r = 0.18f;
    float g = 0.20f;
    float b = 0.25f;
    float a = 1.0f;

    CColor() {}
    CColor(uint32_t argb) {
        a = ((argb >> 24) & 0xFF) / 255.0f;
        r = ((argb >> 16) & 0xFF) / 255.0f;
        g = ((argb >> 8) & 0xFF) / 255.0f;
        b = (argb & 0xFF) / 255.0f;
    }
    CColor(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    CColor(const SOkLab& lab) : r(lab.l), g(lab.a), b(lab.b), a(1.0f) {}
    CColor(const SSRGB& rgb) : r(rgb.r), g(rgb.g), b(rgb.b), a(1.0f) {}

    uint32_t getAsHex() const {
        uint32_t aa = (uint32_t)(a * 255.0f);
        uint32_t rr = (uint32_t)(r * 255.0f);
        uint32_t gg = (uint32_t)(g * 255.0f);
        uint32_t bb = (uint32_t)(b * 255.0f);
        return (aa << 24) | (rr << 16) | (gg << 8) | bb;
    }

    SSRGB asRgb() const {
        return {r, g, b};
    }
    SOkLab asOkLab() const {
        return {0.5f, 0.0f, 0.0f};
    }
    SHSL asHSL() const {
        return {0.0f, 0.0f, 0.5f};
    }
};

using SPCPRimaries = Hyprgraphics::SPCPRimaries;

}

using Hyprgraphics::Color::CColor;
using Hyprgraphics::Color::CPrimaries;

inline CColor::XYZ xy2xyz(const SPoint& p) { return { (float)p.x, (float)p.y, 1.0f - (float)p.x - (float)p.y }; }
inline CColor::XYZ xy2xyz(const CColor::xy& p) { return { p.x, p.y, 1.0f - p.x - p.y }; }

}

using CColor = Hyprgraphics::Color::CColor;
using SPCPRimaries = Hyprgraphics::SPCPRimaries;

#endif /* HYPRGRAPHICS_COLOR_COLOR_HPP */
