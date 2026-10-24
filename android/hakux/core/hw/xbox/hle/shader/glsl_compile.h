/*
 * hakuX HLE GPU — GLSL 4.50 -> SPIR-V 1.6 (Vulkan 1.3) via glslang C API.
 */
#ifndef HW_XBOX_HLE_SHADER_GLSL_COMPILE_H
#define HW_XBOX_HLE_SHADER_GLSL_COMPILE_H

#include <stddef.h>
#include <stdint.h>

typedef enum HleShaderStage {
    HLE_SHADER_VERTEX = 0,
    HLE_SHADER_FRAGMENT = 1,
    HLE_SHADER_COMPUTE = 2,
} HleShaderStage;

void hle_glsl_init(void);
void hle_glsl_finalize(void);
/* Returns g_malloc'd SPIR-V words (free with g_free) or NULL; logs errors
 * with tag hakuX-hle-glsl. */
uint32_t *hle_glsl_compile(HleShaderStage stage, const char *src,
                           size_t *nwords);

#endif
