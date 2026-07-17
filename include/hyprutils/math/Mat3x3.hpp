#ifndef HYPRUTILS_MATH_MAT3X3_HPP
#define HYPRUTILS_MATH_MAT3X3_HPP

#include "Vector2D.hpp"
#include "Box.hpp"
#include <array>

namespace Hyprutils {
namespace Math {

class CMat3x3 {
public:
    std::array<float, 9> m_mat = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    CMat3x3() {}
    CMat3x3(const std::array<float, 9>& mat) : m_mat(mat) {}

    static CMat3x3 identity() {
        return CMat3x3();
    }

    CMat3x3& translate(const Vector2D& offset) {
        m_mat[2] += offset.x;
        m_mat[5] += offset.y;
        return *this;
    }

    CMat3x3& scale(const Vector2D& scale) {
        m_mat[0] *= scale.x;
        m_mat[4] *= scale.y;
        return *this;
    }

    CMat3x3& rotate(float angleRad) {
        float c = std::cos(angleRad);
        float s = std::sin(angleRad);
        m_mat[0] = c;
        m_mat[1] = -s;
        m_mat[3] = s;
        m_mat[4] = c;
        return *this;
    }

    CMat3x3& transform(int tr) { return *this; }
    
    static CMat3x3 outputProjection(const Vector2D& pixelSize, int transform) {
        return CMat3x3();
    }

    CMat3x3 operator*(const CMat3x3& other) const {
        CMat3x3 res;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                res.m_mat[i * 3 + j] = 0;
                for (int k = 0; k < 3; ++k) {
                    res.m_mat[i * 3 + j] += m_mat[i * 3 + k] * other.m_mat[k * 3 + j];
                }
            }
        }
        return res;
    }

    CMat3x3& multiply(const CMat3x3& other) {
        *this = *this * other;
        return *this;
    }

    const std::array<float, 9>& getMatrix() const { return m_mat; }

    template <typename T>
    T operator*(const T& vec) const {
        return T{
            m_mat[0] * vec.x + m_mat[1] * vec.y + m_mat[2] * vec.z,
            m_mat[3] * vec.x + m_mat[4] * vec.y + m_mat[5] * vec.z,
            m_mat[6] * vec.x + m_mat[7] * vec.y + m_mat[8] * vec.z
        };
    }

    Vector2D transform(const Vector2D& pt) const {
        float x = m_mat[0] * pt.x + m_mat[1] * pt.y + m_mat[2];
        float y = m_mat[3] * pt.x + m_mat[4] * pt.y + m_mat[5];
        return Vector2D(x, y);
    }

    CBox transform(const CBox& box) const {
        Vector2D p1 = transform(Vector2D(box.x, box.y));
        Vector2D p2 = transform(Vector2D(box.x + box.w, box.y + box.h));
        return CBox(std::min(p1.x, p2.x), std::min(p1.y, p2.y), std::abs(p2.x - p1.x), std::abs(p2.y - p1.y));
    }
};

using Mat3x3 = CMat3x3;

}
}

using CMat3x3 = Hyprutils::Math::CMat3x3;
using Mat3x3 = Hyprutils::Math::CMat3x3;

#endif /* HYPRUTILS_MATH_MAT3X3_HPP */
