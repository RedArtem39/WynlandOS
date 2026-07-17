#ifndef HYPRUTILS_UTILS_SCOPE_GUARD_HPP
#define HYPRUTILS_UTILS_SCOPE_GUARD_HPP

#include <functional>
#include <utility>

namespace Hyprutils {
namespace Utils {

class CScopeGuard {
public:
    CScopeGuard(std::function<void()> fn) : m_fn(std::move(fn)) {}
    ~CScopeGuard() { if (m_active && m_fn) m_fn(); }

    CScopeGuard(const CScopeGuard&) = delete;
    CScopeGuard& operator=(const CScopeGuard&) = delete;

    CScopeGuard(CScopeGuard&& other) noexcept : m_fn(std::move(other.m_fn)), m_active(other.m_active) {
        other.m_active = false;
    }

    void dismiss() { m_active = false; }

private:
    std::function<void()> m_fn;
    bool m_active = true;
};

}
}

using CScopeGuard = Hyprutils::Utils::CScopeGuard;

#endif /* HYPRUTILS_UTILS_SCOPE_GUARD_HPP */
