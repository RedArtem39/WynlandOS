#ifndef AQUAMARINE_INPUT_INPUT_HPP
#define AQUAMARINE_INPUT_INPUT_HPP

#include <string>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/math/Vector2D.hpp>

namespace Aquamarine {

enum eInputDeviceType : uint8_t {
    AQ_INPUT_DEVICE_POINTER   = 0,
    AQ_INPUT_DEVICE_KEYBOARD  = 1,
    AQ_INPUT_DEVICE_TOUCH     = 2,
    AQ_INPUT_DEVICE_TABLET    = 3,
    AQ_INPUT_DEVICE_TABLET_PAD = 4,
    AQ_INPUT_DEVICE_SWITCH    = 5
};

struct SInputDeviceEvents {
    CSignal destroy;
    CSignal button;
    CSignal key;
    CSignal modifiers;
    CSignal move;
    CSignal warp;
    CSignal motion;
    CSignal axis;
    CSignal frame;
    CSignal cancel;
    CSignal down;
    CSignal up;
    CSignal swipeBegin;
    CSignal swipeUpdate;
    CSignal swipeEnd;
    CSignal pinchBegin;
    CSignal pinchUpdate;
    CSignal pinchEnd;
    CSignal holdBegin;
    CSignal holdEnd;
    CSignal proximity;
    CSignal tip;
    CSignal ring;
    CSignal strip;
    CSignal attach;
    CSignal fire;
};

class IInputDevice {
public:
    SInputDeviceEvents events;
    virtual std::string getName() { return "device"; }
    virtual eInputDeviceType getType() { return AQ_INPUT_DEVICE_POINTER; }
    virtual void* getLibinputHandle() { return nullptr; }
    virtual ~IInputDevice() = default;
};

class IPointer : public IInputDevice {
public:
    struct SMoveEvent { uint32_t timeMs = 0; Hyprutils::Math::Vector2D delta; Hyprutils::Math::Vector2D unaccel; };
    struct SWarpEvent { uint32_t timeMs = 0; Hyprutils::Math::Vector2D absolute; };
    struct SButtonEvent { uint32_t timeMs = 0; uint32_t button = 0; bool pressed = false; };
    struct SAxisEvent { uint32_t timeMs = 0; uint32_t source = 0; uint32_t axis = 0; uint32_t direction = 0; double delta = 0; int32_t discrete = 0; };
    struct SSwipeBeginEvent { uint32_t timeMs = 0; uint32_t fingers = 0; };
    struct SSwipeEndEvent { uint32_t timeMs = 0; bool cancelled = false; };
    struct SSwipeUpdateEvent { uint32_t timeMs = 0; uint32_t fingers = 0; Hyprutils::Math::Vector2D delta; };
    struct SPinchBeginEvent { uint32_t timeMs = 0; uint32_t fingers = 0; };
    struct SPinchEndEvent { uint32_t timeMs = 0; bool cancelled = false; };
    struct SPinchUpdateEvent { uint32_t timeMs = 0; uint32_t fingers = 0; Hyprutils::Math::Vector2D delta; double scale = 0; double rotation = 0; };
    struct SHoldBeginEvent { uint32_t timeMs = 0; uint32_t fingers = 0; };
    struct SHoldEndEvent { uint32_t timeMs = 0; bool cancelled = false; };

    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_POINTER; }
};

class IKeyboard : public IInputDevice {
public:
    struct SKeyEvent {
        uint32_t timeMs = 0;
        uint32_t key = 0;
        bool pressed = false;
    };
    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_KEYBOARD; }
    virtual void updateLEDs(uint32_t leds) {}
};

class ITouch : public IInputDevice {
public:
    struct SDownEvent { uint32_t timeMs = 0; int32_t touchID = 0; Hyprutils::Math::Vector2D pos; };
    struct SUpEvent { uint32_t timeMs = 0; int32_t touchID = 0; };
    struct SMotionEvent { uint32_t timeMs = 0; int32_t touchID = 0; Hyprutils::Math::Vector2D pos; };
    struct SCancelEvent { uint32_t timeMs = 0; int32_t touchID = 0; };
    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_TOUCH; }
};

enum eTabletToolAxis {
    AQ_TABLET_TOOL_AXIS_X = 1,
    AQ_TABLET_TOOL_AXIS_Y = 2,
    AQ_TABLET_TOOL_AXIS_DISTANCE = 4,
    AQ_TABLET_TOOL_AXIS_PRESSURE = 8,
    AQ_TABLET_TOOL_AXIS_TILT_X = 16,
    AQ_TABLET_TOOL_AXIS_TILT_Y = 32,
    AQ_TABLET_TOOL_AXIS_ROTATION = 64,
    AQ_TABLET_TOOL_AXIS_SLIDER = 128,
    AQ_TABLET_TOOL_AXIS_WHEEL = 256
};

class ITabletTool {
public:
    enum eTabletToolType {
        AQ_TABLET_TOOL_TYPE_PEN,
        AQ_TABLET_TOOL_TYPE_ERASER,
        AQ_TABLET_TOOL_TYPE_BRUSH,
        AQ_TABLET_TOOL_TYPE_PENCIL,
        AQ_TABLET_TOOL_TYPE_AIRBRUSH,
        AQ_TABLET_TOOL_TYPE_MOUSE,
        AQ_TABLET_TOOL_TYPE_LENS
    };
    enum eTabletToolCapability {
        AQ_TABLET_TOOL_CAPABILITY_TILT = 1,
        AQ_TABLET_TOOL_CAPABILITY_PRESSURE = 2,
        AQ_TABLET_TOOL_CAPABILITY_DISTANCE = 4,
        AQ_TABLET_TOOL_CAPABILITY_ROTATION = 8,
        AQ_TABLET_TOOL_CAPABILITY_SLIDER = 16,
        AQ_TABLET_TOOL_CAPABILITY_WHEEL = 32
    };
    uint32_t type = AQ_TABLET_TOOL_TYPE_PEN;
    uint32_t capabilities = 0;
    uint64_t serial = 0;
    uint64_t id = 0;
    SInputDeviceEvents events;
    virtual void* getLibinputTool() { return nullptr; }
    virtual std::string getName() { return "TabletTool"; }
};

class ITablet : public IInputDevice {
public:
    struct SAxisEvent { uint32_t timeMs = 0; SP<ITabletTool> tool; uint32_t updatedAxes = 0; Hyprutils::Math::Vector2D delta; Hyprutils::Math::Vector2D tilt; double pressure = 0; double distance = 0; double rotation = 0; double slider = 0; double wheelDelta = 0; Hyprutils::Math::Vector2D absolute; };
    struct SProximityEvent { uint32_t timeMs = 0; SP<ITabletTool> tool; Hyprutils::Math::Vector2D absolute; bool in = false; };
    struct STipEvent { uint32_t timeMs = 0; SP<ITabletTool> tool; Hyprutils::Math::Vector2D absolute; bool down = false; };
    struct SButtonEvent { uint32_t timeMs = 0; SP<ITabletTool> tool; uint32_t button = 0; bool down = false; };

    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_TABLET; }
    Hyprutils::Math::Vector2D physicalSize;
};

class ITabletPad : public IInputDevice {
public:
    struct STabletPadGroup {
        uint32_t buttons = 0;
        uint32_t rings = 0;
        uint32_t strips = 0;
        uint32_t modes = 0;
    };
    enum { AQ_TABLET_PAD_RING_SOURCE_FINGER = 1, AQ_TABLET_PAD_STRIP_SOURCE_FINGER = 1 };
    struct SButtonEvent { uint32_t timeMs = 0; uint32_t button = 0; bool down = false; uint32_t mode = 0; uint32_t group = 0; };
    struct SRingEvent { uint32_t timeMs = 0; uint32_t ring = 0; double pos = 0; uint32_t source = 0; uint32_t mode = 0; };
    struct SStripEvent { uint32_t timeMs = 0; uint32_t strip = 0; double pos = 0; uint32_t source = 0; uint32_t mode = 0; };

    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_TABLET_PAD; }
};

class ISwitch : public IInputDevice {
public:
    struct SFireEvent { uint32_t timeMs = 0; uint32_t state = 0; bool enable = false; };
    eInputDeviceType getType() override { return AQ_INPUT_DEVICE_SWITCH; }
};

}

inline double libinput_device_config_accel_get_default_speed(void* device) { return 0.0; }

#define LIBINPUT_DEVICE_CAP_POINTER 1
inline int libinput_device_has_capability(void* device, int cap) { return 0; }
inline int libinput_device_get_size(void* device, double* w, double* h) { return -1; }

// libinput accel config stubs
inline double libinput_device_config_accel_get_speed(void* device) { return 0.0; }
inline int libinput_device_config_accel_get_profile(void* device) { return 0; }
inline int libinput_device_config_accel_get_default_profile(void* device) { return 0; }
inline int libinput_device_config_accel_set_speed(void* device, double speed) { return 0; }
inline int libinput_device_config_accel_set_profile(void* device, int profile) { return 0; }

// libinput send-events config stubs
#define LIBINPUT_CONFIG_SEND_EVENTS_ENABLED 0
#define LIBINPUT_CONFIG_SEND_EVENTS_DISABLED 1
inline int libinput_device_config_send_events_get_mode(void* device) { return 0; }
inline int libinput_device_config_send_events_set_mode(void* device, int mode) { return 0; }

// libinput click-method config stubs
#define LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS 1
#define LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER 2
inline int libinput_device_config_click_get_method(void* device) { return 0; }
inline int libinput_device_config_click_set_method(void* device, int method) { return 0; }

// libinput left-handed config stubs
inline int libinput_device_config_left_handed_is_available(void* device) { return 0; }
inline int libinput_device_config_left_handed_get(void* device) { return 0; }
inline int libinput_device_config_left_handed_set(void* device, int val) { return 0; }

// libinput middle-button emulation stubs
#define LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED 1
#define LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED 0
inline int libinput_device_config_middle_emulation_is_available(void* device) { return 0; }
inline int libinput_device_config_middle_emulation_get_enabled(void* device) { return 0; }
inline int libinput_device_config_middle_emulation_set_enabled(void* device, int val) { return 0; }

// libinput tap config stubs
enum libinput_config_drag_lock_state {
    LIBINPUT_CONFIG_DRAG_LOCK_DISABLED = 0,
    LIBINPUT_CONFIG_DRAG_LOCK_ENABLED = 1
};
#define LIBINPUT_CONFIG_TAP_ENABLED 1
#define LIBINPUT_CONFIG_TAP_DISABLED 0
#define LIBINPUT_CONFIG_TAP_MAP_LRM 0
#define LIBINPUT_CONFIG_TAP_MAP_LMR 1
#define LIBINPUT_CONFIG_DRAG_ENABLED 1
#define LIBINPUT_CONFIG_DRAG_DISABLED 0
inline int libinput_device_config_tap_get_finger_count(void* device) { return 0; }
inline int libinput_device_config_tap_set_enabled(void* device, int val) { return 0; }
inline int libinput_device_config_tap_get_enabled(void* device) { return 0; }
inline int libinput_device_config_tap_set_button_map(void* device, int map) { return 0; }
inline int libinput_device_config_tap_get_button_map(void* device) { return 0; }
inline int libinput_device_config_tap_set_drag_enabled(void* device, int val) { return 0; }
inline int libinput_device_config_tap_set_drag_lock_enabled(void* device, libinput_config_drag_lock_state val) { return 0; }

// libinput scroll config stubs
#define LIBINPUT_CONFIG_SCROLL_NO_SCROLL 0
#define LIBINPUT_CONFIG_SCROLL_2FG 1
#define LIBINPUT_CONFIG_SCROLL_EDGE 2
#define LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN 3
#define LIBINPUT_CONFIG_SCROLL_AXIS_HORIZONTAL 1
#define LIBINPUT_CONFIG_SCROLL_AXIS_VERTICAL 2
inline int libinput_device_config_scroll_set_method(void* device, int method) { return 0; }
inline int libinput_device_config_scroll_get_method(void* device) { return 0; }
inline int libinput_device_config_scroll_get_default_method(void* device) { return 0; }
inline int libinput_device_config_scroll_set_button(void* device, uint32_t btn) { return 0; }
inline int libinput_device_config_scroll_get_button(void* device) { return 0; }
inline int libinput_device_config_scroll_set_natural_scroll_enabled(void* device, int val) { return 0; }
inline int libinput_device_config_scroll_get_natural_scroll_enabled(void* device) { return 0; }
inline int libinput_device_config_scroll_has_natural_scroll(void* device) { return 0; }

// 3fg drag
enum libinput_config_3fg_drag_state {
    LIBINPUT_CONFIG_3FG_DRAG_DISABLED = 0,
    LIBINPUT_CONFIG_3FG_DRAG_ENABLED = 1
};
inline int libinput_device_config_3fg_drag_get_finger_count(void* device) { return 0; }
inline int libinput_device_config_3fg_drag_set_enabled(void* device, libinput_config_3fg_drag_state val) { return 0; }

// libinput calibration config stubs
inline int libinput_device_config_calibration_has_matrix(void* device) { return 0; }
inline int libinput_device_config_calibration_set_matrix(void* device, const float matrix[6]) { return 0; }

// libinput dwt (disable-while-typing) stubs
enum libinput_config_dwt_state {
    LIBINPUT_CONFIG_DWT_DISABLED = 0,
    LIBINPUT_CONFIG_DWT_ENABLED = 1
};
inline int libinput_device_config_dwt_is_available(void* device) { return 0; }
inline int libinput_device_config_dwt_set_enabled(void* device, libinput_config_dwt_state val) { return 0; }
inline int libinput_device_config_dwt_get_enabled(void* device) { return 0; }

// rotation
inline int libinput_device_config_rotation_is_available(void* device) { return 0; }
inline int libinput_device_config_rotation_set_angle(void* device, unsigned int val) { return 0; }

#define LIBINPUT_CONFIG_ACCEL_PROFILE_NONE 0
#define LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT 1
#define LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE 2
#define LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM 3
#define LIBINPUT_ACCEL_TYPE_SCROLL 0
#define LIBINPUT_ACCEL_TYPE_MOTION 1
inline void* libinput_config_accel_create(int profile) { return nullptr; }
inline int libinput_config_accel_set_points(void* config, int type, double step, int npoints, double* points) { return 0; }
inline int libinput_device_config_accel_apply(void* device, void* config) { return 0; }
inline void libinput_config_accel_destroy(void* config) {}

inline int libinput_device_config_scroll_get_default_button(void* device) { return 0; }
#define LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED 0
#define LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED 1
inline int libinput_device_config_scroll_set_button_lock(void* device, int lock) { return 0; }

#define LIBINPUT_CONFIG_ERASER_BUTTON_DEFAULT 0
#define LIBINPUT_CONFIG_ERASER_BUTTON_BUTTON 1
inline int libinput_tablet_tool_config_eraser_button_set_mode(void* tool, int mode) { return 0; }
inline int libinput_tablet_tool_config_eraser_button_get_default_button(void* tool) { return 0; }
inline int libinput_tablet_tool_config_eraser_button_set_button(void* tool, int button) { return 0; }
inline double libinput_tablet_tool_config_pressure_range_get_default_minimum(void* tool) { return 0.0; }
inline double libinput_tablet_tool_config_pressure_range_get_default_maximum(void* tool) { return 1.0; }
inline int libinput_tablet_tool_config_pressure_range_set(void* tool, double min, double max) { return 0; }

#endif /* AQUAMARINE_INPUT_INPUT_HPP */
