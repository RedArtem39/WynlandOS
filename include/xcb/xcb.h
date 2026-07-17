#pragma once
#include <stdint.h>
#include <stddef.h>

struct xcb_connection_t {
    int fd;
};
typedef struct xcb_connection_t xcb_connection_t;

typedef uint32_t xcb_window_t;
typedef uint32_t xcb_colormap_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_timestamp_t;
typedef uint32_t xcb_atom_t;
typedef uint32_t xcb_cursor_t;
typedef uint32_t xcb_gcontext_t;
typedef uint32_t xcb_pixmap_t;
typedef uint32_t xcb_drawable_t;
typedef uint32_t xcb_font_t;
typedef uint32_t xcb_render_pictformat_t;

typedef struct {
    union {
        uint8_t  data8[20];
        uint16_t data16[10];
        uint32_t data32[5];
    };
} xcb_client_message_data_t;

typedef struct {
    uint8_t         response_type;
    uint8_t         format;
    uint16_t        sequence;
    xcb_window_t    window;
    xcb_atom_t      type;
    xcb_client_message_data_t data;
} xcb_client_message_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    xcb_timestamp_t time;
    xcb_window_t owner;
    xcb_window_t requestor;
    xcb_atom_t   selection;
    xcb_atom_t   target;
    xcb_atom_t   property;
} xcb_selection_request_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    xcb_timestamp_t time;
    xcb_window_t requestor;
    xcb_atom_t   selection;
    xcb_atom_t   target;
    xcb_atom_t   property;
} xcb_selection_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    xcb_timestamp_t timestamp;
    xcb_window_t owner;
    xcb_window_t window;
    xcb_atom_t   selection;
} xcb_xfixes_selection_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    uint32_t     length;
    xcb_atom_t   type;
    uint32_t     bytes_after;
    uint32_t     value_len;
    uint8_t      format;
} xcb_get_property_reply_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    xcb_window_t window;
    xcb_atom_t   atom;
    xcb_timestamp_t time;
    uint8_t      state;
} xcb_property_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
} xcb_destroy_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
} xcb_unmap_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t event;
    xcb_window_t window;
    uint8_t      override_redirect;
} xcb_map_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
} xcb_map_request_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
    uint16_t     width, height, border_width;
    xcb_window_t above;
    uint8_t      override_redirect;
} xcb_configure_request_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t event;
    xcb_window_t window;
    int16_t      x, y;
    uint16_t     width, height, border_width;
    xcb_window_t above;
    uint8_t      override_redirect;
} xcb_configure_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t parent;
    xcb_window_t window;
    int16_t      x, y;
    uint16_t     width, height, border_width;
    uint8_t      override_redirect;
} xcb_create_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
} xcb_button_press_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
} xcb_motion_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
} xcb_enter_notify_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
} xcb_focus_in_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      detail;
    uint16_t     sequence;
    xcb_window_t window;
    int16_t      x, y;
} xcb_focus_out_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      error_code;
    uint16_t     sequence;
    uint32_t     bad_value;
    uint16_t     minor_opcode;
    uint8_t      major_opcode;
} xcb_value_error_t;

typedef struct {
    uint32_t sequence;
} xcb_void_cookie_t;

typedef struct {
    uint32_t sequence;
} xcb_get_property_cookie_t;

typedef struct {
    uint32_t sequence;
} xcb_intern_atom_cookie_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    uint32_t     length;
    xcb_atom_t   atom;
} xcb_intern_atom_reply_t;

typedef struct {
    uint32_t     sequence;
} xcb_get_geometry_cookie_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      depth;
    uint16_t     sequence;
    uint32_t     length;
    xcb_window_t root;
    int16_t      x, y;
    uint16_t     width, height;
    uint16_t     border_width;
} xcb_get_geometry_reply_t;

typedef struct {
    uint32_t sequence;
} xcb_query_tree_cookie_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    uint32_t     length;
    xcb_window_t root;
    xcb_window_t parent;
    uint16_t     children_len;
} xcb_query_tree_reply_t;

typedef struct {
    uint32_t sequence;
} xcb_query_extension_cookie_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      pad0;
    uint16_t     sequence;
    uint32_t     length;
    uint8_t      present;
    uint8_t      major_opcode;
    uint8_t      first_event;
    uint8_t      first_error;
} xcb_query_extension_reply_t;

typedef struct {
    uint32_t sequence;
} xcb_get_window_attributes_cookie_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      backing_store;
    uint16_t     sequence;
    uint32_t     length;
    xcb_visualid_t visual;
    uint16_t     _class;
    uint8_t      bit_gravity;
    uint8_t      win_gravity;
    uint32_t     backing_planes;
    uint32_t     backing_pixel;
    uint8_t      save_under;
    uint8_t      map_is_installed;
    uint8_t      map_state;
    uint8_t      override_redirect;
    xcb_colormap_t colormap;
    uint32_t     all_event_masks;
    uint32_t     your_event_mask;
    uint16_t     do_not_propagate_mask;
} xcb_get_window_attributes_reply_t;

typedef union {
    uint8_t                      response_type;
    xcb_client_message_event_t   client_message;
    xcb_selection_request_event_t selection_request;
    xcb_selection_notify_event_t selection_notify;
    xcb_xfixes_selection_notify_event_t xfixes_selection_notify;
    xcb_property_notify_event_t  property_notify;
    xcb_destroy_notify_event_t   destroy_notify;
    xcb_unmap_notify_event_t     unmap_notify;
    xcb_map_notify_event_t       map_notify;
    xcb_map_request_event_t      map_request;
    xcb_configure_request_event_t configure_request;
    xcb_configure_notify_event_t configure_notify;
    xcb_create_notify_event_t    create_notify;
    xcb_focus_in_event_t         focus_in;
    xcb_focus_out_event_t        focus_out;
} xcb_generic_event_t;

typedef struct {
    uint8_t      response_type;
    uint8_t      error_code;
    uint16_t     sequence;
    uint32_t     resource_id;
    uint16_t     minor_code;
    uint8_t      major_code;
} xcb_generic_error_t;

typedef struct {
    uint32_t root;
    uint32_t default_colormap;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t current_input_masks;
    uint16_t width_in_pixels;
    uint16_t height_in_pixels;
    uint16_t width_in_millimeters;
    uint16_t height_in_millimeters;
    uint16_t min_installed_maps;
    uint16_t max_installed_maps;
    uint32_t root_visual;
    uint8_t  backing_stores;
    uint8_t  save_unders;
    uint8_t  root_depth;
    uint8_t  allowed_depths_len;
} xcb_screen_t;

typedef struct {
    int rem;
    xcb_screen_t* data;
} xcb_screen_iterator_t;

typedef struct {
    uint32_t roots_len;
} xcb_setup_t;

inline xcb_connection_t* xcb_connect_to_fd(int fd, int* screen_p) { static xcb_connection_t c = {fd}; return &c; }
inline void xcb_disconnect(xcb_connection_t* c) {}
inline int xcb_connection_has_error(xcb_connection_t* c) { return 0; }
inline int xcb_get_file_descriptor(xcb_connection_t* c) { return c ? c->fd : -1; }
inline const xcb_setup_t* xcb_get_setup(xcb_connection_t* c) { static xcb_setup_t s = {0}; return &s; }
inline xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t* R) { xcb_screen_iterator_t i = {0, nullptr}; return i; }
inline void xcb_screen_next(xcb_screen_iterator_t* i) {}
inline xcb_void_cookie_t xcb_change_window_attributes(xcb_connection_t* c, xcb_window_t window, uint32_t value_mask, const void* value_list) { return {0}; }
inline int xcb_flush(xcb_connection_t* c) { return 0; }
inline uint32_t xcb_generate_id(xcb_connection_t* c) { return 0; }
inline xcb_void_cookie_t xcb_create_window(xcb_connection_t* c, uint8_t depth, xcb_window_t wid, xcb_window_t parent, int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t border_width, uint16_t _class, xcb_visualid_t visual, uint32_t value_mask, const void* value_list) { return {0}; }
inline xcb_void_cookie_t xcb_destroy_window(xcb_connection_t* c, xcb_window_t window) { return {0}; }
inline xcb_void_cookie_t xcb_map_window(xcb_connection_t* c, xcb_window_t window) { return {0}; }
inline xcb_void_cookie_t xcb_unmap_window(xcb_connection_t* c, xcb_window_t window) { return {0}; }
inline xcb_void_cookie_t xcb_configure_window(xcb_connection_t* c, xcb_window_t window, uint16_t value_mask, const void* value_list) { return {0}; }
inline xcb_void_cookie_t xcb_send_event(xcb_connection_t* c, uint8_t propagate, xcb_window_t destination, uint32_t event_mask, const char* event) { return {0}; }
inline xcb_void_cookie_t xcb_set_selection_owner(xcb_connection_t* c, xcb_window_t owner, xcb_atom_t selection, xcb_timestamp_t time) { return {0}; }
inline xcb_get_property_cookie_t xcb_get_property(xcb_connection_t* c, uint8_t _delete, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint32_t long_offset, uint32_t long_length) { return {0}; }
inline xcb_get_property_reply_t* xcb_get_property_reply(xcb_connection_t* c, xcb_get_property_cookie_t cookie, xcb_generic_error_t** e) { return nullptr; }
inline void* xcb_get_property_value(const xcb_get_property_reply_t* R) { return nullptr; }
inline int xcb_get_property_value_length(const xcb_get_property_reply_t* R) { return 0; }
inline xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t* c, uint8_t only_if_exists, uint16_t name_len, const char* name) { return {0}; }
inline xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t* c, xcb_intern_atom_cookie_t cookie, xcb_generic_error_t** e) { return nullptr; }
inline xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t* c, xcb_drawable_t drawable) { return {0}; }
inline xcb_get_geometry_reply_t* xcb_get_geometry_reply(xcb_connection_t* c, xcb_get_geometry_cookie_t cookie, xcb_generic_error_t** e) { return nullptr; }
inline xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t* c, xcb_window_t window) { return {0}; }
inline xcb_query_tree_reply_t* xcb_query_tree_reply(xcb_connection_t* c, xcb_query_tree_cookie_t cookie, xcb_generic_error_t** e) { return nullptr; }
inline xcb_window_t* xcb_query_tree_children(const xcb_query_tree_reply_t* R) { return nullptr; }
inline int xcb_query_tree_children_length(const xcb_query_tree_reply_t* R) { return 0; }
inline xcb_get_window_attributes_cookie_t xcb_get_window_attributes(xcb_connection_t* c, xcb_window_t window) { return {0}; }
inline xcb_get_window_attributes_reply_t* xcb_get_window_attributes_reply(xcb_connection_t* c, xcb_get_window_attributes_cookie_t cookie, xcb_generic_error_t** e) { return nullptr; }
