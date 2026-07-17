#ifndef HYPRUTILS_MATH_EDGES_HPP
#define HYPRUTILS_MATH_EDGES_HPP

#include <stdint.h>

namespace Hyprutils {
namespace Math {

enum eEdge : uint8_t {
    EDGE_NONE   = 0,
    EDGE_TOP    = 1 << 0,
    EDGE_BOTTOM = 1 << 1,
    EDGE_LEFT   = 1 << 2,
    EDGE_RIGHT  = 1 << 3
};

enum eRectCorner : uint8_t {
    CORNER_NONE         = 0,
    CORNER_TOPLEFT      = 1,
    CORNER_TOPRIGHT     = 2,
    CORNER_BOTTOMRIGHT  = 3,
    CORNER_BOTTOMLEFT   = 4
};

class CEdges {
public:
    static constexpr uint32_t NONE   = 0;
    static constexpr uint32_t TOP    = 1 << 0;
    static constexpr uint32_t BOTTOM = 1 << 1;
    static constexpr uint32_t LEFT   = 1 << 2;
    static constexpr uint32_t RIGHT  = 1 << 3;

    CEdges(uint32_t edges = 0) : m_edges(edges) {}

    bool left() const { return m_edges & LEFT; }
    bool right() const { return m_edges & RIGHT; }
    bool top() const { return m_edges & TOP; }
    bool bottom() const { return m_edges & BOTTOM; }

    CEdges operator^(const CEdges& other) const { return CEdges(m_edges ^ other.m_edges); }
    CEdges operator^(uint32_t other) const { return CEdges(m_edges ^ other); }
    CEdges operator&(const CEdges& other) const { return CEdges(m_edges & other.m_edges); }
    CEdges operator|(const CEdges& other) const { return CEdges(m_edges | other.m_edges); }

    uint32_t m_edges = 0;
};

}
}

using eEdge = Hyprutils::Math::eEdge;
// Note: eRectCorner not aliased globally to avoid conflicts with Layout::eRectCorner
// Use Hyprutils::Math::eRectCorner to access the Hyprutils version
using CEdges = Hyprutils::Math::CEdges;
using namespace Hyprutils::Math;

#endif /* HYPRUTILS_MATH_EDGES_HPP */
