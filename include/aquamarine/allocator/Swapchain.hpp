#ifndef AQUAMARINE_ALLOCATOR_SWAPCHAIN_HPP
#define AQUAMARINE_ALLOCATOR_SWAPCHAIN_HPP

#include "../buffer/Buffer.hpp"
#include <hyprutils/memory/SharedPtr.hpp>

namespace Aquamarine {

class IBackend;

enum eAllocatorType {
    AQ_ALLOCATOR_TYPE_DUMMY = 0,
    AQ_ALLOCATOR_TYPE_GBM = 1,
    AQ_ALLOCATOR_TYPE_DRM_DUMB = 2
};

class IAllocator {
public:
    virtual ~IAllocator() = default;
    virtual eAllocatorType type() { return AQ_ALLOCATOR_TYPE_DUMMY; }
};

struct SSwapchainOptions {
    uint32_t format = 0;
    bool scanout = false;
    int length = 0;
    Hyprutils::Math::Vector2D size;
    bool cursor = false;
    bool multigpu = false;
};

class CSwapchain {
public:
    virtual ~CSwapchain() = default;
    virtual Hyprutils::Memory::CSharedPointer<IBuffer> next(void*) { return nullptr; }
    virtual Hyprutils::Memory::CSharedPointer<IBuffer> current() { return nullptr; }
    virtual Hyprutils::Memory::CSharedPointer<IAllocator> getAllocator() { return nullptr; }
    virtual void rollback() {}
    virtual SSwapchainOptions currentOptions() { return {}; }
    virtual bool reconfigure(const SSwapchainOptions& opts) { return true; }
    static Hyprutils::Memory::CSharedPointer<CSwapchain> create(Hyprutils::Memory::CSharedPointer<IAllocator> allocator, Hyprutils::Memory::CSharedPointer<IBackend> backend) { return nullptr; }
};

}

#endif /* AQUAMARINE_ALLOCATOR_SWAPCHAIN_HPP */
