#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint32_t xkb_keycode_t;
typedef uint32_t xkb_keysym_t;
typedef uint32_t xkb_mod_index_t;
typedef uint32_t xkb_layout_index_t;
typedef uint32_t xkb_mod_mask_t;
typedef uint32_t xkb_led_index_t;

struct xkb_state;
struct xkb_keymap;
struct xkb_context;

#define XKB_KEY_NoSymbol 0
#define XKB_MOD_INVALID (0xffffffff)
#define XKB_LED_INVALID (0xffffffff)
#define XKB_KEYSYM_NO_FLAGS 0
#define XKB_KEYSYM_CASE_INSENSITIVE (1 << 0)
#define XKB_MOD_NAME_CAPS "Caps_Lock"
#define XKB_MOD_NAME_NUM "Num_Lock"
#define XKB_MOD_NAME_SHIFT "Shift"
#define XKB_MOD_NAME_CTRL "Control"
#define XKB_MOD_NAME_ALT "Alt"
#define XKB_MOD_NAME_LOGO "Super"

#define XKB_KEYMAP_FORMAT_TEXT_V2 1
#define XKB_KEYMAP_FORMAT_TEXT_V1 2
#define XKB_KEYMAP_COMPILE_NO_FLAGS 0
#define XKB_STATE_LAYOUT_EFFECTIVE (1 << 0)
#define XKB_STATE_MODS_DEPRESSED (1 << 0)
#define XKB_STATE_MODS_LATCHED (1 << 1)
#define XKB_STATE_MODS_LOCKED (1 << 2)
#define XKB_KEY_DOWN 1
#define XKB_KEY_UP 0
#define XKB_CONTEXT_NO_FLAGS 0

#define XKB_LED_NAME_NUM "Num Lock"
#define XKB_LED_NAME_CAPS "Caps Lock"
#define XKB_LED_NAME_SCROLL "Scroll Lock"

struct xkb_rule_names {
    const char *rules;
    const char *model;
    const char *layout;
    const char *variant;
    const char *options;
};

#ifdef __cplusplus
extern "C" {
#endif

inline xkb_keysym_t xkb_keysym_from_name(const char* name, uint32_t flags) { return 0; }
inline xkb_keycode_t xkb_keymap_min_keycode(struct xkb_keymap* keymap) { return 8; }
inline xkb_keycode_t xkb_keymap_max_keycode(struct xkb_keymap* keymap) { return 255; }
inline xkb_keysym_t xkb_state_key_get_one_sym(struct xkb_state* state, xkb_keycode_t key) { return 0; }
inline int xkb_keycode_is_legal_x11(xkb_keycode_t keycode) { return keycode >= 8 && keycode <= 255; }
inline int xkb_keycode_is_legal_ext(xkb_keycode_t keycode) { return keycode >= 8; }
inline xkb_mod_index_t xkb_keymap_mod_get_index(struct xkb_keymap* keymap, const char* name) { return 0; }

inline xkb_mod_index_t xkb_map_mod_get_index(struct xkb_keymap* keymap, const char* name) { return 0; }
inline xkb_led_index_t xkb_map_led_get_index(struct xkb_keymap* keymap, const char* name) { return 0; }

inline struct xkb_keymap* xkb_keymap_new_from_names2(struct xkb_context* context, const struct xkb_rule_names* names, int format, int flags) { return (struct xkb_keymap*)1; }
inline struct xkb_keymap* xkb_keymap_new_from_file(struct xkb_context* context, FILE* file, int format, int flags) { return (struct xkb_keymap*)1; }
inline struct xkb_context* xkb_context_new(int flags) { return (struct xkb_context*)1; }
inline struct xkb_state* xkb_state_new(struct xkb_keymap* keymap) { return (struct xkb_state*)1; }
inline void xkb_state_unref(struct xkb_state* state) {}
inline void xkb_keymap_unref(struct xkb_keymap* keymap) {}
inline struct xkb_keymap* xkb_keymap_ref(struct xkb_keymap* keymap) { return keymap; }
inline void xkb_context_unref(struct xkb_context* context) {}
inline uint32_t xkb_keymap_num_layouts(struct xkb_keymap* keymap) { return 1; }
inline int xkb_state_layout_index_is_active(struct xkb_state* state, xkb_layout_index_t idx, int type) { return 0; }
inline const char* xkb_keymap_layout_get_name(struct xkb_keymap* keymap, xkb_layout_index_t idx) { return "us"; }
inline int xkb_state_led_index_is_active(struct xkb_state* state, xkb_led_index_t idx) { return 0; }
inline void xkb_state_update_mask(struct xkb_state* state, xkb_mod_mask_t depressed, xkb_mod_mask_t latched, xkb_mod_mask_t locked, xkb_layout_index_t base_group, xkb_layout_index_t latched_group, xkb_layout_index_t locked_group) {}
inline xkb_mod_mask_t xkb_state_serialize_mods(struct xkb_state* state, int type) { return 0; }
inline xkb_layout_index_t xkb_state_serialize_layout(struct xkb_state* state, int type) { return 0; }
inline void xkb_state_update_key(struct xkb_state* state, xkb_keycode_t key, int down) {}
inline char* xkb_keymap_get_as_string(struct xkb_keymap* keymap, int format) { return nullptr; }
inline xkb_keysym_t xkb_keysym_to_upper(xkb_keysym_t ks) { return ks; }

#ifdef __cplusplus
}
#endif
