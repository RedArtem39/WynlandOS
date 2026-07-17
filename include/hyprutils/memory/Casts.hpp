#ifndef HYPRUTILS_MEMORY_CASTS_HPP
#define HYPRUTILS_MEMORY_CASTS_HPP

#include <memory>

namespace Hyprutils {
namespace Memory {

template <typename To, typename From>
std::shared_ptr<To> dynamicPointerCast(const std::shared_ptr<From>& ptr) {
    return std::dynamic_pointer_cast<To>(ptr);
}

template <typename To, typename From>
std::shared_ptr<To> staticPointerCast(const std::shared_ptr<From>& ptr) {
    return std::static_pointer_cast<To>(ptr);
}

template <typename To, typename From>
constexpr To rc(From v) {
    return reinterpret_cast<To>(v);
}

template <typename To, typename From>
constexpr To sc(From v) {
    return static_cast<To>(v);
}

template <typename To, typename From>
constexpr To cc(From v) {
    return const_cast<To>(v);
}

template <typename To, typename From>
To dc(From v) {
    return dynamic_cast<To>(v);
}

}
}

using Hyprutils::Memory::rc;
using Hyprutils::Memory::sc;
using Hyprutils::Memory::cc;
using Hyprutils::Memory::dc;

#endif /* HYPRUTILS_MEMORY_CASTS_HPP */
