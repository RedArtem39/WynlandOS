/*
 * WynlandOS - MiniGL (Software OpenGL implementation) Source
 * ============================================================
 */
#include "opengl.h"
#include <wynland/heap.h>

#define M_PI 3.14159265f

extern "C" {
void *memset(void *s, int c, size_t n);
void serial_write_string(const char *str);
}

struct GLVertex {
    float sx, sy, sz; // Screen coordinates
    float r, g, b;    // Interpolated colors
};

struct GLContext {
    uint32_t *color_buffer;
    float *depth_buffer;
    int width, height;

    /* Viewport */
    int vp_x, vp_y, vp_w, vp_h;

    /* Matrix stacks */
    float modelview[16];
    float projection[16];
    float *current_matrix;
    GLenum matrix_mode;

    /* State flags */
    uint32_t clear_color_hex;
    bool depth_test;

    /* Current attributes */
    float cur_r, cur_g, cur_b;

    /* Primitive building */
    GLenum prim_mode;
    struct {
        float x, y, z;
        float r, g, b;
    } build_vertices[256];
    int build_count;
};

static GLContext g_ctx;
static bool g_gl_initialized = false;

/* Math Helpers */
static inline float sin_f(float x) {
    while (x > M_PI) x -= 2.0f * M_PI;
    while (x < -M_PI) x += 2.0f * M_PI;
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666666f - x2 * (0.00833333f - x2 * 0.00019841f)));
}

static inline float cos_f(float x) {
    return sin_f(x + 1.57079632f);
}

static inline float tan_f(float x) {
    return sin_f(x) / cos_f(x);
}

static void mat_identity(float *m) {
    m[0]=1; m[1]=0; m[2]=0; m[3]=0;
    m[4]=0; m[5]=1; m[6]=0; m[7]=0;
    m[8]=0; m[9]=0; m[10]=1; m[11]=0;
    m[12]=0; m[13]=0; m[14]=0; m[15]=1;
}

static void mat_mul(float *out, const float *a, const float *b) {
    float tmp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) {
                tmp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
            }
        }
    }
    for (int i = 0; i < 16; i++) out[i] = tmp[i];
}

/* API implementation */

void glInit(int width, int height, uint32_t *color_buffer) {
    g_ctx.color_buffer = color_buffer;
    g_ctx.width = width;
    g_ctx.height = height;

    /* Default viewport */
    g_ctx.vp_x = 0;
    g_ctx.vp_y = 0;
    g_ctx.vp_w = width;
    g_ctx.vp_h = height;

    /* Allocate depth buffer (cached to avoid frame-by-frame heap allocations) */
    static float *s_depth_buffer = 0;
    static int s_depth_capacity = 0;
    int needed = width * height;
    if (!s_depth_buffer || needed > s_depth_capacity) {
        if (s_depth_buffer) {
            kfree(s_depth_buffer);
        }
        s_depth_buffer = (float *)kmalloc(needed * sizeof(float));
        s_depth_capacity = needed;
    }
    g_ctx.depth_buffer = s_depth_buffer;

    /* Initialize matrices */
    mat_identity(g_ctx.modelview);
    mat_identity(g_ctx.projection);
    g_ctx.current_matrix = g_ctx.modelview;
    g_ctx.matrix_mode = GL_MODELVIEW;

    g_ctx.clear_color_hex = 0x00000000;
    g_ctx.depth_test = true;

    g_ctx.cur_r = 1.0f;
    g_ctx.cur_g = 1.0f;
    g_ctx.cur_b = 1.0f;

    g_ctx.build_count = 0;
    g_gl_initialized = true;

    serial_write_string("MiniGL: Initialized successfully.\r\n");
}

void glDeinit(void) {
    /* Keep depth buffer cached, do not free it here to save CPU cycles */
    g_ctx.depth_buffer = 0;
    g_gl_initialized = false;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_ctx.vp_x = x;
    g_ctx.vp_y = y;
    g_ctx.vp_w = width;
    g_ctx.vp_h = height;
}

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    (void)a;
    uint32_t ur = (uint32_t)(r * 255.0f) & 0xFF;
    uint32_t ug = (uint32_t)(g * 255.0f) & 0xFF;
    uint32_t ub = (uint32_t)(b * 255.0f) & 0xFF;
    g_ctx.clear_color_hex = (ur << 16) | (ug << 8) | ub;
}

void glClear(GLbitfield mask) {
    if (!g_gl_initialized) return;

    int vp_x = g_ctx.vp_x;
    int vp_y = g_ctx.vp_y;
    int vp_w = g_ctx.vp_w;
    int vp_h = g_ctx.vp_h;
    int bw = g_ctx.width;

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t val = g_ctx.clear_color_hex;
        for (int y = vp_y; y < vp_y + vp_h; y++) {
            int row_offset = y * bw;
            for (int x = vp_x; x < vp_x + vp_w; x++) {
                g_ctx.color_buffer[row_offset + x] = val;
            }
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        float far_val = 1.0f;
        for (int y = vp_y; y < vp_y + vp_h; y++) {
            int row_offset = y * bw;
            for (int x = vp_x; x < vp_x + vp_w; x++) {
                g_ctx.depth_buffer[row_offset + x] = far_val;
            }
        }
    }
}

void glMatrixMode(GLenum mode) {
    if (mode == GL_PROJECTION) {
        g_ctx.current_matrix = g_ctx.projection;
        g_ctx.matrix_mode = GL_PROJECTION;
    } else {
        g_ctx.current_matrix = g_ctx.modelview;
        g_ctx.matrix_mode = GL_MODELVIEW;
    }
}

void glLoadIdentity(void) {
    mat_identity(g_ctx.current_matrix);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    float trans[16];
    mat_identity(trans);
    trans[3] = x;
    trans[7] = y;
    trans[11] = z;
    mat_mul(g_ctx.current_matrix, g_ctx.current_matrix, trans);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    /* Normalize axis */
    float len = x*x + y*y + z*z;
    if (len == 0.0f) return;
    
    // Quick square root approximation for normalization
    float rlen = 1.0f;
    float temp_len = len;
    for (int i = 0; i < 6; i++) {
        rlen = 0.5f * rlen * (3.0f - temp_len * rlen * rlen);
    }
    x *= rlen;
    y *= rlen;
    z *= rlen;

    float rad = angle * M_PI / 180.0f;
    float c = cos_f(rad);
    float s = sin_f(rad);
    float t = 1.0f - c;

    float rot[16];
    rot[0] = x*x*t + c;   rot[1] = x*y*t - z*s; rot[2] = x*z*t + y*s; rot[3] = 0;
    rot[4] = y*x*t + z*s; rot[5] = y*y*t + c;   rot[6] = y*z*t - x*s; rot[7] = 0;
    rot[8] = z*x*t - y*s; rot[9] = z*y*t + x*s; rot[10] = z*z*t + c;  rot[11] = 0;
    rot[12] = 0;          rot[13] = 0;          rot[14] = 0;          rot[15] = 1;

    mat_mul(g_ctx.current_matrix, g_ctx.current_matrix, rot);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    float scale[16];
    mat_identity(scale);
    scale[0] = x;
    scale[5] = y;
    scale[10] = z;
    mat_mul(g_ctx.current_matrix, g_ctx.current_matrix, scale);
}

void gluPerspective(GLfloat fovy, GLfloat aspect, GLfloat zNear, GLfloat zFar) {
    float f = 1.0f / tan_f(fovy * 0.5f * M_PI / 180.0f);
    float proj[16];
    memset(proj, 0, sizeof(proj));

    proj[0] = f / aspect;
    proj[5] = f;
    proj[10] = (zFar + zNear) / (zNear - zFar);
    proj[11] = (2.0f * zFar * zNear) / (zNear - zFar);
    proj[14] = -1.0f;

    mat_mul(g_ctx.projection, g_ctx.projection, proj);
}

void glEnable(GLenum cap) {
    if (cap == GL_DEPTH_TEST) {
        g_ctx.depth_test = true;
    }
}

void glDisable(GLenum cap) {
    if (cap == GL_DEPTH_TEST) {
        g_ctx.depth_test = false;
    }
}

void glBegin(GLenum mode) {
    g_ctx.prim_mode = mode;
    g_ctx.build_count = 0;
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b) {
    g_ctx.cur_r = r;
    g_ctx.cur_g = g;
    g_ctx.cur_b = b;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (g_ctx.build_count >= 256) return;
    g_ctx.build_vertices[g_ctx.build_count].x = x;
    g_ctx.build_vertices[g_ctx.build_count].y = y;
    g_ctx.build_vertices[g_ctx.build_count].z = z;
    g_ctx.build_vertices[g_ctx.build_count].r = g_ctx.cur_r;
    g_ctx.build_vertices[g_ctx.build_count].g = g_ctx.cur_g;
    g_ctx.build_vertices[g_ctx.build_count].b = g_ctx.cur_b;
    g_ctx.build_count++;
}

/* Rasterize flat/Gouraud shaded triangle with depth test */
static void rasterize_triangle(GLVertex v0, GLVertex v1, GLVertex v2) {
    /* Viewport clamping */
    int min_x = (int)v0.sx; if (v1.sx < min_x) min_x = (int)v1.sx; if (v2.sx < min_x) min_x = (int)v2.sx;
    int max_x = (int)v0.sx; if (v1.sx > max_x) max_x = (int)v1.sx; if (v2.sx > max_x) max_x = (int)v2.sx;
    int min_y = (int)v0.sy; if (v1.sy < min_y) min_y = (int)v1.sy; if (v2.sy < min_y) min_y = (int)v2.sy;
    int max_y = (int)v0.sy; if (v1.sy > max_y) max_y = (int)v1.sy; if (v2.sy > max_y) max_y = (int)v2.sy;

    if (min_x < g_ctx.vp_x) min_x = g_ctx.vp_x;
    if (min_y < g_ctx.vp_y) min_y = g_ctx.vp_y;
    if (max_x >= g_ctx.vp_x + g_ctx.vp_w) max_x = g_ctx.vp_x + g_ctx.vp_w - 1;
    if (max_y >= g_ctx.vp_y + g_ctx.vp_h) max_y = g_ctx.vp_y + g_ctx.vp_h - 1;

    float det = (v1.sy - v2.sy) * (v0.sx - v2.sx) + (v2.sx - v1.sx) * (v0.sy - v2.sy);
    if (det == 0.0f) return;
    float inv_det = 1.0f / det;

    uint32_t bw = g_ctx.width;

    /* Precompute incremental barycentric stepping factors to avoid redundant inner-loop multiplications */
    float factor0_x = (v1.sy - v2.sy) * inv_det;
    float factor0_y = (v2.sx - v1.sx) * inv_det;
    float const0    = - ((v1.sy - v2.sy) * v2.sx + (v2.sx - v1.sx) * v2.sy) * inv_det;

    float factor1_x = (v2.sy - v0.sy) * inv_det;
    float factor1_y = (v0.sx - v2.sx) * inv_det;
    float const1    = - ((v2.sy - v0.sy) * v2.sx + (v0.sx - v2.sx) * v2.sy) * inv_det;

    float z_factor0 = v0.sz - v2.sz;
    float z_factor1 = v1.sz - v2.sz;

    float r_factor0 = v0.r - v2.r;
    float r_factor1 = v1.r - v2.r;
    float g_factor0 = v0.g - v2.g;
    float g_factor1 = v1.g - v2.g;
    float b_factor0 = v0.b - v2.b;
    float b_factor1 = v1.b - v2.b;

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        float px_start = (float)min_x + 0.5f;

        /* Calculate starting values for this row */
        float w0_row = factor0_x * px_start + factor0_y * py + const0;
        float w1_row = factor1_x * px_start + factor1_y * py + const1;

        for (int x = min_x; x <= max_x; x++) {
            float w0 = w0_row;
            float w1 = w1_row;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                /* Depth Test */
                float z = w0 * z_factor0 + w1 * z_factor1 + v2.sz;
                int pixel_idx = y * bw + x;

                if (!g_ctx.depth_test || z < g_ctx.depth_buffer[pixel_idx]) {
                    if (g_ctx.depth_test) g_ctx.depth_buffer[pixel_idx] = z;

                    /* Interpolate color */
                    float r = w0 * r_factor0 + w1 * r_factor1 + v2.r;
                    float g = w0 * g_factor0 + w1 * g_factor1 + v2.g;
                    float b = w0 * b_factor0 + w1 * b_factor1 + v2.b;

                    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
                    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
                    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;

                    uint32_t ur = (uint32_t)(r * 255.0f);
                    uint32_t ug = (uint32_t)(g * 255.0f);
                    uint32_t ub = (uint32_t)(b * 255.0f);

                    g_ctx.color_buffer[pixel_idx] = (ur << 16) | (ug << 8) | ub;
                }
            }
            w0_row += factor0_x;
            w1_row += factor1_x;
        }
    }
}

void glEnd(void) {
    if (!g_gl_initialized) return;

    /* Transform vertices and project */
    GLVertex trans_verts[256];

    for (int i = 0; i < g_ctx.build_count; i++) {
        float x = g_ctx.build_vertices[i].x;
        float y = g_ctx.build_vertices[i].y;
        float z = g_ctx.build_vertices[i].z;

        /* Modelview transformation */
        float *mv = g_ctx.modelview;
        float cx = mv[0]*x + mv[1]*y + mv[2]*z + mv[3];
        float cy = mv[4]*x + mv[5]*y + mv[6]*z + mv[7];
        float cz = mv[8]*x + mv[9]*y + mv[10]*z + mv[11];
        float cw = mv[12]*x + mv[13]*y + mv[14]*z + mv[15];

        /* Projection transformation */
        float *proj = g_ctx.projection;
        float px = proj[0]*cx + proj[1]*cy + proj[2]*cz + proj[3]*cw;
        float py = proj[4]*cx + proj[5]*cy + proj[6]*cz + proj[7]*cw;
        float pz = proj[8]*cx + proj[9]*cy + proj[10]*cz + proj[11]*cw;
        float pw = proj[12]*cx + proj[13]*cy + proj[14]*cz + proj[15]*cw;

        if (pw == 0.0f) pw = 1.0f;
        float ndc_x = px / pw;
        float ndc_y = py / pw;
        float ndc_z = pz / pw;

        /* Viewport mapping */
        trans_verts[i].sx = (float)g_ctx.vp_x + (ndc_x + 1.0f) * 0.5f * (float)g_ctx.vp_w;
        trans_verts[i].sy = (float)g_ctx.vp_y + (1.0f - ndc_y) * 0.5f * (float)g_ctx.vp_h;
        trans_verts[i].sz = ndc_z;
        trans_verts[i].r  = g_ctx.build_vertices[i].r;
        trans_verts[i].g  = g_ctx.build_vertices[i].g;
        trans_verts[i].b  = g_ctx.build_vertices[i].b;
    }

    /* Rasterize */
    if (g_ctx.prim_mode == GL_TRIANGLES) {
        for (int i = 0; i < g_ctx.build_count - 2; i += 3) {
            rasterize_triangle(trans_verts[i], trans_verts[i+1], trans_verts[i+2]);
        }
    } else if (g_ctx.prim_mode == GL_QUADS) {
        for (int i = 0; i < g_ctx.build_count - 3; i += 4) {
            /* Split quad into two triangles */
            rasterize_triangle(trans_verts[i], trans_verts[i+1], trans_verts[i+2]);
            rasterize_triangle(trans_verts[i], trans_verts[i+2], trans_verts[i+3]);
        }
    }
}
