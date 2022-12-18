/*
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

#include "vulkan_encode.h"

#include <vk_video/vulkan_video_codecs_common.h> // TODO: REMOVE WHEN THE HEADERS BUG IS FIXED

const VkExtensionProperties ff_vk_enc_ext[AV_CODEC_ID_FIRST_AUDIO] = {
    [AV_CODEC_ID_H264] = (VkExtensionProperties) {
        .extensionName = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
        .specVersion   = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION,
    },
    [AV_CODEC_ID_HEVC] = (VkExtensionProperties) {
        .extensionName = VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_EXTENSION_NAME,
        .specVersion   = VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_SPEC_VERSION,
    },
};

const AVCodecHWConfigInternal *const ff_vulkan_encode_hw_configs[] = {
    HW_CONFIG_ENCODER_FRAMES(VULKAN, VULKAN),
    NULL,
};

av_cold void ff_vulkan_encode_uninit(FFVulkanEncodeContext *ctx)
{
    FFVulkanContext *s = &ctx->s;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    /* Wait on and free execution pool */
    ff_vk_exec_pool_free(s, &ctx->enc_pool);

    /* This also frees all references from this pool */
    av_frame_free(&ctx->layered_frame);
    av_buffer_unref(&ctx->dpb_hwfc_ref);

    /* Destroy YUV sampler */
    if (ctx->yuv_sampler)
        vk->DestroySamplerYcbcrConversion(s->hwctx->act_dev, ctx->yuv_sampler,
                                          s->hwctx->alloc);

    /* Destroy parameters */
    if (ctx->session_params)
        vk->DestroyVideoSessionParametersKHR(s->hwctx->act_dev, ctx->session_params,
                                             s->hwctx->alloc);

    ff_vk_video_common_uninit(s, &ctx->common);

    ff_vk_uninit(s);

    av_free(ctx->pic);
}

int ff_vk_encode_create_view(FFVulkanEncodeContext *ctx, VkImageView *dst_view,
                             VkImageAspectFlags *aspect, AVVkFrame *src, int layer)
{
    VkResult ret;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkImageAspectFlags aspect_mask = ff_vk_aspect_bits_from_vkfmt(ctx->pic_format);

    VkSamplerYcbcrConversionInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = ctx->yuv_sampler,
    };
    VkImageViewCreateInfo img_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &yuv_sampler_info,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ctx->pic_format,
        .image = src->img[0],
        .components = (VkComponentMapping) {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = layer,
            .layerCount     = 1,
            .levelCount     = 1,
        },
    };

    ret = vk->CreateImageView(ctx->s.hwctx->act_dev, &img_view_create_info,
                              ctx->s.hwctx->alloc, dst_view);
    if (ret != VK_SUCCESS)
        return AVERROR_EXTERNAL;

    *aspect = aspect_mask;

    return 0;
}

static AVFrame *vk_get_dpb_pool(FFVulkanEncodeContext *ctx)
{
    AVFrame *avf = av_frame_alloc();
    AVHWFramesContext *dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
    if (!avf)
        return NULL;

    avf->hw_frames_ctx = av_buffer_ref(ctx->dpb_hwfc_ref);
    if (!avf->hw_frames_ctx)
        av_frame_free(&avf);
    avf->buf[0] = av_buffer_pool_get(dpb_frames->pool);
    if (!avf->buf[0])
        av_frame_free(&avf);
    avf->data[0] = avf->buf[0]->data;

    return avf;
}

av_cold int ff_vulkan_encode_init(AVCodecContext *avctx, FFVulkanEncodeContext *ctx,
                                  void *codec_profile, void *caps,
                                  const FFVulkanEncoder *enc,
                                  int output_delay, int decode_delay)
{
    int i, err, qf;
    VkResult ret;
    int cxpos = 0, cypos = 0;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    FFVulkanContext *s = &ctx->s;
    FFVulkanExtensions extensions;
    const struct FFVkCodecMap *vk_codec = &ff_vk_codec_map[avctx->codec_id];
    const AVPixFmtDescriptor *desc;

    AVHWFramesContext *dpb_frames;
    AVVulkanFramesContext *dpb_hwfc;

    VkVideoFormatPropertiesKHR *ret_info;
    uint32_t nb_out_fmts = 0;

    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR query_create = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
        .encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                               VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR,
    };
    VkVideoSessionCreateInfoKHR session_create = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
    };
    VkPhysicalDeviceVideoFormatInfoKHR fmt_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        .pNext = &ctx->profile_list,
    };
    VkSamplerYcbcrConversionCreateInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .components = ff_comp_identity_map,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = avctx->color_range == AVCOL_RANGE_MPEG, /* Ignored */
    };

    if (!avctx->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the encoding device.\n");
        return AVERROR(EINVAL);
    }

    s->frames_ref = av_buffer_ref(avctx->hw_frames_ctx);
    s->frames = (AVHWFramesContext *)s->frames_ref->data;
    s->hwfc = s->frames->hwctx;

    s->device = (AVHWDeviceContext *)s->frames->device_ref->data;
    s->hwctx = s->device->hwctx;

    desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
    if (!desc)
        return AVERROR(EINVAL);

    ctx->enc          = enc;
    ctx->output_delay = output_delay;
    ctx->decode_delay = decode_delay;

    extensions = ff_vk_extensions_to_mask(s->hwctx->enabled_dev_extensions,
                                          s->hwctx->nb_enabled_dev_extensions);

    if (!(extensions & FF_VK_EXT_VIDEO_ENCODE_QUEUE)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support the %s extension!\n",
               VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
        return AVERROR(ENOSYS);
    } else if (!(vk_codec->encode_extension & extensions)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support decoding %s!\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    }

    /* Load functions */
    err = ff_vk_load_functions(s->device, vk, extensions, 1, 1);
    if (err < 0)
        return err;

    /* Load all properties */
    err = ff_vk_load_props(s);
    if (err < 0)
        return err;

    /* Create queue context */
    qf = ff_vk_qf_init(s, &ctx->qf_enc, VK_QUEUE_VIDEO_ENCODE_BIT_KHR);

    /* Check for support */
    if (!(s->video_props[qf].videoCodecOperations & vk_codec->encode_op)) {
        av_log(avctx, AV_LOG_ERROR, "Encoding %s not supported on the given "
               "queue family %i!\n", avcodec_get_name(avctx->codec_id), qf);
        return AVERROR(EINVAL);
    }

    /* Set tuning */
    ctx->usage_info = (VkVideoEncodeUsageInfoKHR) {
        .sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
        .pNext             = codec_profile,
        .videoUsageHints   = ctx->opts.usage,
        .videoContentHints = ctx->opts.content,
        .tuningMode        = ctx->opts.tune,
    };

    /* Load up the profile now, we need it to create a query pool */
    ctx->profile.sType               = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    ctx->profile.pNext               = &ctx->usage_info;
    ctx->profile.videoCodecOperation = vk_codec->encode_op;
    ctx->profile.chromaSubsampling   = ff_vk_subsampling_from_av_desc(desc);
    ctx->profile.lumaBitDepth        = ff_vk_depth_from_av_depth(desc->comp[0].depth);
    ctx->profile.chromaBitDepth      = ctx->profile.lumaBitDepth;

    query_create.pNext = &ctx->profile;

    /* Create command and query pool.
     * 18.12. Video Encode Bitstream Buffer Range Queries:
     * Two values are written, the buffer offset, and the number of bytes written. */
    err = ff_vk_exec_pool_init(s, &ctx->qf_enc, &ctx->enc_pool, 1,
                               1, VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR, 0,
                               &query_create);
    if (err < 0)
        return err;

    ctx->profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    ctx->profile_list.profileCount = 1;
    ctx->profile_list.pProfiles    = &ctx->profile;

    /* Get the capabilities of the decoder for the given profile */
    ctx->common.caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    ctx->common.caps.pNext = &ctx->enc_caps;
    ctx->enc_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    ctx->enc_caps.pNext = caps;

    ret = vk->GetPhysicalDeviceVideoCapabilitiesKHR(s->hwctx->phys_dev,
                                                    &ctx->profile,
                                                    &ctx->common.caps);
    if (ret == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_ERROR, "Unable to initialize encoding: "
               "%s profile \"%s\" not supported!\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, ctx->opts.profile));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_ERROR, "Unable to initialize encoding: "
               "format (%s) not supported!\n",
               av_get_pix_fmt_name(avctx->sw_pix_fmt));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_FEATURE_NOT_PRESENT ||
               ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "encoder capabilities for %s profile \"%s\":\n",
           avcodec_get_name(avctx->codec_id),
           avcodec_profile_name(avctx->codec_id, ctx->opts.profile));
    av_log(avctx, AV_LOG_VERBOSE, "    Width: from %i to %i\n",
           ctx->common.caps.minCodedExtent.width, ctx->common.caps.maxCodedExtent.width);
    av_log(avctx, AV_LOG_VERBOSE, "    Height: from %i to %i\n",
           ctx->common.caps.minCodedExtent.height, ctx->common.caps.maxCodedExtent.height);
    av_log(avctx, AV_LOG_VERBOSE, "    Width alignment: %i\n",
           ctx->common.caps.pictureAccessGranularity.width);
    av_log(avctx, AV_LOG_VERBOSE, "    Height alignment: %i\n",
           ctx->common.caps.pictureAccessGranularity.height);
    av_log(avctx, AV_LOG_VERBOSE, "    Bitstream offset alignment: %"PRIu64"\n",
           ctx->common.caps.minBitstreamBufferOffsetAlignment);
    av_log(avctx, AV_LOG_VERBOSE, "    Bitstream size alignment: %"PRIu64"\n",
           ctx->common.caps.minBitstreamBufferSizeAlignment);
    av_log(avctx, AV_LOG_VERBOSE, "    Maximum references: %u\n",
           ctx->common.caps.maxDpbSlots);
    av_log(avctx, AV_LOG_VERBOSE, "    Maximum active references: %u\n",
           ctx->common.caps.maxActiveReferencePictures);
    av_log(avctx, AV_LOG_VERBOSE, "    Codec header version: %i.%i.%i (driver), %i.%i.%i (compiled)\n",
           CODEC_VER(ctx->common.caps.stdHeaderVersion.specVersion),
           CODEC_VER(ff_vk_enc_ext[avctx->codec_id].specVersion));
    av_log(avctx, AV_LOG_VERBOSE, "    encode quality levels: %i\n",
           ctx->enc_caps.maxQualityLevels);
    av_log(avctx, AV_LOG_VERBOSE, "    encode image width alignment: %i\n",
           ctx->enc_caps.inputImageDataFillAlignment.width);
    av_log(avctx, AV_LOG_VERBOSE, "    encode image height alignment: %i\n",
           ctx->enc_caps.inputImageDataFillAlignment.height);
    av_log(avctx, AV_LOG_VERBOSE, "    Capability flags:%s%s%s\n",
           ctx->common.caps.flags ? "" :
               " none",
           ctx->common.caps.flags & VK_VIDEO_CAPABILITY_PROTECTED_CONTENT_BIT_KHR ?
               " protected" : "",
           ctx->common.caps.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR ?
               " separate_references" : "");

    /* Check if decoding is possible with the given parameters */
    if (avctx->coded_width  < ctx->common.caps.minCodedExtent.width   ||
        avctx->coded_height < ctx->common.caps.minCodedExtent.height  ||
        avctx->coded_width  > ctx->common.caps.maxCodedExtent.width   ||
        avctx->coded_height > ctx->common.caps.maxCodedExtent.height)
        return AVERROR(EINVAL);

    fmt_info.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR |
                          VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR;

    ctx->layered_dpb = !(ctx->common.caps.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);

    /* Get the supported image formats */
    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(s->hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, NULL);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    ret_info = av_mallocz(sizeof(*ret_info)*nb_out_fmts);
    if (!ret_info)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_out_fmts; i++)
        ret_info[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;

    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(s->hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, ret_info);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        av_free(ret_info);
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        av_free(ret_info);
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "Supported input formats:\n");
    for (i = 0; i < nb_out_fmts; i++)
        av_log(avctx, AV_LOG_VERBOSE, "    %i: %i\n", i, ret_info[i].format);

    for (i = 0; i < nb_out_fmts; i++) {
        if (ff_vk_pix_fmt_from_vkfmt(ret_info[i].format) == s->frames->sw_format) {
            ctx->pic_format = ret_info[i].format;
            break;
        }
    }

    av_free(ret_info);

    if (i == nb_out_fmts) {
        av_log(avctx, AV_LOG_ERROR, "Pixel format %s of input frames not supported!\n",
               av_get_pix_fmt_name(s->frames->sw_format));
        return AVERROR(EINVAL);
    }

    session_create.pVideoProfile = &ctx->profile;
    session_create.flags = 0x0;
    session_create.queueFamilyIndex = s->hwctx->queue_family_encode_index;
    session_create.maxCodedExtent = ctx->common.caps.maxCodedExtent;
    session_create.maxDpbSlots = ctx->common.caps.maxDpbSlots;
    session_create.maxActiveReferencePictures = ctx->common.caps.maxActiveReferencePictures;
    session_create.pictureFormat = ctx->pic_format;
    session_create.referencePictureFormat = session_create.pictureFormat;
    session_create.pStdHeaderVersion = &ff_vk_enc_ext[avctx->codec_id];

    err = ff_vk_video_common_init(avctx, s, &ctx->common, &session_create);
    if (err < 0)
        return err;

    /* Get sampler */
    av_chroma_location_enum_to_pos(&cxpos, &cypos, avctx->chroma_sample_location);
    yuv_sampler_info.xChromaOffset = cxpos >> 7;
    yuv_sampler_info.yChromaOffset = cypos >> 7;
    yuv_sampler_info.format = ctx->pic_format;
    ret = vk->CreateSamplerYcbcrConversion(s->hwctx->act_dev, &yuv_sampler_info,
                                           s->hwctx->alloc, &ctx->yuv_sampler);
    if (ret != VK_SUCCESS)
        return AVERROR_EXTERNAL;

    ctx->dpb_hwfc_ref = av_hwframe_ctx_alloc(s->frames->device_ref);
    if (!ctx->dpb_hwfc_ref)
        return AVERROR(ENOMEM);

    dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
    dpb_frames->format    = s->frames->format;
    dpb_frames->sw_format = s->frames->sw_format;
    dpb_frames->width     = s->frames->width;
    dpb_frames->height    = s->frames->height;

    dpb_hwfc = dpb_frames->hwctx;
    dpb_hwfc->create_pnext = &ctx->profile_list;
    dpb_hwfc->tiling       = VK_IMAGE_TILING_OPTIMAL;
    dpb_hwfc->format[0]    = ctx->pic_format;
    dpb_hwfc->usage        = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR |
                             VK_IMAGE_USAGE_SAMPLED_BIT; /* Shuts validator up. */

    if (ctx->layered_dpb) {
        ctx->dpb_layers = ctx->common.caps.maxDpbSlots;
        dpb_hwfc->nb_layers = ctx->dpb_layers;
    }

    err = av_hwframe_ctx_init(ctx->dpb_hwfc_ref);
    if (err < 0)
        return err;

    if (dpb_hwfc->nb_layers) {
        ctx->dpb_layer_taken = av_mallocz(ctx->dpb_layers*sizeof(*ctx->dpb_layer_taken));
        if (!ctx->dpb_layer_taken)
            return AVERROR(ENOMEM);
    }

    if (ctx->layered_dpb) {
        ctx->layered_frame = vk_get_dpb_pool(ctx);
        if (!ctx->layered_frame)
            return AVERROR(ENOMEM);
    }

    ctx->pic = av_mallocz(3*sizeof(*ctx->pic));
    if (!ctx->pic)
        return AVERROR(ENOMEM);

    return 0;
}

static void vkctx_frame_free(FFVulkanEncodeContext *ctx,
                             FFVulkanEncodePicture *pic)
{
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    if (pic->view)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, pic->view,
                             ctx->s.hwctx->alloc);

    /* TODO: keep these */
    if (pic->dpb_view)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, pic->dpb_view,
                             ctx->s.hwctx->alloc);

    av_frame_free(&pic->dpb_frame);
    av_buffer_unref(&pic->pkt_buf);
    av_freep(&pic->priv_data);

    ctx->dpb_layer_taken[pic->dpb_layer] = 0;
}

static int vkctx_frame_init(FFVulkanEncodeContext *ctx,
                            FFVulkanEncodePicture *pic, AVFrame *src)
{
    int err;
    AVVkFrame *vkf;

    if (!src) {
        ctx->end_of_stream = 1;

        /* Fix timestamps if we hit end-of-stream before the initial decode
         * delay has elapsed. */
        if (ctx->input_order < ctx->decode_delay)
            ctx->dts_pts_diff = ctx->pic_end->pts - ctx->first_pts;

        return AVERROR_EOF;
    }

    vkf = (AVVkFrame *)src->buf[0]->data;

    /* Create image view for the input frame */
    err = ff_vk_encode_create_view(ctx, &pic->view, &pic->aspect, vkf, 0);
    if (err < 0)
        goto fail;

    /* Create private data for input frame TODO remove this */
    pic->priv_data = av_mallocz(ctx->enc->pic_priv_data_size);
    if (!pic->priv_data) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Allocate a DPB buffer */
    if (!pic->dpb_frame) {
        int layer_id = 0;
        if (ctx->layered_dpb) {
            for (int layer_id = 0; layer_id < ctx->dpb_layers; layer_id++)
                if (!ctx->dpb_layer_taken[layer_id])
                    break;

            /* No free DPB slots */
            if (layer_id == ctx->dpb_layers)
                return AVERROR(EAGAIN);

            pic->dpb_frame = av_frame_clone(ctx->layered_frame);
        } else {
            pic->dpb_frame = vk_get_dpb_pool(ctx);
        }

        if (!pic->dpb_frame) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        /* Create image view for the DPB */
        err = ff_vk_encode_create_view(ctx, &pic->dpb_view, &pic->dpb_aspect,
                                       (AVVkFrame *)pic->dpb_frame->data[0], layer_id);
        if (err < 0)
            goto fail;

        ctx->dpb_layer_taken[layer_id] = 1;
        pic->dpb_layer = layer_id;
    }

    if (ctx->input_order == 0 || src->pict_type == AV_PICTURE_TYPE_I)
        pic->force_idr = 1;

    pic->pts = src->pts;
    pic->duration = src->duration;
    pic->time_base = src->time_base;

    if (ctx->input_order == 0)
        ctx->first_pts = pic->pts;
    if (ctx->input_order == ctx->decode_delay)
        ctx->dts_pts_diff = pic->pts - ctx->first_pts;
    if (ctx->output_delay > 0)
        ctx->dts_ring[ctx->input_order %
                      (3 * ctx->output_delay + ctx->opts.async_depth)] = pic->pts;

    pic->display_order = ctx->input_order;
    ctx->input_order++;

    if (ctx->pic_start) {
        ctx->pic_end->next = pic;
        ctx->pic_end       = pic;
    } else {
        ctx->pic_start     = pic;
        ctx->pic_end       = pic;
    }

    return 0;

fail:
    vkctx_frame_free(ctx, pic);
    return err;
}

static int vulkan_encode_issue(AVCodecContext *avctx,
                               FFVulkanEncodeContext *ctx,
                               FFVulkanEncodePicture *pic,
                               AVFrame *src)
{
    int err, max_pkt_size;
    const size_t size_align = ctx->common.caps.minBitstreamBufferSizeAlignment;
    const int layered_dpb = ctx->layered_dpb;

    FFVulkanFunctions *vk = &ctx->s.vkfn;
    AVVkFrame *vkf = (AVVkFrame *)src->buf[0]->data;

    FFVkVideoBuffer *sd_buf;

    FFVkExecContext *exec;
    VkCommandBuffer cmd_buf;
    VkImageMemoryBarrier2 img_bar[37];
    int nb_img_bar = 0;

    /* Coding start/end */
    VkVideoBeginCodingInfoKHR encode_start = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
        .videoSession = ctx->common.session,
        .videoSessionParameters = ctx->session_params,
        .referenceSlotCount = 0,
        .pReferenceSlots = NULL,
    };
    VkVideoEndCodingInfoKHR encode_end = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    };

    VkVideoEncodeRateControlLayerInfoKHR rc_layer;
    VkVideoEncodeRateControlInfoKHR rc_info;
    VkVideoCodingControlInfoKHR encode_ctrl;
    VkVideoPictureResourceInfoKHR dpb_pic;
    VkVideoReferenceSlotInfoKHR dpb_slot;
    VkVideoPictureResourceInfoKHR ref_pic[37];
    VkVideoReferenceSlotInfoKHR ref_slot[37];
    VkVideoEncodeInfoKHR encode_info;

    /* Initialize all codec-specific headers */
    err = ctx->enc->init_pic_headers(avctx, pic);
    if (err < 0)
        return err;

    /* Create packet data buffer, TODO make this smarter */
    max_pkt_size = (src->width * src->height)*2;

    err = ff_vk_video_get_buffer(&ctx->s, &ctx->common, &pic->pkt_buf,
                                 VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
                                 &ctx->profile_list, max_pkt_size);
    if (err < 0)
        goto fail;

    sd_buf = (FFVkVideoBuffer *)pic->pkt_buf->data;

    /* Rate control */
    rc_layer = (VkVideoEncodeRateControlLayerInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
        .pNext = pic->codec_rc_layer,
    };
    rc_info = (VkVideoEncodeRateControlInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
        .pNext = NULL,
        .rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR,
        .layerCount = 1, /* Required to be >= 1 */
        .pLayers = &rc_layer,
    };
    encode_ctrl = (VkVideoCodingControlInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
        .pNext = &rc_info,
        .flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR,
    };

    /* Current picture */
    dpb_pic = (VkVideoPictureResourceInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
        .pNext = NULL,
        .codedOffset = { 0 },
        .codedExtent = (VkExtent2D){ src->width, src->height },
        .baseArrayLayer = 0,
        .imageViewBinding = pic->dpb_view,
    };
    dpb_slot = (VkVideoReferenceSlotInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
        .pNext = NULL,
        .slotIndex = pic->slot,
        .pPictureResource = &dpb_pic,
    };

    /* References for current picture */
    for (int i = 0; i < pic->nb_refs; i++) {
        FFVulkanEncodePicture *ref = pic->refs[i];

        ref_pic[i] = (VkVideoPictureResourceInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
            .pNext = NULL,
            .codedOffset = { 0 },
            .codedExtent = (VkExtent2D){ src->width, src->height },
            .baseArrayLayer = 0,
            .imageViewBinding = ref->dpb_view,
        };
        ref_slot[i] = (VkVideoReferenceSlotInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
            .pNext = NULL,
            .slotIndex = ref->slot,
            .pPictureResource = &ref_pic[i],
        };
    }

    encode_info = (VkVideoEncodeInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
        .pNext = pic->codec_info,
        .flags = 0x0,
        .qualityLevel = 0,
        .srcPictureResource = (VkVideoPictureResourceInfoKHR) { // SPEC: this should be separate
            .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
            .pNext = NULL,
            .codedOffset = 0,
            .codedExtent = (VkExtent2D){ src->width, src->height },
            .baseArrayLayer = 0,
            .imageViewBinding = pic->view,
        },
        .pSetupReferenceSlot = &dpb_slot, /* pic->is_reference ? &ref_slot : NULL */
        .referenceSlotCount = pic->nb_refs,
        .pReferenceSlots = ref_slot,
        .dstBuffer = sd_buf->buf.buf,
        .dstBufferOffset = 0,
        .dstBufferRange = sd_buf->buf.size,
        .precedingExternallyEncodedBytes = 0,
    };

    /* Reset encoder for the very first frame and on every keyframe */
    if (pic->display_order == 0 || pic->type == FF_VK_FRAME_KEY)
        encode_ctrl.flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;

    /* Write header */
    if (pic->type == FF_VK_FRAME_KEY && ctx->enc->write_stream_headers) {
        uint8_t *hdr_dst = sd_buf->mem + encode_info.dstBufferOffset;
        size_t data_size = encode_info.dstBufferRange;
        err = ctx->enc->write_stream_headers(avctx, hdr_dst, &data_size);
        if (err < 0)
            goto fail;
        encode_info.dstBufferOffset += data_size;
        encode_info.dstBufferRange  -= data_size;
    }

    /* Write extra units */
    if (ctx->enc->write_extra_headers) {
        uint8_t *hdr_dst = sd_buf->mem + encode_info.dstBufferOffset;
        size_t data_size = encode_info.dstBufferRange;
        err = ctx->enc->write_extra_headers(avctx, pic, hdr_dst, &data_size);
        if (err < 0)
            goto fail;
        encode_info.dstBufferOffset += data_size;
        encode_info.dstBufferRange  -= data_size;
    }

    /* Align buffer offset to the required value with filler units */
    if (ctx->enc->write_filler) {
        uint8_t *hdr_dst = sd_buf->mem + encode_info.dstBufferOffset;
        size_t data_size = encode_info.dstBufferRange;

        uint32_t offset = encode_info.dstBufferOffset;
        size_t offset_align = ctx->common.caps.minBitstreamBufferOffsetAlignment;

        uint32_t filler_data = FFALIGN(offset, offset_align) - offset;

        if (filler_data) {
            while (filler_data < ctx->enc->filler_header_size)
                filler_data += offset_align;

            filler_data -= ctx->enc->filler_header_size;

            err = ctx->enc->write_filler(avctx, filler_data,
                                         hdr_dst, &data_size);
            if (err < 0)
                goto fail;
            encode_info.dstBufferOffset += data_size;
            encode_info.dstBufferRange  -= data_size;
        }
    }

    pic->pkt_buf_offset = encode_info.dstBufferOffset;

    /* Align buffer size to the nearest lower alignment requirement. */
    encode_info.dstBufferRange -= size_align;
    encode_info.dstBufferRange = FFALIGN(encode_info.dstBufferRange,
                                         size_align);

    /* Start command buffer recording */
    exec = ff_vk_exec_get(&ctx->enc_pool);
    ff_vk_exec_start(&ctx->s, exec);
    cmd_buf = exec->buf;

    /* Output packet buffer */
    err = ff_vk_exec_add_dep_buf(&ctx->s, exec, &pic->pkt_buf, 1, 1);
    if (err < 0)
        goto fail;

    /* Source image - change encode to compute once we have analysis */
    err = ff_vk_exec_add_dep_frame(&ctx->s, exec, src,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
    if (err < 0)
        goto fail;

    /* Source image layout conversion */
    img_bar[nb_img_bar] = (VkImageMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = NULL,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = vkf->access[0],
        .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
        .oldLayout = vkf->layout[0],
        .newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
        .srcQueueFamilyIndex = vkf->queue_family[0],
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vkf->img[0],
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask = pic->aspect,
            .layerCount = 1,
            .levelCount = 1,
        },
    };
    ff_vk_exec_update_frame(&ctx->s, exec, src,
                            &img_bar[nb_img_bar], &nb_img_bar);

    /* Source image's DPB */
    if (encode_info.pSetupReferenceSlot) {
        err = ff_vk_exec_add_dep_frame(&ctx->s, exec, pic->dpb_frame,
                                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
        if (err < 0)
            return err;
    }

    /* Reference frame DPBs */
    if (!layered_dpb) {
        for (int i = 0; i < pic->nb_refs; i++) {
            FFVulkanEncodePicture *ref = pic->refs[i];
            err = ff_vk_exec_add_dep_frame(&ctx->s, exec, ref->dpb_frame,
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
            if (err < 0)
                return err;
        }
    }

    /* Change image layout */
    vk->CmdPipelineBarrier2(cmd_buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
        });

    /* Start, use parameters */
    vk->CmdBeginVideoCodingKHR(cmd_buf, &encode_start);

    /* Send control data */
    vk->CmdControlVideoCodingKHR(cmd_buf, &encode_ctrl);

    /* encode and fetch the status.
     * SPEC: cannot insert AUD units between slices! */
    vk->CmdBeginQuery(cmd_buf, ctx->enc_pool.query_pool, exec->query_idx + 0, 0);
    vk->CmdEncodeVideoKHR(cmd_buf, &encode_info);
    vk->CmdEndQuery(cmd_buf, ctx->enc_pool.query_pool, exec->query_idx + 0);

    /* End encoding */
    vk->CmdEndVideoCodingKHR(cmd_buf, &encode_end);

    /* End recording and submit for execution */
    ff_vk_exec_submit(&ctx->s, exec);

    pic->encode_issued = 1;
    pic->exec = exec;

    return 0;

fail:
    vkctx_frame_free(ctx, pic);
    return err;
}

static int vulkan_encode_output(AVCodecContext *avctx,
                                FFVulkanEncodeContext *ctx,
                                AVPacket *pkt,
                                FFVulkanEncodePicture *pic)
{
    int err;
    VkResult ret;
    int64_t qstatus = 0;
    uint32_t pkt_size, *query_data;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    FFVkVideoBuffer *sd_buf = (FFVkVideoBuffer *)pic->pkt_buf->data;

    /* TODO: replace this with a semaphore wait, maybe? */
    ff_vk_exec_wait(&ctx->s, pic->exec);

    /* Get status */
    ret = ff_vk_exec_get_query(&ctx->s, pic->exec, (void **)&query_data, &qstatus);
    if (ret != VK_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Error querying results from encoder: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }
    // SPEC: signal if there hasn't been enough buffer with a unique return code */

    if (qstatus < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error while encoding: %li!\n", qstatus);
        return AVERROR(EINVAL);
    }

    // SPEC: why?
    query_data[0] += pic->pkt_buf_offset;

    pkt_size = query_data[0] /* Buffer offset */ +
               query_data[1] /* Data written */;

    av_log(ctx, AV_LOG_VERBOSE, "Received a packet, %u bytes large (%u off, %u data), "
           "status = %i\n", pkt_size, query_data[0], query_data[1], err);

    if (!(sd_buf->buf.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange invalidate_buf = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = sd_buf->buf.mem,
            .offset = query_data[0], /* Should be already aligned */
            .size = FFALIGN(query_data[1],
                            ctx->s.props.properties.limits.nonCoherentAtomSize),
        };

        vk->FlushMappedMemoryRanges(ctx->s.hwctx->act_dev, 1, &invalidate_buf);
    }

    /* Transfer data buffer ref */
    pkt->pts       = pkt->dts = pic->pts;
//    pkt->duration  = pic->duration;
    pkt->time_base = pic->time_base;
    pkt->buf       = pic->pkt_buf;
    pkt->data      = sd_buf->mem;
    pkt->size      = pkt_size;
    pkt->flags     = ( pic->type == FF_VK_FRAME_KEY ? AV_PKT_FLAG_KEY : 0x0) |
                     (!pic->is_reference ?  AV_PKT_FLAG_DISPOSABLE : 0x0);

    if (ctx->output_delay == 0) {
        pkt->dts = pkt->pts;
    } else if (pic->encode_order < ctx->decode_delay) {
        if (ctx->dts_ring[pic->encode_order] < INT64_MIN + ctx->dts_pts_diff)
            pkt->dts = INT64_MIN;
        else
            pkt->dts = ctx->dts_ring[pic->encode_order] - ctx->dts_pts_diff;
    } else {
        pkt->dts = ctx->dts_ring[(pic->encode_order - ctx->decode_delay) %
                                (3 * ctx->output_delay + ctx->opts.async_depth)];
    }

    pic->encode_complete = 1;
    pic->encode_size = pkt_size;
    pic->pkt_buf = NULL;

    return 0;
}

int ff_vulkan_encode_receive_packet(AVCodecContext *avctx, FFVulkanEncodeContext *ctx,
                                    AVPacket *pkt)
{
    int err;
    AVFrame *frame;
    FFVulkanEncodePicture *cur_pic = &ctx->pic[avctx->frame_num % ctx->gop_size];
    int ft = !(avctx->frame_num % ctx->gop_size) ? FF_VK_FRAME_KEY : FF_VK_FRAME_P;

    {
        /* TODO: remove this per-frame alloc */
        frame = av_frame_alloc();
        if (!frame)
            return AVERROR(ENOMEM);

        err = ff_encode_get_frame(avctx, frame);
        if (err < 0 && err != AVERROR_EOF) {
            av_frame_free(&frame);
            return err;
        }

        if (err == AVERROR_EOF) {
            av_frame_free(&frame);
            return err;
        }

        if (ft == FF_VK_FRAME_P)
            cur_pic->prev = &ctx->pic[(avctx->frame_num % ctx->gop_size) - 1];

        err = vkctx_frame_init(ctx, cur_pic, frame);
        if (err < 0) {
            av_frame_free(&frame);
            return err;
        }

        cur_pic->refs[0]      = !(avctx->frame_num % ctx->gop_size) ? NULL : &ctx->pic[0];
        cur_pic->slot         = avctx->frame_num % ctx->gop_size;
        cur_pic->nb_refs      = !!(avctx->frame_num % ctx->gop_size);
        cur_pic->is_reference = ft == FF_VK_FRAME_KEY;
        cur_pic->type         = ft;
        cur_pic->qp           = 30;
        cur_pic->encode_order = ctx->encode_order++;

        err = vulkan_encode_issue(avctx, ctx, cur_pic, frame);
        av_frame_free(&frame);
        if (err < 0)
            return err;
    }

    err = vulkan_encode_output(avctx, ctx, pkt, cur_pic);
    if (err < 0)
        return err;

    if (!((avctx->frame_num + 1) % ctx->gop_size)) {
        vkctx_frame_free(ctx, &ctx->pic[0]);
        vkctx_frame_free(ctx, &ctx->pic[1]);
        vkctx_frame_free(ctx, &ctx->pic[2]);
    }

    return pkt->size;
}
