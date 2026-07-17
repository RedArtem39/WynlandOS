#ifndef HYPRUTILS_MEMORY_WEAK_PTR_HPP
#define HYPRUTILS_MEMORY_WEAK_PTR_HPP

#include <memory>
#include <functional>
#include "SharedPtr.hpp"

namespace Hyprutils {
namespace Memory {

template <typename T>
class CWeakPointer : public std::weak_ptr<T> {
public:
    CWeakPointer() : std::weak_ptr<T>() {}
    CWeakPointer(std::nullptr_t) : std::weak_ptr<T>() {}
    CWeakPointer(const std::weak_ptr<T>& wp) : std::weak_ptr<T>(wp) {}
    CWeakPointer(const std::shared_ptr<T>& sp) : std::weak_ptr<T>(sp) {}
    CWeakPointer(const CSharedPointer<T>& sp) : std::weak_ptr<T>(sp) {}

    template <typename U>
    CWeakPointer(const std::shared_ptr<U>& sp) : std::weak_ptr<T>(sp) {}
    template <typename U>
    CWeakPointer(const std::weak_ptr<U>& wp) : std::weak_ptr<T>(wp) {}
    template <typename U>
    CWeakPointer(const CWeakPointer<U>& wp) : std::weak_ptr<T>(wp) {}
    template <typename U>
    CWeakPointer(const CSharedPointer<U>& sp) : std::weak_ptr<T>(sp) {}

    CWeakPointer& operator=(std::nullptr_t) {
        this->reset();
        return *this;
    }
    template <typename U>
    CWeakPointer& operator=(const CSharedPointer<U>& rhs) {
        std::weak_ptr<T>::operator=(rhs);
        return *this;
    }
    template <typename U>
    CWeakPointer& operator=(const std::shared_ptr<U>& rhs) {
        std::weak_ptr<T>::operator=(rhs);
        return *this;
    }
    template <typename U>
    CWeakPointer& operator=(const CWeakPointer<U>& rhs) {
        std::weak_ptr<T>::operator=(rhs);
        return *this;
    }
    template <typename U>
    CWeakPointer& operator=(const std::weak_ptr<U>& rhs) {
        std::weak_ptr<T>::operator=(rhs);
        return *this;
    }

    CSharedPointer<T> lock() const noexcept {
        return CSharedPointer<T>(std::weak_ptr<T>::lock());
    }

    T* get() const {
        return this->lock().get();
    }

    bool valid() const {
        return !this->expired() && this->lock() != nullptr;
    }

    explicit operator bool() const {
        return !this->expired() && this->lock() != nullptr;
    }

    T* operator->() const {
        return this->lock().get();
    }

    T& operator*() const {
        return *this->lock();
    }

    template <typename U>
    bool operator==(const CWeakPointer<U>& rhs) const {
        return (void*)this->lock().get() == (void*)rhs.lock().get();
    }
    template <typename U>
    bool operator!=(const CWeakPointer<U>& rhs) const {
        return (void*)this->lock().get() != (void*)rhs.lock().get();
    }

    template <typename U>
    bool operator==(const CSharedPointer<U>& rhs) const {
        return (void*)this->lock().get() == (void*)rhs.get();
    }
    template <typename U>
    bool operator!=(const CSharedPointer<U>& rhs) const {
        return (void*)this->lock().get() != (void*)rhs.get();
    }

    template <typename U>
    bool operator==(const std::shared_ptr<U>& rhs) const {
        return (void*)this->lock().get() == (void*)rhs.get();
    }
    template <typename U>
    bool operator!=(const std::shared_ptr<U>& rhs) const {
        return (void*)this->lock().get() != (void*)rhs.get();
    }

    template <typename U>
    bool operator==(const U* rhs) const {
        return (void*)this->lock().get() == (void*)rhs;
    }
    template <typename U>
    bool operator!=(const U* rhs) const {
        return (void*)this->lock().get() != (void*)rhs;
    }

    bool operator==(std::nullptr_t) const {
        return this->lock().get() == nullptr;
    }
    bool operator!=(std::nullptr_t) const {
        return this->lock().get() != nullptr;
    }

    template <typename U>
    bool operator<(const CWeakPointer<U>& rhs) const {
        return this->owner_before(rhs);
    }
};

template <typename T>
using WP = CWeakPointer<T>;

}
}

namespace std {
template <typename T>
struct hash<Hyprutils::Memory::CWeakPointer<T>> {
    size_t operator()(const Hyprutils::Memory::CWeakPointer<T>& wp) const noexcept {
        return std::hash<void*>{}((void*)wp.get());
    }
};
}

#endif /* HYPRUTILS_MEMORY_WEAK_PTR_HPP */
