#ifndef HYPRUTILS_MATH_MISC_HPP
#define HYPRUTILS_MATH_MISC_HPP

#include <cmath>
#include <cstdint>

namespace Hyprutils {
namespace Math {

enum eTransform : uint8_t {
    HYPRUTILS_TRANSFORM_NORMAL = 0,
    HYPRUTILS_TRANSFORM_90,
    HYPRUTILS_TRANSFORM_180,
    HYPRUTILS_TRANSFORM_270,
    HYPRUTILS_TRANSFORM_FLIPPED,
    HYPRUTILS_TRANSFORM_FLIPPED_90,
    HYPRUTILS_TRANSFORM_FLIPPED_180,
    HYPRUTILS_TRANSFORM_FLIPPED_270,
};



inline double clamp(double val, double minVal, double maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

}
}

using Hyprutils::Math::eTransform;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_NORMAL;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_90;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_180;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_270;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_FLIPPED;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_FLIPPED_90;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_FLIPPED_180;
using Hyprutils::Math::HYPRUTILS_TRANSFORM_FLIPPED_270;
using Hyprutils::Math::clamp;

#endif /* HYPRUTILS_MATH_MISC_HPP */
