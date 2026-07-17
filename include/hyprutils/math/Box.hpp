#ifndef HYPRUTILS_MATH_BOX_HPP
#define HYPRUTILS_MATH_BOX_HPP

#include "Vector2D.hpp"
#include <algorithm>

namespace Hyprutils {
namespace Math {

struct SBoxExtents {
    Vector2D topLeft;
    Vector2D bottomRight;

    bool operator==(const SBoxExtents& other) const {
        return topLeft == other.topLeft && bottomRight == other.bottomRight;
    }
    bool operator!=(const SBoxExtents& other) const {
        return topLeft != other.topLeft || bottomRight != other.bottomRight;
    }

    SBoxExtents& addExtents(const SBoxExtents& other) {
        topLeft += other.topLeft;
        bottomRight += other.bottomRight;
        return *this;
    }
};

class CBox {
public:
    double x = 0;
    double y = 0;
    union {
        double w = 0;
        double width;
    };
    union {
        double h = 0;
        double height;
    };

    CBox() : x(0), y(0), w(0), h(0) {}
    CBox(double val) : x(val), y(val), w(val), h(val) {}
    CBox(int val) : x(val), y(val), w(val), h(val) {}
    CBox(double xx, double yy, double ww, double hh) : x(xx), y(yy), w(ww), h(hh) {}
    CBox(const Vector2D& pos, const Vector2D& size) : x(pos.x), y(pos.y), w(size.x), h(size.y) {}

    bool operator==(const CBox& other) const {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }

    bool empty() const { return w <= 0 || h <= 0; }
    CBox& transform(int tr, double trW, double trH) { return *this; }

    CBox& noNegativeSize() {
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        return *this;
    }

    Vector2D closestPoint(const Vector2D& p) const {
        return {std::clamp(p.x, x, x + w), std::clamp(p.y, y, y + h)};
    }

    Vector2D middle() const { return pos() + size() / 2.0; }

    CBox& scaleFromCenter(double factor) {
        Vector2D mid = middle();
        w *= factor;
        h *= factor;
        x = mid.x - w / 2.0;
        y = mid.y - h / 2.0;
        return *this;
    }

    Vector2D pos() const {
        return Vector2D(x, y);
    }

    Vector2D size() const {
        return Vector2D(w, h);
    }

    CBox& translate(const Vector2D& vec) {
        x += vec.x;
        y += vec.y;
        return *this;
    }

    CBox& scale(double val) {
        x *= val;
        y *= val;
        w *= val;
        h *= val;
        return *this;
    }

    CBox& scale(const Vector2D& vec) {
        x *= vec.x;
        y *= vec.y;
        w *= vec.x;
        h *= vec.y;
        return *this;
    }

    bool containsPoint(const Vector2D& pt) const {
        return pt.x >= x && pt.x <= x + w && pt.y >= y && pt.y <= y + h;
    }

    CBox copy() const { return *this; }
    template<typename T> bool inside(const T&) const { return true; }
    Vector2D extent() const { return {w, h}; }

    CBox round() const { return CBox(std::round(x), std::round(y), std::round(w), std::round(h)); }

    CBox intersection(const CBox& other) const {
        double nx = std::max(x, other.x);
        double ny = std::max(y, other.y);
        double nw = std::min(x + w, other.x + other.w) - nx;
        double nh = std::min(y + h, other.y + other.h) - ny;
        if (nw <= 0 || nh <= 0)
            return CBox{0, 0, 0, 0};
        return CBox{nx, ny, nw, nh};
    }

    bool overlaps(const CBox& other) const {
        return !intersection(other).empty();
    }

    bool overlaps(const Vector2D& pt) const {
        return containsPoint(pt);
    }

    CBox& expand(double val) {
        x -= val;
        y -= val;
        w += val * 2;
        h += val * 2;
        return *this;
    }

    CBox& addExtents(const SBoxExtents& extents) {
        x -= extents.topLeft.x;
        y -= extents.topLeft.y;
        w += extents.topLeft.x + extents.bottomRight.x;
        h += extents.topLeft.y + extents.bottomRight.y;
        return *this;
    }
};

}
}

using CBox = Hyprutils::Math::CBox;
using SBoxExtents = Hyprutils::Math::SBoxExtents;

#endif /* HYPRUTILS_MATH_BOX_HPP */
