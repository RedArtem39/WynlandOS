#pragma once
#include <xcb/xcb.h>
struct xcb_errors_context_t;
inline int xcb_errors_context_new(xcb_connection_t* c, xcb_errors_context_t** ctx) { return -1; }
inline void xcb_errors_context_free(xcb_errors_context_t* ctx) {}
inline const char* xcb_errors_get_name_for_error(xcb_errors_context_t* ctx, uint8_t error_code, const char** extension) { return "Unknown error"; }
inline const char* xcb_errors_get_name_for_major_code(xcb_errors_context_t* ctx, uint8_t major_code) { return "Unknown major"; }
inline const char* xcb_errors_get_name_for_minor_code(xcb_errors_context_t* ctx, uint8_t major_code, uint16_t minor_code) { return "Unknown minor"; }
