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

#include "vulkan_decode.h"
#include "hwaccel_internal.h"

#include "ffv1_vulkan_common.h"

extern const char *ff_source_common_comp;
extern const char *ff_source_rangecoder_comp;
extern const char *ff_source_ffv1_vlc_comp;
extern const char *ff_source_ffv1_common_comp;
extern const char *ff_source_ffv1_reset_comp;

const FFVulkanDecodeDescriptor ff_vk_dec_ffv1_desc = {
    .codec_id         = AV_CODEC_ID_FFV1,
    .decode_extension = FF_VK_EXT_PUSH_DESCRIPTOR,
    .queue_flags      = VK_QUEUE_COMPUTE_BIT,
};

typedef struct FFv1VulkanDecodePicture {
    FFVulkanDecodePicture vp;

    VkImageView  img_view_rct;
    AVFrame     *rct;
} FFv1VulkanDecodePicture;

static void vk_ffv1_free_frame_priv(AVRefStructOpaque _hwctx, void *data)
{
    AVHWDeviceContext *dev_hwctx = _hwctx.nc;
    AVVulkanDeviceContext *hwctx = dev_hwctx->hwctx;

    FFv1VulkanDecodePicture *fp = data;

//    FFVulkanDecodeSDContext *dec = avctx->internal->hwaccel_priv_data;
//    FFVulkanDecodeSDShared *ctx = dec->shared_ctx;
//    FFVulkanFunctions *vk = &ctx->s.vkfn;

//    ff_vk_decode_sd_free_frame(dev_hwctx, &fp->vp);

//    vk->DestroyImageView(hwctx->act_dev, fp->img_view_rct, hwctx->alloc);
    av_frame_free(&fp->rct);
}

static int vk_ffv1_start_frame(AVCodecContext          *avctx,
                               av_unused const uint8_t *buffer,
                               av_unused uint32_t       size)
{


    return 0;
}

static int vk_ffv1_decode_slice(AVCodecContext *avctx,
                                const uint8_t  *data,
                                uint32_t        size)
{
    return 0;
}

static int vk_ffv1_end_frame(AVCodecContext *avctx)
{
    return 0;
}



static int vk_decode_ffv1_init(AVCodecContext *avctx)
{
    int err;

    err = ff_vk_decode_init(avctx);
    if (err < 0)
        return err;




    return 0;
}

const FFHWAccel ff_ffv1_vulkan_hwaccel = {
    .p.name                = "ffv1_vulkan",
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_FFV1,
    .p.pix_fmt             = AV_PIX_FMT_VULKAN,
    .start_frame           = &vk_ffv1_start_frame,
    .decode_slice          = &vk_ffv1_decode_slice,
    .end_frame             = &vk_ffv1_end_frame,
    .free_frame_priv       = &vk_ffv1_free_frame_priv,
    .frame_priv_data_size  = sizeof(FFv1VulkanDecodePicture),
    .init                  = &vk_decode_ffv1_init,
    .update_thread_context = &ff_vk_update_thread_context,
    .decode_params         = &ff_vk_params_invalidate,
    .flush                 = &ff_vk_decode_flush,
    .uninit                = &ff_vk_decode_uninit,
    .frame_params          = &ff_vk_frame_params,
    .priv_data_size        = sizeof(FFVulkanDecodeContext),
    .caps_internal         = HWACCEL_CAP_ASYNC_SAFE | HWACCEL_CAP_THREAD_SAFE,
};
