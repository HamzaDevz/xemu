/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "texture.h"
#include "util.h"

#ifdef __aarch64__
#include <arm_neon.h>
#endif

const BasicColorFormatInfo kelvin_color_format_info_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8] = { 4, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = { 1, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8] = { 1, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8] = { 1, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8] = { 2, true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] = { 2, false },

    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8] = { 2, true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] = { 2, false, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] = { 4, true,
                                                                     true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT] = { 4, true,
                                                                     true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] = { 2, true,
                                                                  true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] = { 2, true,
                                                                  true },

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y16] = { 2, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] = { 2, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8] = { 4, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8] = { 4, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8] = { 4, true },
};

hwaddr pgraph_get_texture_phys_addr(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    int i = texture_idx;

    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i*4);
    unsigned int dma_select =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);

    hwaddr offset = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i*4);

    hwaddr dma_len;
    uint8_t *texture_data;
    if (dma_select) {
        texture_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &dma_len);
    } else {
        texture_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &dma_len);
    }
    assert(offset < dma_len);
    texture_data += offset;

    return texture_data - d->vram_ptr;
}

hwaddr pgraph_get_texture_palette_phys_addr_length(PGRAPHState *pg, int texture_idx, size_t *length)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    int i = texture_idx;

    uint32_t palette = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + i*4);
    bool palette_dma_select =
        GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA);
    unsigned int palette_length_index =
        GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_LENGTH);
    unsigned int palette_offset =
        palette & NV_PGRAPH_TEXPALETTE0_OFFSET;

    unsigned int palette_length = 0;
    switch (palette_length_index) {
    case NV_PGRAPH_TEXPALETTE0_LENGTH_256: palette_length = 256; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_128: palette_length = 128; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_64: palette_length = 64; break;
    case NV_PGRAPH_TEXPALETTE0_LENGTH_32: palette_length = 32; break;
    default: assert(false); break;
    }
    if (length) {
        *length = palette_length * 4;
    }

    hwaddr palette_dma_len;
    uint8_t *palette_data;
    if (palette_dma_select) {
        palette_data = (uint8_t*)nv_dma_map(d, pg->dma_b, &palette_dma_len);
    } else {
        palette_data = (uint8_t*)nv_dma_map(d, pg->dma_a, &palette_dma_len);
    }
    assert(palette_offset < palette_dma_len);
    palette_data += palette_offset;

    return palette_data - d->vram_ptr;
}

size_t pgraph_get_texture_length(PGRAPHState *pg, TextureShape *shape)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[shape->color_format];
    size_t length = 0;

    if (f.linear) {
        assert(shape->cubemap == false);
        assert(shape->dimensionality == 2);
        length = shape->height * shape->pitch;
    } else {
        if (shape->dimensionality >= 2) {
            unsigned int w = shape->width, h = shape->height;
            int level;
            if (!pgraph_is_texture_format_compressed(pg, shape->color_format)) {
                for (level = 0; level < shape->levels; level++) {
                    w = MAX(w, 1);
                    h = MAX(h, 1);
                    length += w * h * f.bytes_per_pixel;
                    w /= 2;
                    h /= 2;
                }
            } else {
                /* Compressed textures are a bit different */
                unsigned int block_size =
                    shape->color_format ==
                            NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                        8 : 16;
                for (level = 0; level < shape->levels; level++) {
                    w = MAX(w, 1);
                    h = MAX(h, 1);
                    unsigned int phys_w = (w + 3) & ~3,
                                 phys_h = (h + 3) & ~3;
                    length += phys_w/4 * phys_h/4 * block_size;
                    w /= 2;
                    h /= 2;
                }
            }
            if (shape->cubemap) {
                assert(shape->dimensionality == 2);
                length = (length + NV2A_CUBEMAP_FACE_ALIGNMENT - 1) & ~(NV2A_CUBEMAP_FACE_ALIGNMENT - 1);
                length *= 6;
            }
            if (shape->dimensionality >= 3) {
                length *= shape->depth;
            }
        }
    }

    return length;
}

TextureShape pgraph_get_texture_shape(PGRAPHState *pg, int texture_idx)
{
    int i = texture_idx;

    uint32_t ctl_0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i*4);
    uint32_t ctl_1 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i*4);
    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i*4);

#if DEBUG_NV2A
    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i*4);
    uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i*4);
#endif

    unsigned int min_mipmap_level =
        GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MIN_LOD_CLAMP);
    unsigned int max_mipmap_level =
        GET_MASK(ctl_0, NV_PGRAPH_TEXCTL0_0_MAX_LOD_CLAMP);

    unsigned int pitch =
        GET_MASK(ctl_1, NV_PGRAPH_TEXCTL1_0_IMAGE_PITCH);

    bool cubemap =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
    unsigned int dimensionality =
        GET_MASK(fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);

    int tex_mode = (pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (texture_idx * 5)) & 0x1F;
    if (tex_mode == 0x02) {
        assert(pgraph_is_texture_enabled(pg, texture_idx));
        // assert(state.dimensionality == 3);

        // OVERRIDE
        // dimensionality = 3;
    }

    unsigned int color_format = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_COLOR);
    unsigned int levels = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
    unsigned int log_width = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
    unsigned int log_height = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
    unsigned int log_depth = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);

    unsigned int rect_width =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i*4),
                 NV_PGRAPH_TEXIMAGERECT0_WIDTH);
    unsigned int rect_height =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i*4),
                 NV_PGRAPH_TEXIMAGERECT0_HEIGHT);
#if DEBUG_NV2A
    unsigned int lod_bias =
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
#endif
    unsigned int border_source = GET_MASK(fmt,
                                          NV_PGRAPH_TEXFMT0_BORDER_SOURCE);

    NV2A_DPRINTF(" texture %d is format 0x%x, "
                    "off 0x%" HWADDR_PRIx " (r %d, %d or %d, %d, %d; %d%s),"
                    " filter %x %x, levels %d-%d %d bias %d\n",
                 i, color_format, address,
                 rect_width, rect_height,
                 1 << log_width, 1 << log_height, 1 << log_depth,
                 pitch,
                 cubemap ? "; cubemap" : "",
                 GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN),
                 GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG),
                 min_mipmap_level, max_mipmap_level, levels,
                 lod_bias);

    assert(color_format < ARRAY_SIZE(kelvin_color_format_info_map));
    BasicColorFormatInfo f = kelvin_color_format_info_map[color_format];
    if (f.bytes_per_pixel == 0) {
        fprintf(stderr, "nv2a: unimplemented texture color format 0x%x\n",
                color_format);
        abort();
    }

    unsigned int width, height, depth;
    if (f.linear) {
        assert(dimensionality == 2);
        width = rect_width;
        height = rect_height;
        depth = 1;
    } else {
        width = 1 << log_width;
        height = 1 << log_height;
        depth = 1 << log_depth;
        pitch = 0;

        levels = MIN(levels, max_mipmap_level + 1);

        /* Discard mipmap levels that would be smaller than 1x1.
         * FIXME: Is this actually needed?
         *
         * >> Level 0: 32 x 4
         *    Level 1: 16 x 2
         *    Level 2: 8 x 1
         *    Level 3: 4 x 1
         *    Level 4: 2 x 1
         *    Level 5: 1 x 1
         */
        levels = MIN(levels, MAX(log_width, log_height) + 1);
        assert(levels > 0);

        if (dimensionality == 3) {
            /* FIXME: What about 3D mipmaps? */
            if (log_width < 2 || log_height < 2) {
                /* Base level is smaller than 4x4... */
                levels = 1;
            } else {
                levels = MIN(levels, MIN(log_width, log_height) - 1);
            }
        }
        min_mipmap_level = MIN(levels-1, min_mipmap_level);
        max_mipmap_level = MIN(levels-1, max_mipmap_level);
    }

    TextureShape shape;

    // We will hash it, so make sure any padding is zero
    memset(&shape, 0, sizeof(shape));

    shape.cubemap = cubemap;
    shape.dimensionality = dimensionality;
    shape.color_format = color_format;
    shape.levels = levels;
    shape.width = width;
    shape.height = height;
    shape.depth = depth;
    shape.min_mipmap_level = min_mipmap_level;
    shape.max_mipmap_level = max_mipmap_level;
    shape.pitch = pitch;
    shape.border = border_source != NV_PGRAPH_TEXFMT0_BORDER_SOURCE_COLOR;
    return shape;
}

/*
 * One line of YUY2 (Y0 U Y1 V) or UYVY (U Y0 V Y1) to RGBA, with the same
 * results as convert_yuy2_to_rgb() and convert_uyvy_to_rgb(). Video textures
 * are converted again on every upload, sixteen pixels at a time with NEON.
 */
static void convert_yuv422_line(const uint8_t *line, uint8_t *pixel,
                                unsigned int width, bool uyvy)
{
    unsigned int x = 0;

#ifdef __aarch64__
    const int16x8_t k128 = vdupq_n_s16(128);
    const uint8x16_t alpha = vdupq_n_u8(255);

    for (; x + 16 <= width; x += 16, pixel += 64) {
        uint8x8x4_t in = vld4_u8(line + x * 2);
        uint8x8_t y0 = uyvy ? in.val[1] : in.val[0];
        uint8x8_t u = uyvy ? in.val[0] : in.val[1];
        uint8x8_t y1 = uyvy ? in.val[3] : in.val[2];
        uint8x8_t v = uyvy ? in.val[2] : in.val[3];
        int16x8_t d = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u)), k128);
        int16x8_t e = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v)), k128);
        int16x8_t c0 = vreinterpretq_s16_u16(vsubl_u8(y0, vdup_n_u8(16)));
        int16x8_t c1 = vreinterpretq_s16_u16(vsubl_u8(y1, vdup_n_u8(16)));
        int32x4_t chroma[2][3];
        uint8x8_t out[3][2];

        for (int h = 0; h < 2; h++) {
            int16x4_t dh = h ? vget_high_s16(d) : vget_low_s16(d);
            int16x4_t eh = h ? vget_high_s16(e) : vget_low_s16(e);
            chroma[h][0] = vmlal_n_s16(vdupq_n_s32(128), eh, 409);
            chroma[h][1] = vmlal_n_s16(vmlal_n_s16(vdupq_n_s32(128), dh, -100),
                                       eh, -208);
            chroma[h][2] = vmlal_n_s16(vdupq_n_s32(128), dh, 516);
        }
        for (int k = 0; k < 2; k++) {
            int16x8_t c = k ? c1 : c0;
            int32x4_t luma[2] = {
                vmull_n_s16(vget_low_s16(c), 298),
                vmull_n_s16(vget_high_s16(c), 298),
            };
            for (int i = 0; i < 3; i++) {
                uint16x4_t lo = vqmovun_s32(
                    vshrq_n_s32(vaddq_s32(luma[0], chroma[0][i]), 8));
                uint16x4_t hi = vqmovun_s32(
                    vshrq_n_s32(vaddq_s32(luma[1], chroma[1][i]), 8));
                out[i][k] = vqmovn_u16(vcombine_u16(lo, hi));
            }
        }
        /* even pixels come from Y0, odd ones from Y1 */
        uint8x16x4_t rgba;
        for (int i = 0; i < 3; i++) {
            uint8x8x2_t z = vzip_u8(out[i][0], out[i][1]);
            rgba.val[i] = vcombine_u8(z.val[0], z.val[1]);
        }
        rgba.val[3] = alpha;
        vst4q_u8(pixel, rgba);
    }
#endif

    for (; x < width; x++, pixel += 4) {
        if (uyvy) {
            convert_uyvy_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
        } else {
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
        }
        pixel[3] = 255;
    }
}

uint8_t *pgraph_convert_texture_data(const TextureShape s, const uint8_t *data,
                                     const uint8_t *palette_data,
                                     unsigned int width, unsigned int height,
                                     unsigned int depth, unsigned int row_pitch,
                                     unsigned int slice_pitch,
                                     size_t *converted_size)
{
    size_t size = 0;
    uint8_t *converted_data;

    if (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        size = width * height * depth * 4;
        converted_data = g_malloc(size);
        const uint8_t *src = data;
        uint32_t *dst = (uint32_t *)converted_data;
        for (int z = 0; z < depth; z++) {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    uint8_t index = src[y * row_pitch + x];
                    uint32_t color = *(uint32_t *)(palette_data + index * 4);
                    *dst++ = color;
                }
            }
            src += slice_pitch;
        }
    } else if (s.color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
               s.color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8) {
        // TODO: Investigate whether a non-1 depth is possible.
        // Generally the hardware asserts when attempting to use volumetric
        // textures in linear formats.
        assert(depth == 1); /* FIXME */
        // FIXME: only valid if control0 register allows for colorspace
        // conversion
        size = width * height * 4;
        converted_data = g_malloc(size);
        for (int y = 0; y < height; y++) {
            convert_yuv422_line(&data[y * row_pitch * depth],
                                &converted_data[y * width * 4], width,
                                s.color_format !=
                                NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8);
        }
    } else if (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
        assert(depth == 1); /* FIXME */
        /* Expanded to four components rather than the three this format
         * actually carries: three-component 8-bit formats are widely
         * unsupported for sampling. Vulkan does not require an implementation
         * to expose R8G8B8_SNORM at all, and Adreno reports no features for
         * it, which would leave the texture unusable.
         */
        size = width * height * 4;
        converted_data = g_malloc(size);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint16_t rgb655 = *(uint16_t *)(data + y * row_pitch + x * 2);
                int8_t *pixel = (int8_t *)&converted_data[(y * width + x) * 4];
                /* Maps 5 bit G and B signed value range to 8 bit
                 * signed values. R is probably unsigned.
                 */
                rgb655 ^= (1 << 9) | (1 << 4);
                pixel[0] = ((rgb655 & 0xFC00) >> 10) * 0x7F / 0x3F;
                pixel[1] = ((rgb655 & 0x03E0) >> 5) * 0xFF / 0x1F - 0x80;
                pixel[2] = (rgb655 & 0x001F) * 0xFF / 0x1F - 0x80;
                /* 0x7F is 1.0 in SNORM, so alpha still reads as it did when
                 * this was sampled through a three-component format.
                 */
                pixel[3] = 0x7F;
            }
        }
    } else {
        return NULL;
    }

    if (converted_size) {
        *converted_size = size;
    }
    return converted_data;
}
