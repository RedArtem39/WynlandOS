#ifndef HYPRUTILS_MEMORY_UNIQUE_PTR_HPP
#define HYPRUTILS_MEMORY_UNIQUE_PTR_HPP

#include <memory>
#include <utility>

namespace Hyprutils {
namespace Memory {

template <typename T>
class CUniquePointer : public std::shared_ptr<T> {
public:
    CUniquePointer(T* ptr) : std::shared_ptr<T>(ptr) {}
    CUniquePointer() : std::shared_ptr<T>() {}
    CUniquePointer(const std::shared_ptr<T>& ptr) : std::shared_ptr<T>(ptr) {}
    CUniquePointer(std::shared_ptr<T>&& ptr) : std::shared_ptr<T>(std::move(ptr)) {}
    template<typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    CUniquePointer(CUniquePointer<U>&& other) : std::shared_ptr<T>(std::move(other)) {}
    template<typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    CUniquePointer(const CUniquePointer<U>& other) : std::shared_ptr<T>(other) {}
    template<typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    CUniquePointer& operator=(CUniquePointer<U>&& other) {
        std::shared_ptr<T>::operator=(std::move(other));
        return *this;
    }
};

template <typename T, typename... Args>
CUniquePointer<T> makeUnique(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T>
using UP = CUniquePointer<T>;

}
}

#endif /* HYPRUTILS_MEMORY_UNIQUE_PTR_HPP */
