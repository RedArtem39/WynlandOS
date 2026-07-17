#pragma once
#include <stdint.h>
#include <stddef.h>
static inline void g_object_unref(void* object) {}
typedef struct _PangoLayout PangoLayout;

#define PANGO_WEIGHT_NORMAL 400
#define PANGO_STYLE_NORMAL 0

static inline void* pango_font_description_new() { return NULL; }
static inline void pango_font_description_set_family(void* desc, const char* family) {}
static inline void pango_font_description_set_style(void* desc, int style) {}

static inline void pango_font_description_set_weight(void* desc, int weight) {}
static inline void pango_layout_set_font_description(void* layout, void* desc) {}
static inline void pango_font_description_free(void* desc) {}

static inline void* pango_cairo_create_layout(void* cairo) { return NULL; }
static inline void pango_layout_set_text(void* layout, const char* text, int length) {}
static inline int pango_layout_get_unknown_glyphs_count(void* layout) { return 0; }
