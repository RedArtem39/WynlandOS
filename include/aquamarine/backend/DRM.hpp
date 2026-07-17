#ifndef AQUAMARINE_BACKEND_DRM_HPP
#define AQUAMARINE_BACKEND_DRM_HPP

#include "Backend.hpp"
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

namespace Aquamarine {

class CDRMLease;

class CDRMBackend : public IBackendImplementation {
public:
    CDRMBackend() {}
    std::string gpuName = "";
    WP<CDRMBackend> self;
    Hyprutils::OS::CFileDescriptor getNonMasterFD() { return Hyprutils::OS::CFileDescriptor{}; }
};

class CDRMLease {
public:
    int leaseFD = -1;
    struct {
        CSignal destroy;
    } events;
    static SP<CDRMLease> create(std::vector<SP<IOutput>> outputs) { return nullptr; }
};

class CDRMOutput : public IOutput {
public:
    CDRMOutput() {}
    WP<CDRMOutput> self;
    SP<CDRMLease> lease;
    uint32_t getConnectorID() { return 0; }
};

}

#endif /* AQUAMARINE_BACKEND_DRM_HPP */
