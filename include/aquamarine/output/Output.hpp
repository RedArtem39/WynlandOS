#ifndef AQUAMARINE_OUTPUT_OUTPUT_HPP
#define AQUAMARINE_OUTPUT_OUTPUT_HPP

#include <string>
#include <vector>
#include <optional>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include <xf86drmMode.h>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/allocator/Swapchain.hpp>

#include "../backend/Misc.hpp"

namespace Aquamarine {

class IBackend;

enum eOutputState : uint8_t {
    AQ_OUTPUT_STATE_ENABLED = 0,
    AQ_OUTPUT_STATE_DISABLED = 1
};

enum eOutputPresentationMode {
    AQ_OUTPUT_PRESENTATION_IMMEDIATE,
    AQ_OUTPUT_PRESENTATION_VSYNC
};



struct SOutputMode {
    Hyprutils::Math::Vector2D pixelSize = {1920, 1080};
    int32_t width = 1920;
    int32_t height = 1080;
    float refreshRate = 60000.0f;
    bool preferred = true;
    std::optional<drmModeModeInfo> modeInfo;
};

struct SHDRMetadata {
    uint32_t desiredContentMaxLuminance = 10000;
    uint32_t desiredContentMinLuminance = 1;
    uint32_t desiredMaxFrameAverageLuminance = 10000;
    uint32_t maxLuminance = 10000;
    bool supportsPQ = false;
    struct {
        int eotf = 0;
    } hdmi_metadata_type1;
};

struct SChromaticityPoint { double x, y; };
struct SChromaticityCoords {
    SChromaticityPoint red, green, blue, white;
};

struct SEDID {
    std::optional<SHDRMetadata> hdrMetadata = SHDRMetadata{};
    std::optional<SChromaticityCoords> chromaticityCoords;
    bool supportsBT2020 = false;
};

struct SOutputEvents {
    CSignal frame;
    CSignal needsFrame;
    CSignal destroy;
    CSignal state;
    CSignal commit;
    CSignal present;
};

struct SOutputState {
    bool enabled = true;
    bool adaptiveSync = false;
    uint32_t drmFormat = 0;
    SP<SOutputMode> mode = makeShared<SOutputMode>();
    SP<SOutputMode> customMode = makeShared<SOutputMode>();
    SP<Aquamarine::IBuffer> buffer;
    SHDRMetadata hdrMetadata;
    eOutputPresentationMode presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;

    void resetExplicitFences() {}
    void setAdaptiveSync(bool v) { adaptiveSync = v; }
    void setBuffer(SP<Aquamarine::IBuffer> b) { buffer = b; }
    void setFormat(uint32_t fmt) {}
    void setMode(SP<SOutputMode> m) {}
    void setCustomMode(SP<SOutputMode> m) {}
    void setGammaLut(const std::vector<uint16_t>& lut) {}
    void setPresentationMode(eOutputPresentationMode) {}
    void addDamage(const Hyprutils::Math::CBox&) {}
    void setExplicitInFence(int) {}
    void setEnabled(bool e) { enabled = e; }
    const SOutputState& state() const { return *this; }
};



class CAllocator {
public:
    virtual int drmFD() { return -1; }
};



class IOutput {
public:
    enum scheduleFrameReason : uint8_t {
        AQ_SCHEDULE_CLIENT_UNKNOWN = 0,
        AQ_SCHEDULE_CURSOR = 1,
        AQ_SCHEDULE_CURSOR_MOVE = 2,
        AQ_SCHEDULE_DAMAGE = 3,
        AQ_SCHEDULE_ANIMATION = 4,
        AQ_SCHEDULE_DAMAGE_TRACKING = 5,
        AQ_SCHEDULE_NEW_MONITOR = 6,
        AQ_SCHEDULE_CURSOR_SHAPE = 7,
        AQ_SCHEDULE_NEEDS_FRAME = 8
    };

    std::string   name = "AQ-Output";
    int32_t       id = 0;
    std::string   serial = "AQ-SERIAL-12345";
    std::string   make = "make";
    std::string   model = "model";
    Hyprutils::Math::Vector2D physicalSize = {520, 300};
    std::vector<SP<SOutputMode>> modes;
    CSignal       events_frame;
    CSignal       events_needsFrame;
    CSignal       events_destroy;
    SOutputEvents events;
    SEDID         parsedEDID;
    SP<SOutputState> state = makeShared<SOutputState>();
    SP<Aquamarine::CSwapchain> swapchain = makeShared<Aquamarine::CSwapchain>();
    std::string   description = "Description";
    bool          supportsExplicit = false;
    bool          vrrCapable = false;
    bool          nonDesktop = false;
    
    enum { AQ_OUTPUT_PRESENT_HW_CLOCK = 1 };

    struct SPresentEvent {
        timespec* when = nullptr;
        uint32_t refresh = 0;
        uint32_t seq = 0;
        uint32_t flags = 0;
    };
    struct SStateEvent {
        Hyprutils::Math::Vector2D size;
    };

    virtual SP<IBackend> getBackend() { return nullptr; }

    virtual SP<SOutputMode> preferredMode() { return nullptr; }

    virtual ~IOutput() = default;
    virtual void scheduleFrame(scheduleFrameReason reason = AQ_SCHEDULE_CLIENT_UNKNOWN) {}
    virtual bool setCursor(const std::vector<uint8_t>& data, const Hyprutils::Math::CBox& box, const Hyprutils::Math::Vector2D& hotspot) { return true; }
    virtual bool setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) { return true; }
    virtual void clearCursor() {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() { return {256, 256}; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& pos, bool b = false) {}
    virtual bool commit() { return true; }
    virtual bool test() { return true; }
    virtual int getGammaSize() { return 0; }
};

}

#endif /* AQUAMARINE_OUTPUT_OUTPUT_HPP */
