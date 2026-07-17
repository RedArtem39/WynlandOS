#ifndef PIXMAN_H
#define PIXMAN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pixman_region32 pixman_region32_t;
struct pixman_region32 {
    int32_t extents_x1, extents_y1, extents_x2, extents_y2;
    void* data;
};

typedef struct pixman_box32 pixman_box32_t;
struct pixman_box32 {
    int32_t x1, y1, x2, y2;
};

#ifdef __cplusplus
}
#endif

#endif /* PIXMAN_H */
