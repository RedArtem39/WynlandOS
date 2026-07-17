#ifndef HYPRUTILS_MATH_REGION_HPP
#define HYPRUTILS_MATH_REGION_HPP

#include "Box.hpp"
#include <vector>
#include <functional>

namespace Hyprutils {
namespace Math {

class CRegion {
public:
    std::vector<CBox> rects;

    CRegion() {}
    CRegion(const CBox& box) { if (box.w > 0 && box.h > 0) rects.push_back(box); }
    CRegion(double x, double y, double w, double h) : CRegion(CBox(x, y, w, h)) {}

    CRegion& add(const CBox& box) { if (box.w > 0 && box.h > 0) rects.push_back(box); return *this; }
    CRegion& add(const CRegion& other) { for (auto& r : other.rects) add(r); return *this; }
    CRegion& intersect(double x, double y, double w, double h) { return *this; }
    void clear() { rects.clear(); }
    void set(const CBox& box) { clear(); add(box); }
    bool empty() const { return rects.empty(); }
    void forEachRect(std::function<void(const CBox&)> cb) const { for (auto& r : rects) cb(r); }
    void* pixman() const { return nullptr; }
    CRegion copy() const { return *this; }

    CRegion& translate(const Vector2D& vec) {
        for (auto& r : rects) r.translate(vec);
        return *this;
    }

    bool containsPoint(const Vector2D& pt) const {
        for (const auto& r : rects) {
            if (r.containsPoint(pt)) return true;
        }
        return false;
    }

    Vector2D closestPoint(const Vector2D& pt) const {
        if (containsPoint(pt)) return pt;
        if (rects.empty()) return pt;
        Vector2D closest = pt;
        double minD = 1e9;
        for (const auto& r : rects) {
            auto cp = r.closestPoint(pt);
            double d = cp.distanceSq(pt);
            if (d < minD) { minD = d; closest = cp; }
        }
        return closest;
    }

    CBox getExtents() const {
        if (rects.empty()) return CBox{0, 0, 0, 0};
        double minX = rects[0].x, minY = rects[0].y;
        double maxX = rects[0].x + rects[0].w, maxY = rects[0].y + rects[0].h;
        for (size_t i = 1; i < rects.size(); ++i) {
            minX = std::min(minX, rects[i].x);
            minY = std::min(minY, rects[i].y);
            maxX = std::max(maxX, rects[i].x + rects[i].w);
            maxY = std::max(maxY, rects[i].y + rects[i].h);
        }
        return CBox{minX, minY, maxX - minX, maxY - minY};
    }

    CRegion& transform(auto tr, double w, double h) { return *this; }
    
    CRegion& scale(double val) {
        for (auto& r : rects) r.scale(val);
        return *this;
    }

    CRegion& scale(const Vector2D& vec) {
        for (auto& r : rects) r.scale(vec);
        return *this;
    }

    CRegion& intersect(const CBox& box) {
        std::vector<CBox> newRects;
        for (const auto& r : rects) {
            auto intersection = r.intersection(box);
            if (!intersection.empty()) newRects.push_back(intersection);
        }
        rects = std::move(newRects);
        return *this;
    }
};

}
}

using CRegion = Hyprutils::Math::CRegion;

#ifdef __cplusplus
extern "C" {
#endif
static inline int pixman_region32_n_rects(void* r) { return 0; }
#ifdef __cplusplus
}
#endif

#endif /* HYPRUTILS_MATH_REGION_HPP */
