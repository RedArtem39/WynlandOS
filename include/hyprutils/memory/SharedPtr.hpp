#ifndef HYPRUTILS_MEMORY_SHARED_PTR_HPP
#define HYPRUTILS_MEMORY_SHARED_PTR_HPP

#include <memory>
#include <functional>

namespace Hyprutils {
namespace Memory {

template <typename T>
class CWeakPointer;

template <typename T>
class CSharedPointer : public std::shared_ptr<T> {
public:
    CSharedPointer() : std::shared_ptr<T>() {}
    CSharedPointer(std::nullptr_t) : std::shared_ptr<T>() {}
    CSharedPointer(T* ptr) : std::shared_ptr<T>(ptr) {}
    CSharedPointer(const std::shared_ptr<T>& sp) : std::shared_ptr<T>(sp) {}
    CSharedPointer(std::shared_ptr<T>&& sp) : std::shared_ptr<T>(std::move(sp)) {}

    template <typename U>
    CSharedPointer(const std::shared_ptr<U>& sp) : std::shared_ptr<T>(sp) {}
    template <typename U>
    CSharedPointer(std::shared_ptr<U>&& sp) : std::shared_ptr<T>(std::move(sp)) {}
    template <typename U>
    CSharedPointer(const CSharedPointer<U>& sp) : std::shared_ptr<T>(sp) {}

    CSharedPointer& operator=(std::nullptr_t) {
        this->reset();
        return *this;
    }
    template <typename U>
    CSharedPointer& operator=(const CSharedPointer<U>& rhs) {
        std::shared_ptr<T>::operator=(rhs);
        return *this;
    }
    template <typename U>
    CSharedPointer& operator=(const std::shared_ptr<U>& rhs) {
        std::shared_ptr<T>::operator=(rhs);
        return *this;
    }
    template <typename U>
    CSharedPointer& operator=(std::shared_ptr<U>&& rhs) {
        std::shared_ptr<T>::operator=(std::move(rhs));
        return *this;
    }

    long strongRef() const { return this->use_count(); }

    bool operator==(const CSharedPointer<T>& rhs) const { return this->get() == rhs.get(); }
    bool operator!=(const CSharedPointer<T>& rhs) const { return this->get() != rhs.get(); }
    template <typename U>
    bool operator==(const CSharedPointer<U>& rhs) const { return (void*)this->get() == (void*)rhs.get(); }
    template <typename U>
    bool operator!=(const CSharedPointer<U>& rhs) const { return (void*)this->get() != (void*)rhs.get(); }
    bool operator==(std::nullptr_t) const { return this->get() == nullptr; }
    bool operator!=(std::nullptr_t) const { return this->get() != nullptr; }
    template <typename U>
    bool operator==(const std::shared_ptr<U>& rhs) const { return (void*)this->get() == (void*)rhs.get(); }
    template <typename U>
    bool operator!=(const std::shared_ptr<U>& rhs) const { return (void*)this->get() != (void*)rhs.get(); }
    template <typename U>
    bool operator==(const CWeakPointer<U>& rhs) const { return (void*)this->get() == (void*)rhs.get(); }
    template <typename U>
    bool operator!=(const CWeakPointer<U>& rhs) const { return (void*)this->get() != (void*)rhs.get(); }
    template <typename U>
    bool operator==(const U* rhs) const { return (void*)this->get() == (void*)rhs; }
    template <typename U>
    bool operator!=(const U* rhs) const { return (void*)this->get() != (void*)rhs; }
};

template <typename T, typename... Args>
CSharedPointer<T> makeShared(Args&&... args) {
    return CSharedPointer<T>(std::make_shared<T>(std::forward<Args>(args)...));
}

template <typename T>
using SP = CSharedPointer<T>;

template <typename T, typename U>
CSharedPointer<T> reinterpretPointerCast(const CSharedPointer<U>& sp) {
    return CSharedPointer<T>(std::reinterpret_pointer_cast<T>(sp));
}

template <typename T, typename U>
CSharedPointer<T> staticPointerCast(const CSharedPointer<U>& sp) {
    return CSharedPointer<T>(std::static_pointer_cast<T>(sp));
}

template <typename T, typename U>
CSharedPointer<T> dynamicPointerCast(const CSharedPointer<U>& sp) {
    return CSharedPointer<T>(std::dynamic_pointer_cast<T>(sp));
}

}
}

namespace std {
template <typename T>
struct hash<Hyprutils::Memory::CSharedPointer<T>> {
    size_t operator()(const Hyprutils::Memory::CSharedPointer<T>& sp) const noexcept {
        return std::hash<void*>{}((void*)sp.get());
    }
};
}

#include "UniquePtr.hpp"
#include "WeakPtr.hpp"

#ifndef HYPRUTILS_MEMORY_SP_ALIAS
#define HYPRUTILS_MEMORY_SP_ALIAS
template <typename T>
using SP = Hyprutils::Memory::CSharedPointer<T>;
#endif

using Hyprutils::Memory::makeShared;
using Hyprutils::Memory::makeUnique;
using Hyprutils::Memory::reinterpretPointerCast;
using Hyprutils::Memory::staticPointerCast;
using Hyprutils::Memory::dynamicPointerCast;

#endif /* HYPRUTILS_MEMORY_SHARED_PTR_HPP */
