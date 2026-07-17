#ifndef GLSLANG_C_INTERFACE_H
#define GLSLANG_C_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* glslang_shader_t;
typedef void* glslang_program_t;
typedef void* glslang_resource_t;

typedef enum glslang_stage_t {
    GLSLANG_STAGE_VERTEX = 0,
    GLSLANG_STAGE_FRAGMENT = 4
} glslang_stage_t;

typedef struct glsl_include_result_t {
    const char* header_name;
    const char* header_data;
    size_t header_length;
} glsl_include_result_t;

typedef struct glsl_include_callbacks_t {
    void* include_system;
    void* include_local;
    void* free_include_result;
} glsl_include_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif /* GLSLANG_C_INTERFACE_H */
