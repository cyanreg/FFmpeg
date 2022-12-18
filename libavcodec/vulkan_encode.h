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

#ifndef AVCODEC_VULKAN_ENCODE_H
#define AVCODEC_VULKAN_ENCODE_H

#include "encode.h"
#include "hwconfig.h"

#include "vulkan_video.h"

#define MAX_REORDER_DELAY 16
#define MAX_ASYNC_DEPTH 64
#define MAX_PICTURE_REFERENCES 2

enum FFVkFrameType {
    FF_VK_FRAME_KEY, /* IDR in mpeg-ese */
    FF_VK_FRAME_I,   /* mpeg-only */

    FF_VK_FRAME_P,
    FF_VK_FRAME_B,   /* mpeg-only */

    FF_VK_FRAME_S,   /* av1-only */
};

typedef struct FFVkEncodeCommonOptions {
    int profile;
    int async_depth;
    VkVideoEncodeUsageFlagBitsKHR usage;
    VkVideoEncodeContentFlagBitsKHR content;
    VkVideoEncodeTuningModeKHR tune;
} FFVkEncodeCommonOptions;

typedef struct FFVulkanEncodePicture {
    int                qp;
    enum FFVkFrameType type;
    int64_t            display_order;
    int64_t            encode_order;

    int                slot;
    int64_t            pts;
    int64_t            duration;
    AVRational         time_base;
    int64_t            reordered_opaque;

    FFVkExecContext   *exec;
    int                encode_issued;
    int                encode_complete;
    size_t             encode_size;

    int                force_idr;
    int                b_depth;

    int                is_reference; /* Set if picture is used as a ref */

    void              *priv_data;

    void              *codec_info;
    void              *codec_rc_layer;

    /* Input frame */
    VkImageView           view;
    VkImageAspectFlags    aspect;

    /* DPB */
    AVFrame              *dpb_frame;
    VkImageView           dpb_view;
    VkImageAspectFlags    dpb_aspect;
    int                   dpb_layer;

    /* Packet buffer - contains an FFVkVideoBuffer struct */
    size_t                pkt_buf_offset; // SPEC: MAKE UP YOUR MIND JAMMIT
    AVBufferRef          *pkt_buf;

    /* The previous reference picture in encode order. Must be in at least
     * one of the reference list and DPB list. */
    struct FFVulkanEncodePicture *prev;
    struct FFVulkanEncodePicture *next;

    /* The contents of the DPB after this picture has been decoded.
     * This will contain the picture itself if it is a reference picture,
     * but not if it isn't. */
    struct FFVulkanEncodePicture *dpb[16];
    int                           nb_dpb_pics;

    /* The reference pictures used in decoding this picture. If they are
     * used by later pictures they will also appear in the DPB. */
    struct FFVulkanEncodePicture *refs[MAX_PICTURE_REFERENCES];
    int                           nb_refs;

    /* Reference count for other pictures referring to this one through
     * the above pointers, directly from incomplete pictures and indirectly
     * through completed pictures. */
    int             ref_count[2];
    int             ref_removed[2];
} FFVulkanEncodePicture;

/**
 * Callback for writing stream-level headers.
 */
typedef int (*vkenc_cb_write_stream_headers)(AVCodecContext *avctx,
                                             uint8_t *data, size_t *data_len);

/**
 * Callback for initializing codec-specific picture headers.
 */
typedef int (*vkenc_cb_init_pic_headers)(AVCodecContext *avctx,
                                         FFVulkanEncodePicture *pic);

/**
 * Callback for writing alignment data.
 * Align is the value to align offset to.
 */
typedef int (*vkenc_cb_write_filler)(AVCodecContext *avctx, uint32_t filler,
                                     uint8_t *data, size_t *data_len);

/**
 * Callback for writing any extra units requested. data_len must be set
 * to the available size, and its value will be overwritten by the #bytes written
 * to the output buffer.
 */
typedef int (*vkenc_cb_write_extra_headers)(AVCodecContext *avctx,
                                            FFVulkanEncodePicture *pic,
                                            uint8_t *data, size_t *data_len);

typedef struct FFVulkanEncoder {
    size_t                        pic_priv_data_size;
    uint32_t                      filler_header_size;

    vkenc_cb_write_stream_headers write_stream_headers;
    vkenc_cb_init_pic_headers     init_pic_headers;
    vkenc_cb_write_filler         write_filler;
    vkenc_cb_write_extra_headers  write_extra_headers;
} FFVulkanEncoder;

typedef struct FFVulkanEncodeContext {
    FFVulkanContext s;
    FFVkVideoCommon common;
    const FFVulkanEncoder *enc;

    int64_t input_order;
    int64_t encode_order;

    FFVulkanEncodePicture *pic;

    int gop_size;
    int64_t bitrate;

    int64_t first_pts;
    int output_delay;
    int decode_delay;
    int end_of_stream;
    int64_t dts_pts_diff;

    int64_t dts_ring[MAX_REORDER_DELAY * 3 + MAX_ASYNC_DEPTH];

    /* Current encoding window, in display (input) order. */
    FFVulkanEncodePicture *pic_start, *pic_end;
    /* The next picture to use as the previous reference picture in
     * encoding order. */
    FFVulkanEncodePicture *next_prev;

    int frame_num;

    /* DPB */
    AVBufferRef *dpb_hwfc_ref;
    AVFrame *layered_frame;
    int layered_dpb;
    int *dpb_layer_taken;
    int dpb_layers;

    VkVideoSessionParametersKHR session_params;

    VkSamplerYcbcrConversion yuv_sampler;
    VkFormat pic_format;

    FFVkEncodeCommonOptions opts;

    VkVideoProfileInfoKHR profile;
    VkVideoProfileListInfoKHR profile_list;
    VkVideoEncodeCapabilitiesKHR enc_caps;
    VkVideoEncodeUsageInfoKHR usage_info;

    FFVkQueueFamilyCtx qf_enc;
    FFVkExecPool enc_pool;
} FFVulkanEncodeContext;

#define FF_VK_ENCODE_COMMON_OPTS \
    { "tune", "Select tuning type", OFFSET(vkenc.opts.tune), AV_OPT_TYPE_INT, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR }, 0, INT_MAX, FLAGS, "tune" }, \
        { "default",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR           }, INT_MIN, INT_MAX, FLAGS, "tune" }, \
        { "hq",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR      }, INT_MIN, INT_MAX, FLAGS, "tune" }, \
        { "ll",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR       }, INT_MIN, INT_MAX, FLAGS, "tune" }, \
        { "ull",      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR }, INT_MIN, INT_MAX, FLAGS, "tune" }, \
        { "lossless", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR          }, INT_MIN, INT_MAX, FLAGS, "tune" }, \
    { "usage", "Select usage type", OFFSET(vkenc.opts.usage), AV_OPT_TYPE_FLAGS, { .i64 = VK_VIDEO_DECODE_USAGE_DEFAULT_KHR }, 0, INT_MAX, FLAGS, "usage" }, \
        { "default",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_DECODE_USAGE_DEFAULT_KHR          }, INT_MIN, INT_MAX, FLAGS, "usage" }, \
        { "transcode",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_USAGE_TRANSCODING_BIT_KHR  }, INT_MIN, INT_MAX, FLAGS, "usage" }, \
        { "stream",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR    }, INT_MIN, INT_MAX, FLAGS, "usage" }, \
        { "record",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_USAGE_RECORDING_BIT_KHR    }, INT_MIN, INT_MAX, FLAGS, "usage" }, \
        { "conference", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_USAGE_CONFERENCING_BIT_KHR }, INT_MIN, INT_MAX, FLAGS, "usage" }, \
    { "content", "Select content type", OFFSET(vkenc.opts.content), AV_OPT_TYPE_FLAGS, { .i64 = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR }, 0, INT_MAX, FLAGS, "content" }, \
        { "default",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR      }, INT_MIN, INT_MAX, FLAGS, "content" }, \
        { "camera",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_CONTENT_CAMERA_BIT_KHR   }, INT_MIN, INT_MAX, FLAGS, "content" }, \
        { "desktop",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_CONTENT_DESKTOP_BIT_KHR  }, INT_MIN, INT_MAX, FLAGS, "content" }, \
        { "rendered", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR }, INT_MIN, INT_MAX, FLAGS, "content" }, \
    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(vkenc.opts.async_depth), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, MAX_ASYNC_DEPTH, FLAGS }, \

/**
 * Paperwork.
 */
extern const AVCodecHWConfigInternal *const ff_vulkan_encode_hw_configs[];

/**
 * Extension name and version.
 */
extern const VkExtensionProperties ff_vk_enc_ext[AV_CODEC_ID_FIRST_AUDIO];

/**
 * Create image view for a frame.
 */
int ff_vk_encode_create_view(FFVulkanEncodeContext *ctx, VkImageView *dst_view,
                             VkImageAspectFlags *aspect, AVVkFrame *src, int layer);

/**
 * Initialize encoder.
 */
int ff_vulkan_encode_init(AVCodecContext *avctx, FFVulkanEncodeContext *ctx,
                          void *codec_profile, void *caps,
                          const FFVulkanEncoder *enc,
                          int output_delay, int decode_delay);

/**
 * Uninitialize encoder.
 */
void ff_vulkan_encode_uninit(FFVulkanEncodeContext *ctx);

/**
 * Encode.
 */
int ff_vulkan_encode_receive_packet(AVCodecContext *avctx, FFVulkanEncodeContext *ctx,
                                    AVPacket *pkt);

#endif /* AVCODEC_VULKAN_ENCODE_H */
