/*
 * Copyright (c) 2024 Lynne <dev@lynne.ee>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ffv1_vulkan_common.h"

void ff_ffv1_vk_define_shared_code(FFV1Context *f, FFVulkanShader *shd)
{
    int smp_bits = f->use32bit ? 32 : 16;

    av_bprintf(&shd->src, "#define CONTEXT_SIZE %i\n"                    ,CONTEXT_SIZE);
    av_bprintf(&shd->src, "#define MAX_QUANT_TABLE_MASK 0x%x\n"          ,MAX_QUANT_TABLE_MASK);

    if (f->ac == AC_GOLOMB_RICE) {
        av_bprintf(&shd->src, "#define PB_UNALIGNED\n"                   );
        av_bprintf(&shd->src, "#define GOLOMB\n"                         );
    }

    GLSLF(0, #define TYPE int%i_t                                        ,smp_bits);
    GLSLF(0, #define VTYPE2 i%ivec2                                      ,smp_bits);
    GLSLF(0, #define VTYPE3 i%ivec3                                      ,smp_bits);
    GLSLD(ff_source_common_comp);
    GLSLD(ff_source_rangecoder_comp);

    if (f->ac == AC_GOLOMB_RICE)
        GLSLD(ff_source_ffv1_vlc_comp);

    GLSLD(ff_source_ffv1_common_comp);
}

int ff_ffv1_vk_init_reset_shader(FFV1Context *f, FFVulkanContext *s,
                                 FFVkExecPool *pool, FFVkSPIRVCompiler *spv,
                                 FFVulkanShader *shd)
{
    int err;
    FFVulkanDescriptorSetBinding *desc_set;

    uint8_t *spv_data;
    size_t spv_len;
    void *spv_opaque = NULL;
    int wg_dim = FFMIN(s->props.properties.limits.maxComputeWorkGroupSize[0], 1024);

    RET(ff_vk_shader_init(s, shd, "ffv1_reset",
                          VK_SHADER_STAGE_COMPUTE_BIT,
                          (const char *[]) { "GL_EXT_buffer_reference",
                                             "GL_EXT_buffer_reference2" }, 2,
                          wg_dim, 1, 1,
                          0));

    av_bprintf(&shd->src, "#define MAX_QUANT_TABLES %i\n", MAX_QUANT_TABLES);
    av_bprintf(&shd->src, "#define MAX_CONTEXT_INPUTS %i\n", MAX_CONTEXT_INPUTS);
    av_bprintf(&shd->src, "#define MAX_QUANT_TABLE_SIZE %i\n", MAX_QUANT_TABLE_SIZE);

    desc_set = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "rangecoder_static_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "uint8_t zero_one_state[512];",
        },
        {
            .name        = "quant_buf",
            .type        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .mem_layout  = "scalar",
            .buf_content = "int16_t quant_table[MAX_QUANT_TABLES]"
                           "[MAX_CONTEXT_INPUTS][MAX_QUANT_TABLE_SIZE];",
        },
    };
    RET(ff_vk_shader_add_descriptor_set(s, shd, desc_set, 2, 1, 0));

    ff_ffv1_vk_define_shared_code(f, shd);

    desc_set = (FFVulkanDescriptorSetBinding []) {
        {
            .name        = "slice_data_buf",
            .type        = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .mem_quali   = "readonly",
            .stages      = VK_SHADER_STAGE_COMPUTE_BIT,
            .buf_content = "SliceContext slice_ctx[1024];",
        },
    };
    RET(ff_vk_shader_add_descriptor_set(s, shd, desc_set, 1, 0, 0));

    GLSLC(0, layout(push_constant, scalar) uniform pushConstants {             );
    GLSLC(1,    u8buf slice_state;                                             );
    GLSLC(1,    uint plane_state_size;                                         );
    GLSLC(1,    uint context_count;                                            );
    GLSLC(1,    uint8_t codec_planes;                                          );
    GLSLC(1,    uint8_t key_frame;                                             );
    GLSLC(1,    uint8_t padding[3];                                            );
    GLSLC(0, };                                                                );
    ff_vk_shader_add_push_const(shd, 0, sizeof(FFv1VkResetParameters),
                                VK_SHADER_STAGE_COMPUTE_BIT);

    GLSLD(ff_source_ffv1_reset_comp);

    RET(spv->compile_shader(s, spv, shd, &spv_data, &spv_len, "main",
                            &spv_opaque));
    RET(ff_vk_shader_link(s, shd, spv_data, spv_len, "main"));

    RET(ff_vk_shader_register_exec(s, pool, shd));

fail:
    if (spv_opaque)
        spv->free_shader(spv, &spv_opaque);

    return err;
}
