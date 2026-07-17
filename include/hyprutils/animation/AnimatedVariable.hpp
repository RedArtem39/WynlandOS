#ifndef HYPRUTILS_ANIMATION_ANIMATED_VARIABLE_HPP
#define HYPRUTILS_ANIMATION_ANIMATED_VARIABLE_HPP

#include <string>
#include <memory>
#include <unordered_map>
#include <any>
#include <functional>
#include <hyprutils/memory/SharedPtr.hpp>

using Hyprutils::Memory::makeShared;
using Hyprutils::Memory::makeUnique;
using Hyprutils::Memory::CSharedPointer;

#ifndef HYPRUTILS_MEMORY_SP_ALIAS
#define HYPRUTILS_MEMORY_SP_ALIAS
template <typename T>
using SP = Hyprutils::Memory::CSharedPointer<T>;
#endif

#ifndef HYPRUTILS_MEMORY_WP_ALIAS
#define HYPRUTILS_MEMORY_WP_ALIAS
template <typename T>
using WP = Hyprutils::Memory::CWeakPointer<T>;
#endif

#ifndef HYPRUTILS_MEMORY_UP_ALIAS
#define HYPRUTILS_MEMORY_UP_ALIAS
template <typename T>
using UP = Hyprutils::Memory::CUniquePointer<T>;
#endif

namespace Hyprutils {
namespace Animation {

class SAnimationPropertyConfig {
public:
    std::string pValues = "default";
    bool        m_bEnabled = true;
    float       m_fSpeed = 8.f;
    std::string m_szBezier = "default";
    std::string m_szStyle = "";
};

class CAnimationConfigTree {
public:
    std::unordered_map<std::string, SP<SAnimationPropertyConfig>> m_mConfigs;

    void createNode(const std::string& name, const std::string& parent = "") {
        if (!m_mConfigs.contains(name))
            m_mConfigs[name] = makeShared<SAnimationPropertyConfig>();
    }

    void setConfigForNode(const std::string& name, bool enabled, float speed, const std::string& bezier, const std::string& style = "") {
        if (!m_mConfigs.contains(name))
            m_mConfigs[name] = makeShared<SAnimationPropertyConfig>();
        auto cfg = m_mConfigs[name];
        cfg->m_bEnabled = enabled;
        cfg->m_fSpeed = speed;
        cfg->m_szBezier = bezier;
        cfg->m_szStyle = style;
    }

    const std::unordered_map<std::string, SP<SAnimationPropertyConfig>>& getFullConfig() {
        return m_mConfigs;
    }

    SP<SAnimationPropertyConfig> getConfig(const std::string& name) {
        if (!m_mConfigs.contains(name))
            m_mConfigs[name] = makeShared<SAnimationPropertyConfig>();
        return m_mConfigs[name];
    }

    bool nodeExists(const std::string& name) {
        return m_mConfigs.contains(name);
    }
};

struct SAnimationCurveStep {
    float value = 1.0f;
    bool finished = true;
};

class CBaseAnimatedVariable {
public:
    int         m_Type = -1;
    bool        m_bIsAnimating = false;
    std::string m_szName = "AV";
    SP<SAnimationPropertyConfig> m_pConfig;

    CBaseAnimatedVariable() {}
    virtual ~CBaseAnimatedVariable() = default;

    virtual void setConfig(SAnimationPropertyConfig* pConfig) {}
    virtual void setConfig(SP<SAnimationPropertyConfig> pConfig) { m_pConfig = pConfig; }
    virtual void unregister() {}
    virtual void registerVar() {}
    virtual bool isBeingAnimated() const { return m_bIsAnimating; }
    virtual bool enabled() const { return m_bIsAnimating; }
    virtual void warp(bool force = false, bool callback = false) {}
    virtual void onUpdate() {}

    virtual std::string getStyle() const { return m_pConfig ? m_pConfig->m_szStyle : ""; }
    virtual float getSpeed() const { return m_pConfig ? m_pConfig->m_fSpeed : 8.f; }
    virtual std::string getBezier() const { return m_pConfig ? m_pConfig->m_szBezier : "default"; }
};

template <typename T, typename Context = void>
class CGenericAnimatedVariable : public CBaseAnimatedVariable {
public:
    T m_vValue{};
    T m_vGoal{};
    T m_vBegun{};
    std::conditional_t<std::is_void_v<Context>, char, Context> m_Context{};
    std::function<void(void*)> m_updateCallback;

    CGenericAnimatedVariable() {
        if constexpr (std::is_same_v<T, float>) m_Type = 0;
        else if constexpr (sizeof(T) == sizeof(float) * 2) m_Type = 1;
        else m_Type = 2;
    }
    virtual ~CGenericAnimatedVariable() = default;

    virtual void setConfig(SAnimationPropertyConfig* pConfig) override {}
    virtual void setConfig(SP<SAnimationPropertyConfig> pConfig) override { m_pConfig = pConfig; }
    virtual void unregister() override {}
    virtual void registerVar() override {}
    virtual bool isBeingAnimated() const override { return m_bIsAnimating; }
    virtual bool enabled() const override { return m_bIsAnimating; }
    virtual void warp(bool force = false, bool callback = false) override { m_vValue = m_vGoal; m_vBegun = m_vGoal; }
    virtual void onUpdate() override { if (m_updateCallback) m_updateCallback(this); }

    template <typename... Args>
    void create(Args&&... args) {}

    template <typename... Args>
    void create2(Args&&... args) {}

    SAnimationCurveStep getCurveStep() {
        return SAnimationCurveStep{1.0f, m_vValue == m_vGoal};
    }

    virtual std::string getStyle() const override { return m_pConfig ? m_pConfig->m_szStyle : ""; }

    template <typename F>
    void setUpdateCallback(F&& cb) {
        m_updateCallback = [cb = std::forward<F>(cb)](void* ptr) {
            cb(ptr);
        };
    }

    template <typename F>
    CGenericAnimatedVariable* setCallbackOnEnd(F&& cb, bool something = false) {
        return this;
    }

    template <typename F>
    CGenericAnimatedVariable* setCallbackOnBegin(F&& cb, bool something = false) {
        return this;
    }

    void resetAllCallbacks() {
    }

    void setValueAndWarp(const T& val) {
        m_vValue = val;
        m_vGoal = val;
        m_vBegun = val;
    }

    void setValue(const T& val) {
        m_vGoal = val;
    }

    T& value() { return m_vValue; }
    const T& value() const { return m_vValue; }
    T& goal() { return m_vGoal; }
    const T& goal() const { return m_vGoal; }
    T& begun() { return m_vBegun; }
    const T& begun() const { return m_vBegun; }

    T& operator*() { return m_vGoal; }
    const T& operator*() const { return m_vGoal; }

    CGenericAnimatedVariable<T, Context>& operator=(const T& v) {
        m_vGoal = v;
        return *this;
    }

    operator T() const { return m_vValue; }
};

template <typename T>
using CAnimatedVariable = CGenericAnimatedVariable<T, void>;

template <typename T>
class CAnimatedVariableVector : public CAnimatedVariable<T> {
public:
    CAnimatedVariableVector() {}
    virtual ~CAnimatedVariableVector() = default;
};

}
}

using Hyprutils::Animation::CGenericAnimatedVariable;
using Hyprutils::Animation::CAnimatedVariableVector;
using Hyprutils::Animation::CBaseAnimatedVariable;
using Hyprutils::Animation::SAnimationPropertyConfig;
using Hyprutils::Animation::CAnimationConfigTree;

#endif /* HYPRUTILS_ANIMATION_ANIMATED_VARIABLE_HPP */
