#ifndef HYPRUTILS_MATH_VECTOR2D_HPP
#define HYPRUTILS_MATH_VECTOR2D_HPP

#include <cmath>
#include <algorithm>
#include <format>
#include <string>
#include <limits>

namespace Hyprutils {
namespace Math {

class Vector2D {
public:
    double x = 0;
    double y = 0;

    constexpr Vector2D() = default;
    constexpr Vector2D(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vector2D operator+(const Vector2D& rhs) const { return Vector2D(x + rhs.x, y + rhs.y); }
    constexpr Vector2D operator-(const Vector2D& rhs) const { return Vector2D(x - rhs.x, y - rhs.y); }
    constexpr Vector2D operator-() const { return Vector2D(-x, -y); }
    constexpr Vector2D operator*(double scale) const { return Vector2D(x * scale, y * scale); }
    constexpr Vector2D operator/(double scale) const { return scale != 0 ? Vector2D(x / scale, y / scale) : Vector2D(0, 0); }
    constexpr Vector2D operator*(const Vector2D& rhs) const { return Vector2D(x * rhs.x, y * rhs.y); }
    constexpr Vector2D operator/(const Vector2D& rhs) const { return Vector2D(rhs.x != 0 ? x / rhs.x : 0, rhs.y != 0 ? y / rhs.y : 0); }
    constexpr bool operator>(const Vector2D& rhs) const { return x > rhs.x && y > rhs.y; }
    constexpr bool operator<(const Vector2D& rhs) const { return x < rhs.x && y < rhs.y; }

    constexpr Vector2D& operator+=(const Vector2D& rhs) { x += rhs.x; y += rhs.y; return *this; }
    constexpr Vector2D& operator-=(const Vector2D& rhs) { x -= rhs.x; y -= rhs.y; return *this; }
    constexpr Vector2D& operator*=(double scale) { x *= scale; y *= scale; return *this; }
    constexpr Vector2D& operator/=(double scale) { if (scale != 0) { x /= scale; y /= scale; } return *this; }

    constexpr bool operator==(const Vector2D& rhs) const { return x == rhs.x && y == rhs.y; }
    constexpr bool operator!=(const Vector2D& rhs) const { return !(*this == rhs); }

    double distance(const Vector2D& other) const {
        return std::sqrt((x - other.x) * (x - other.x) + (y - other.y) * (y - other.y));
    }
    double distanceSq(const Vector2D& other) const {
        return (x - other.x) * (x - other.x) + (y - other.y) * (y - other.y);
    }
    double length() const {
        return std::sqrt(x * x + y * y);
    }
    double size() const { return length(); }
    Vector2D clamp(const Vector2D& min, const Vector2D& max = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()}) const {
        return Vector2D(std::clamp(x, min.x, max.x), std::clamp(y, min.y, max.y));
    }
    Vector2D floor() const { return Vector2D(std::floor(x), std::floor(y)); }
    Vector2D round() const { return Vector2D(std::round(x), std::round(y)); }
};

}
}

using Vector2D = Hyprutils::Math::Vector2D;

template <>
struct std::formatter<Hyprutils::Math::Vector2D> {
    constexpr auto parse(std::format_parse_context& ctx) {
        auto it = ctx.begin();
        while (it != ctx.end() && *it != '}') ++it;
        return it;
    }
    auto format(const Hyprutils::Math::Vector2D& vec, auto& ctx) const {
        return std::format_to(ctx.out(), "[{}, {}]", vec.x, vec.y);
    }
};

#endif /* HYPRUTILS_MATH_VECTOR2D_HPP */
