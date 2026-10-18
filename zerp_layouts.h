/*
 * WynlandOS / Zerp - keyboard layouts: PS/2 set-1 make codes 0..58 ->
 * the character they type, without and with Shift. Zerp 2.0 (its own
 * fields: apps/zerp2/main.cpp) and the Qt platform plugin of its clients
 * (qt_qpa/qwynlandfbinput.cpp) share them; Wayland clients get the same
 * layouts as xkb keymaps instead (apps/zerp2/zerp2.cpp).
 *
 * The keys themselves (Qt::Key_*, the shortcuts) stay the US ones in
 * every layout -- Ctrl+C is Ctrl+C on a Russian keyboard too.
 */
#ifndef ZERP_LAYOUTS_H
#define ZERP_LAYOUTS_H

#include <stdint.h>

#define ZERP_LAYOUT_US 0
#define ZERP_LAYOUT_RU 1
#define ZERP_LAYOUT_COUNT 2

/* xkb's names and the short ones for a panel */
static const char *const ZERP_LAYOUT_XKB[ZERP_LAYOUT_COUNT]   = { "us", "ru" };
static const char *const ZERP_LAYOUT_SHORT[ZERP_LAYOUT_COUNT] = { "EN", "RU" };
static const char *const ZERP_LAYOUT_NAME[ZERP_LAYOUT_COUNT]  = { "English (US)", "Russian" };

static const uint16_t ZERP_KEYMAP[ZERP_LAYOUT_COUNT][2][59] = {
    { /* us */
        {   0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
          '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
            0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
          '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 },
        {   0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
          '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
            0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
          '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 },
    },
    { /* ru: ЙЦУКЕН */
        {   0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
          '\t', 0x439, 0x446, 0x443, 0x43A, 0x435, 0x43D, 0x433, 0x448, 0x449, 0x437, 0x445, 0x44A, '\r',
            0, 0x444, 0x44B, 0x432, 0x430, 0x43F, 0x440, 0x43E, 0x43B, 0x434, 0x436, 0x44D, 0x451, 0,
          '\\', 0x44F, 0x447, 0x441, 0x43C, 0x438, 0x442, 0x44C, 0x431, 0x44E, '.', 0, '*', 0, ' ', 0 },
        {   0,  27, '!', '"', 0x2116, ';', '%', ':', '?', '*', '(', ')', '_', '+', '\b',
          '\t', 0x419, 0x426, 0x423, 0x41A, 0x415, 0x41D, 0x413, 0x428, 0x429, 0x417, 0x425, 0x42A, '\r',
            0, 0x424, 0x42B, 0x412, 0x410, 0x41F, 0x420, 0x41E, 0x41B, 0x414, 0x416, 0x42D, 0x401, 0,
          '/', 0x42F, 0x427, 0x421, 0x41C, 0x418, 0x422, 0x42C, 0x411, 0x42E, ',', 0, '*', 0, ' ', 0 },
    },
};

/* the character `code` types in `layout`, 0 for none. Caps Lock flips the
   case of letters (Latin and Cyrillic) */
static inline uint16_t zerp_key_char(int layout, int code, int shift, int caps)
{
    if (layout < 0 || layout >= ZERP_LAYOUT_COUNT || code < 0 || code >= 59) return 0;
    uint16_t c = ZERP_KEYMAP[layout][shift ? 1 : 0][code];
    if (caps) {
        uint16_t o = ZERP_KEYMAP[layout][shift ? 0 : 1][code];
        int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0x401 && c <= 0x451 && c != 0x2116);
        if (letter) c = o;
    }
    return c;
}

#endif /* ZERP_LAYOUTS_H */
