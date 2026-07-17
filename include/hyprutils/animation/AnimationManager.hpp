#ifndef HYPRUTILS_ANIMATION_ANIMATION_MANAGER_HPP
#define HYPRUTILS_ANIMATION_ANIMATION_MANAGER_HPP

#include "AnimatedVariable.hpp"
#include <vector>

namespace Hyprutils {
namespace Animation {

struct SSpringCurve {
    float stiffness = 100.0f;
    float damping = 10.0f;
    float mass = 1.0f;
};

class CAnimationManager {
public:
    std::vector<Hyprutils::Memory::CWeakPointer<CBaseAnimatedVariable>> m_vActiveAnimatedVariables;
    void tick() {}
    void tickDone() {}
    bool shouldTickForNext() const { return !m_vActiveAnimatedVariables.empty(); }
    template <typename V>
    void addBezierWithName(const std::string& name, const V& p1, const V& p2) {}
    template <typename C>
    void addSpringWithName(const std::string& name, const C& curve) {}
    bool bezierExists(const std::string& name) const { return true; }
    bool springExists(const std::string& name) const { return true; }
    void removeAllBeziers() {}
    void removeAllSprings() {}
};

}
}

using Hyprutils::Animation::CAnimationManager;

#endif /* HYPRUTILS_ANIMATION_ANIMATION_MANAGER_HPP */
