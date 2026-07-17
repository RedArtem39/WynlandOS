#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

typedef struct _cairo_pattern cairo_pattern_t;
typedef struct _cairo_matrix {
    double xx; double yx;
    double xy; double yy;
    double x0; double y0;
} cairo_matrix_t;

#define CAIRO_FORMAT_ARGB32 0
#define CAIRO_FILTER_BILINEAR 1
#define CAIRO_OPERATOR_SOURCE 1

static inline cairo_surface_t* cairo_image_surface_create(int format, int width, int height) { return NULL; }
static inline unsigned char* cairo_image_surface_get_data(cairo_surface_t* surface) { return NULL; }
static inline int cairo_image_surface_get_stride(cairo_surface_t* surface) { return 0; }
static inline int cairo_image_surface_get_height(cairo_surface_t* surface) { return 0; }
static inline cairo_t* cairo_create(cairo_surface_t* target) { return NULL; }

static inline void cairo_set_source_rgba(cairo_t* cr, double r, double g, double b, double a) {}
static inline void cairo_rectangle(cairo_t* cr, double x, double y, double width, double height) {}
static inline void cairo_fill(cairo_t* cr) {}

static inline int cairo_format_stride_for_width(int format, int width) { return width * 4; }
static inline cairo_surface_t* cairo_image_surface_create_for_data(unsigned char* data, int format, int width, int height, int stride) { return NULL; }
static inline void cairo_paint(cairo_t* cr) {}
static inline void cairo_destroy(cairo_t* cr) {}
static inline void cairo_surface_destroy(cairo_surface_t* surface) {}
static inline cairo_pattern_t* cairo_pattern_create_for_surface(cairo_surface_t* surface) { return NULL; }
static inline void cairo_pattern_set_filter(cairo_pattern_t* pattern, int filter) {}
static inline void cairo_matrix_init_identity(cairo_matrix_t* matrix) {}
static inline void cairo_matrix_init(cairo_matrix_t* matrix, double xx, double yx, double xy, double yy, double x0, double y0) {}
static inline void cairo_pattern_set_matrix(cairo_pattern_t* pattern, const cairo_matrix_t* matrix) {}
static inline void cairo_set_source(cairo_t* cr, cairo_pattern_t* source) {}
static inline void cairo_set_operator(cairo_t* cr, int op) {}
static inline void cairo_surface_flush(cairo_surface_t* surface) {}
static inline void cairo_pattern_destroy(cairo_pattern_t* pattern) {}
