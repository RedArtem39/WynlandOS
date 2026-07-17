#ifndef LIBINPUT_H
#define LIBINPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct libinput;
struct libinput_device;
struct libinput_event;
struct libinput_event_pointer;
struct libinput_event_keyboard;
struct libinput_event_touch;

enum libinput_event_type {
    LIBINPUT_EVENT_NONE = 0,
    LIBINPUT_EVENT_KEYBOARD_KEY = 300,
    LIBINPUT_EVENT_POINTER_MOTION = 400,
    LIBINPUT_EVENT_POINTER_BUTTON = 401
};

#ifdef __cplusplus
}
#endif

#endif /* LIBINPUT_H */
