#ifndef AQUAMARINE_BACKEND_MISC_HPP
#define AQUAMARINE_BACKEND_MISC_HPP

#include <stdint.h>
#include <vector>

namespace Aquamarine {

enum eBackendType : uint8_t {
    AQ_BACKEND_DRM      = 0,
    AQ_BACKEND_WAYLAND  = 1,
    AQ_BACKEND_X11      = 2,
    AQ_BACKEND_HEADLESS = 3,
    AQ_BACKEND_NULL     = 4
};

enum eBackendRequestMode : uint8_t {
    AQ_BACKEND_REQUEST_OPTIONAL     = 0,
    AQ_BACKEND_REQUEST_IF_AVAILABLE = 1,
    AQ_BACKEND_REQUEST_MANDATORY    = 2,
    AQ_BACKEND_REQUEST_FALLBACK     = 3
};

struct SBackendImplementationOptions {
    eBackendType        backendType        = AQ_BACKEND_HEADLESS;
    eBackendRequestMode backendRequestMode = AQ_BACKEND_REQUEST_OPTIONAL;
};

struct SDevice {
    int fd = -1;
};

struct SDRMFormat {
    uint32_t drmFormat = 0;
    uint64_t modifier = 0;
};

}

#endif /* AQUAMARINE_BACKEND_MISC_HPP */
