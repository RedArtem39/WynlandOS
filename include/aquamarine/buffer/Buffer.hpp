#ifndef AQUAMARINE_BUFFER_BUFFER_HPP
#define AQUAMARINE_BUFFER_BUFFER_HPP

#include <stdint.h>
#include <tuple>
#include <hyprutils/math/Vector2D.hpp>

namespace Aquamarine {

enum eBufferCapability : uint32_t {
    AQ_BUFFER_CAPABILITY_NONE = 0,
    AQ_BUFFER_CAPABILITY_SHM = (1 << 0),
    AQ_BUFFER_CAPABILITY_DMABUF = (1 << 1),
    BUFFER_CAPABILITY_DATAPTR = (1 << 0)
};

enum eBufferType : uint8_t {
    AQ_BUFFER_TYPE_SHM = 0,
    AQ_BUFFER_TYPE_DMABUF = 1,
    BUFFER_TYPE_SHM = 0
};

struct SDMABUFAttrs {
    bool success = true;
    int32_t  width = 0;
    int32_t  height = 0;
    uint32_t format = 0;
    uint64_t modifier = 0;
    int      fds[4] = {-1, -1, -1, -1};
    uint32_t strides[4] = {0, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    int      planes = 1;
    Hyprutils::Math::Vector2D size;
};

struct SSHMAttrs {
    bool success = true;
    uint32_t format = 0;
    int32_t  width = 0;
    int32_t  height = 0;
    int32_t  stride = 0;
    void*    data = nullptr;
    Hyprutils::Math::Vector2D size;
};

class IBuffer {
public:
    virtual ~IBuffer() = default;
    Hyprutils::Math::Vector2D size = {0, 0};
    virtual eBufferCapability caps() { return AQ_BUFFER_CAPABILITY_SHM; }
    virtual eBufferType type() { return AQ_BUFFER_TYPE_SHM; }
    virtual SDMABUFAttrs dmabuf() { return SDMABUFAttrs(); }
    virtual SSHMAttrs shm() { return SSHMAttrs(); }
    virtual void sendRelease() {}
    virtual std::tuple<void*, uint32_t, size_t> beginDataPtr(uint32_t flags) { return {nullptr, 0, 0}; }
    virtual void endDataPtr() {}
};

}

#endif /* AQUAMARINE_BUFFER_BUFFER_HPP */
