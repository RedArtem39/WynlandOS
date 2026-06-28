/*
 * WynlandOS - MiniGL (Software OpenGL implementation) Header
 * ============================================================
 */
#pragma once

#include <wynland/types.h>

typedef float          GLfloat;
typedef int            GLint;
typedef unsigned int   GLenum;
typedef unsigned int   GLbitfield;
typedef int            GLsizei;
typedef unsigned char  GLboolean;

#define GL_FALSE            0
#define GL_TRUE             1

/* Buffer masks */
#define GL_COLOR_BUFFER_BIT   (1 << 0)
#define GL_DEPTH_BUFFER_BIT   (1 << 1)

/* Primitives */
#define GL_TRIANGLES          0x0004
#define GL_QUADS              0x0007

/* Matrix modes */
#define GL_MODELVIEW          0x1700
#define GL_PROJECTION         0x1701

/* Capabilities */
#define GL_DEPTH_TEST         0x0B71

#ifdef __cplusplus
extern "C" {
#endif

/* MiniGL context management */
void glInit(int width, int height, uint32_t *color_buffer);
void glDeinit(void);

/* Viewport and buffers */
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear(GLbitfield mask);

/* Matrix operations */
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void gluPerspective(GLfloat fovy, GLfloat aspect, GLfloat zNear, GLfloat zFar);

/* Capabilities */
void glEnable(GLenum cap);
void glDisable(GLenum cap);

/* Primitive assembly */
void glBegin(GLenum mode);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glEnd(void);

#ifdef __cplusplus
}
#endif
