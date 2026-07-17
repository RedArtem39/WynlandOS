#ifndef HYPRUTILS_MEMORY_ATOMIC_HPP
#define HYPRUTILS_MEMORY_ATOMIC_HPP

#include "SharedPtr.hpp"

namespace Hyprutils {
namespace Memory {

template <typename T>
using CAtomicSharedPointer = CSharedPointer<T>;

}
}

template <typename T>
using ASP = Hyprutils::Memory::CAtomicSharedPointer<T>;

#endif /* HYPRUTILS_MEMORY_ATOMIC_HPP */
