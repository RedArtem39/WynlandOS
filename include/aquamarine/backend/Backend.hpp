#ifndef AQUAMARINE_BACKEND_BACKEND_HPP
#define AQUAMARINE_BACKEND_BACKEND_HPP

#include "Misc.hpp"
#include <vector>
#include <string>
#include <memory>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/cli/Logger.hpp>
#include <aquamarine/input/Input.hpp>
#include <aquamarine/output/Output.hpp>

namespace Aquamarine {

class IBackendImplementation {
public:
    enum eBackendCapabilities {
        AQ_BACKEND_CAPABILITY_POINTER = 1
    };
    virtual int type() { return 0; }
    virtual ~IBackendImplementation() = default;
};

struct SBackendOptions {
    std::string socketName;
    bool logToStdout = false;
    std::string waylandDisplay;
    SP<Hyprutils::CLI::CLoggerConnection> logConnection;
};

struct SBackendEvents {
    CSignal newOutput;
    CSignal newPointer;
    CSignal newKeyboard;
    CSignal newTouch;
    CSignal newTablet;
    CSignal newTabletPad;
    CSignal newSwitch;
    CSignal destroy;
    CSignal pointerMotion;
    CSignal pointerMotionAbsolute;
    CSignal pointerButton;
    CSignal pointerAxis;
    CSignal keyboardKey;
    CSignal keyboardModifiers;
    CSignal touchDown;
    CSignal touchUp;
    CSignal touchMotion;
    CSignal touchCancel;
    CSignal touchFrame;
    CSignal swipeBegin;
    CSignal swipeUpdate;
    CSignal swipeEnd;
    CSignal pinchBegin;
    CSignal pinchUpdate;
    CSignal pinchEnd;
};

struct SSessionEvents {
    CSignal changeActive;
};

class CSession {
public:
    bool active = true;
    SSessionEvents events;
    bool switchVT(int vt) { return true; }
};

class IBackend {
public:
    std::vector<SP<IOutput>> m_vOutputs;
    SBackendEvents events;
    SP<CSession> session = makeShared<CSession>();

    bool hasSession() { return true; }
    int drmFD() { return -1; }
    int drmRenderNodeFD() { return -1; }
    bool createOutput(const std::string& name = "") { return true; }
    std::vector<SP<IBackendImplementation>> getImplementations() { return {}; }
    int type() { return 0; }

    static SP<IBackend> create(std::vector<eBackendType> backends, SBackendOptions options) {
        return makeShared<IBackend>();
    }

    static SP<IBackend> create(std::vector<SBackendImplementationOptions> backends, SBackendOptions options) {
        return makeShared<IBackend>();
    }
    virtual bool start() { return true; }
    virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators() { return {}; }
    virtual Hyprutils::Memory::CSharedPointer<IAllocator> preferredAllocator() { return nullptr; }
    virtual Hyprutils::Memory::CWeakPointer<IBackend> getPrimary() { return Hyprutils::Memory::CWeakPointer<IBackend>(); }
    virtual uint32_t capabilities() { return IBackendImplementation::eBackendCapabilities::AQ_BACKEND_CAPABILITY_POINTER; }

    std::vector<SP<IOutput>> getOutputs() { return m_vOutputs; }

    virtual ~IBackend() = default;
};

}

#endif /* AQUAMARINE_BACKEND_BACKEND_HPP */
