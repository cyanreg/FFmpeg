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

#ifndef AVCODEC_FFV1_VULKAN_COMMON_H
#define AVCODEC_FFV1_VULKAN_COMMON_H

#include "ffv1.h"
#include "libavutil/vulkan.h"
#include "libavutil/vulkan_spirv.h"

extern const char *ff_source_common_comp;
extern const char *ff_source_rangecoder_comp;
extern const char *ff_source_ffv1_vlc_comp;
extern const char *ff_source_ffv1_common_comp;
extern const char *ff_source_ffv1_reset_comp;

typedef struct FFv1VkResetParameters {
    VkDeviceAddress slice_state;
    uint32_t plane_state_size;
    uint32_t context_count;
    uint8_t codec_planes;
    uint8_t key_frame;
    uint8_t padding[3];
} FFv1VkResetParameters;

void ff_ffv1_vk_define_shared_code(FFV1Context *f, FFVulkanShader *shd);

int ff_ffv1_vk_init_reset_shader(FFV1Context *f, FFVulkanContext *s,
                                 FFVkExecPool *pool, FFVkSPIRVCompiler *spv,
                                 FFVulkanShader *shd);

#endif /* AVCODEC_FFV1_VULKAN_COMMON_H */
