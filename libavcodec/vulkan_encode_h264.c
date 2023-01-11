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

#include "libavutil/opt.h"

#include "cbs.h"
#include "cbs_h264.h"
#include "h264_levels.h"
#include "h2645data.h"
#include "codec_internal.h"
#include "version.h"

#include "vulkan_encode.h"

enum UnitElems {
    UNIT_AUD        = 1 << 0,
    UNIT_TIMING     = 1 << 1,
    UNIT_IDENTIFIER = 1 << 2,
    UNIT_RECOVERY   = 1 << 3,
};

/* Random (version 4) ISO 11578 UUID. */
static const uint8_t vulkan_encode_h264_sei_identifier_uuid[16] = {
    0x03, 0xfd, 0xf2, 0x0a, 0x5d, 0x4c, 0x05, 0x48,
    0x20, 0x98, 0xca, 0x6b, 0x0c, 0x95, 0x30, 0x1c,
};

typedef struct VulkanEncodeH264Context {
    FFVulkanEncodeContext vkenc;
    VkVideoEncodeH264ProfileInfoEXT profile;
    VkVideoEncodeH264CapabilitiesEXT caps;

    int bit_rate;
    int output_delay;
    int decode_delay;
    int gop_size;
    int b_per_p;
    int dpb_frames;
    int max_b_depth;
    int hrd_initial_buffer_fullness;
    int hrd_buffer_size;

    int mb_width;
    int mb_height;

    /* Options */
    enum UnitElems insert_units;
    int coder;
    int desired_b_depth;

    /* State */
    enum UnitElems write_units;

    /* SPS structs */
    H264RawSPS raw_sps;
    StdVideoH264ScalingLists vksps_scaling;
    StdVideoH264HrdParameters vksps_vui_header;
    StdVideoH264SequenceParameterSetVui vksps_vui;
    StdVideoH264SequenceParameterSet vksps;

    /* PPS structs */
    H264RawPPS raw_pps;
    StdVideoH264ScalingLists vkpps_scaling;
    StdVideoH264PictureParameterSet vkpps;

    /* Structs needed for CBC */
    H264RawAUD                  raw_aud;

    CodedBitstreamContext      *cbc;
    CodedBitstreamFragment      current_access_unit;
    SEIRawUserDataUnregistered  sei_identifier;
    H264RawSEIBufferingPeriod   sei_buffering_period;
    H264RawSEIPicTiming         sei_pic_timing;
    H264RawSEIRecoveryPoint     sei_recovery_point;
    char                       *sei_identifier_string;
} VulkanEncodeH264Context;

typedef struct VulkanEncodeH264Picture {
    uint64_t frame_num;
    int64_t last_idr_frame;
    uint16_t idr_pic_id;
    uint16_t pic_order_cnt;

    StdVideoEncodeH264WeightTable slice_wt;
    StdVideoEncodeH264SliceHeader slice_hdr;
    VkVideoEncodeH264NaluSliceInfoEXT vkslice;
    StdVideoEncodeH264PictureInfo h264pic_info;
    VkVideoEncodeH264VclFrameInfoEXT vkh264pic_info;
    VkVideoEncodeH264RateControlLayerInfoEXT vkrc_layer_info;

    StdVideoEncodeH264ReferenceListsInfo final_list;
    VkVideoEncodeH264DpbSlotInfoEXT l0refs[37];
    VkVideoEncodeH264DpbSlotInfoEXT l1refs[37];
    StdVideoEncodeH264ReferenceInfo l0ref_info[37];
    StdVideoEncodeH264ReferenceInfo l1ref_info[37];

    StdVideoEncodeH264ReferenceListsInfo ref_list_info;
    StdVideoEncodeH264RefListModEntry l0mods[37];
    StdVideoEncodeH264RefListModEntry l1mods[37];
    StdVideoEncodeH264RefPicMarkingEntry marks[37];
} VulkanEncodeH264Picture;

static av_cold int vulkan_encode_h264_init_seq_params(AVCodecContext *avctx)
{
    VulkanEncodeH264Context             *enc = avctx->priv_data;

    H264RawSPS                          *sps = &enc->raw_sps;
    H264RawHRD                          *hrd = &sps->vui.nal_hrd_parameters;
    StdVideoH264ScalingLists            *vksps_scaling = &enc->vksps_scaling;
    StdVideoH264HrdParameters           *vksps_vui_header = &enc->vksps_vui_header;
    StdVideoH264SequenceParameterSetVui *vksps_vui = &enc->vksps_vui;
    StdVideoH264SequenceParameterSet    *vksps = &enc->vksps;

    H264RawPPS                          *pps = &enc->raw_pps;
    StdVideoH264ScalingLists            *vkpps_scaling = &enc->vkpps_scaling;
    StdVideoH264PictureParameterSet     *vkpps = &enc->vkpps;

    memset(sps, 0, sizeof(*sps));
    memset(pps, 0, sizeof(*pps));

    sps->nal_unit_header.nal_ref_idc   = 3;
    sps->nal_unit_header.nal_unit_type = H264_NAL_SPS;

    sps->profile_idc = enc->vkenc.opts.profile & 0xff;

    if (sps->profile_idc == FF_PROFILE_H264_MAIN)
        sps->constraint_set1_flag = 1;

    if (sps->profile_idc == FF_PROFILE_H264_HIGH)
        sps->constraint_set3_flag = enc->gop_size == 1;

    if (sps->profile_idc == FF_PROFILE_H264_MAIN ||
        sps->profile_idc == FF_PROFILE_H264_HIGH) {
        sps->constraint_set4_flag = 1;
        sps->constraint_set5_flag = enc->b_per_p == 0;
    }

    if (avctx->gop_size == 1)
        enc->dpb_frames = 0;
    else
        enc->dpb_frames = 1 + enc->max_b_depth;

    if (avctx->level != FF_LEVEL_UNKNOWN) {
        sps->level_idc = avctx->level;
    } else {
        const H264LevelDescriptor *level;
        int framerate;

        if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
            framerate = avctx->framerate.num / avctx->framerate.den;
        else
            framerate = 0;

        level = ff_h264_guess_level(sps->profile_idc,
                                    avctx->bit_rate,
                                    framerate,
                                    enc->mb_width  * 16,
                                    enc->mb_height * 16,
                                    enc->dpb_frames);
        if (level) {
            av_log(avctx, AV_LOG_VERBOSE, "Using level %s.\n", level->name);
            if (level->constraint_set3_flag)
                sps->constraint_set3_flag = 1;
            sps->level_idc = level->level_idc;
        } else {
            av_log(avctx, AV_LOG_WARNING, "Stream will not conform "
                   "to any level: using level 6.2.\n");
            sps->level_idc = 62;
        }
    }

    sps->seq_parameter_set_id = 0;
    sps->chroma_format_idc    = 1;

    sps->log2_max_frame_num_minus4 = 4;
    sps->pic_order_cnt_type        = enc->max_b_depth ? 0 : 2;
    if (!sps->pic_order_cnt_type)
        sps->log2_max_pic_order_cnt_lsb_minus4 = 4;

    sps->max_num_ref_frames = enc->dpb_frames;

    sps->pic_width_in_mbs_minus1        = enc->mb_width  - 1;
    sps->pic_height_in_map_units_minus1 = enc->mb_height - 1;

    sps->frame_mbs_only_flag = 1;
    sps->direct_8x8_inference_flag = 1;

    if (avctx->width  != 16 * enc->mb_width ||
        avctx->height != 16 * enc->mb_height) {
        sps->frame_cropping_flag = 1;

        sps->frame_crop_left_offset   = 0;
        sps->frame_crop_right_offset  = (16 * enc->mb_width - avctx->width) / 2;
        sps->frame_crop_top_offset    = 0;
        sps->frame_crop_bottom_offset = (16 * enc->mb_height - avctx->height) / 2;
    } else {
        sps->frame_cropping_flag = 0;
    }

    sps->vui_parameters_present_flag = 1;

    if (avctx->sample_aspect_ratio.num != 0 &&
        avctx->sample_aspect_ratio.den != 0) {
        int num, den, i;
        av_reduce(&num, &den, avctx->sample_aspect_ratio.num,
                  avctx->sample_aspect_ratio.den, 65535);
        for (i = 0; i < FF_ARRAY_ELEMS(ff_h2645_pixel_aspect); i++) {
            if (num == ff_h2645_pixel_aspect[i].num &&
                den == ff_h2645_pixel_aspect[i].den) {
                sps->vui.aspect_ratio_idc = i;
                break;
            }
        }
        if (i >= FF_ARRAY_ELEMS(ff_h2645_pixel_aspect)) {
            sps->vui.aspect_ratio_idc = 255;
            sps->vui.sar_width  = num;
            sps->vui.sar_height = den;
        }
        sps->vui.aspect_ratio_info_present_flag = 1;
    }

    /* Unspecified video format, from table E-2. */
    sps->vui.video_format             = 5;
    sps->vui.video_full_range_flag    = avctx->color_range == AVCOL_RANGE_JPEG;
    sps->vui.colour_primaries         = avctx->color_primaries;
    sps->vui.transfer_characteristics = avctx->color_trc;
    sps->vui.matrix_coefficients      = avctx->colorspace;
    if (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED ||
        avctx->color_trc       != AVCOL_TRC_UNSPECIFIED ||
        avctx->colorspace      != AVCOL_SPC_UNSPECIFIED)
        sps->vui.colour_description_present_flag = 1;
    if (avctx->color_range     != AVCOL_RANGE_UNSPECIFIED ||
        sps->vui.colour_description_present_flag)
        sps->vui.video_signal_type_present_flag = 1;

    if (avctx->chroma_sample_location != AVCHROMA_LOC_UNSPECIFIED) {
        sps->vui.chroma_loc_info_present_flag = 1;
        sps->vui.chroma_sample_loc_type_top_field =
            sps->vui.chroma_sample_loc_type_bottom_field =
                avctx->chroma_sample_location - 1;
    }

    sps->vui.timing_info_present_flag = 1;
    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        sps->vui.num_units_in_tick = avctx->framerate.den;
        sps->vui.time_scale        = 2 * avctx->framerate.num;
        sps->vui.fixed_frame_rate_flag = 1;
    } else {
        sps->vui.num_units_in_tick = avctx->time_base.num;
        sps->vui.time_scale        = 2 * avctx->time_base.den;
        sps->vui.fixed_frame_rate_flag = 0;
    }

    if (enc->insert_units & UNIT_TIMING) {
        H264RawHRD *hrd = &sps->vui.nal_hrd_parameters;
        H264RawSEIBufferingPeriod *bp = &enc->sei_buffering_period;

        sps->vui.nal_hrd_parameters_present_flag = 1;

        hrd->cpb_cnt_minus1 = 0;

        /* Try to scale these to a sensible range so that the
         * golomb encode of the value is not overlong. */
        hrd->bit_rate_scale = av_clip_uintp2(av_log2(enc->bit_rate) - 15 - 6, 4);
        hrd->bit_rate_value_minus1[0] = (enc->bit_rate >> hrd->bit_rate_scale + 6) - 1;

        hrd->cpb_size_scale = av_clip_uintp2(av_log2(enc->hrd_buffer_size) - 15 - 4, 4);
        hrd->cpb_size_value_minus1[0] = (enc->hrd_buffer_size >> hrd->cpb_size_scale + 4) - 1;

        /* CBR mode as defined for the HRD cannot be achieved without filler
         * data */
        hrd->cbr_flag[0] = 0;

        hrd->initial_cpb_removal_delay_length_minus1 = 23;
        hrd->cpb_removal_delay_length_minus1         = 23;
        hrd->dpb_output_delay_length_minus1          = 7;
        hrd->time_offset_length                      = 0;

        bp->seq_parameter_set_id = sps->seq_parameter_set_id;

        // This calculation can easily overflow 32 bits.
        bp->nal.initial_cpb_removal_delay[0] = 90000 *
            (uint64_t)enc->hrd_initial_buffer_fullness / enc->hrd_buffer_size;
        bp->nal.initial_cpb_removal_delay_offset[0] = 0;
    } else {
        sps->vui.nal_hrd_parameters_present_flag = 0;
        sps->vui.low_delay_hrd_flag = 1 - sps->vui.fixed_frame_rate_flag;
    }

    sps->vui.bitstream_restriction_flag    = 1;
    sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
    sps->vui.log2_max_mv_length_horizontal = 15;
    sps->vui.log2_max_mv_length_vertical   = 15;
    sps->vui.max_num_reorder_frames        = enc->max_b_depth;
    sps->vui.max_dec_frame_buffering       = enc->max_b_depth + 1;

    pps->nal_unit_header.nal_ref_idc = 3;
    pps->nal_unit_header.nal_unit_type = H264_NAL_PPS;

    pps->pic_parameter_set_id = 0;
    pps->seq_parameter_set_id = 0;

    pps->entropy_coding_mode_flag =
        !(sps->profile_idc == FF_PROFILE_H264_BASELINE ||
          sps->profile_idc == FF_PROFILE_H264_EXTENDED ||
          sps->profile_idc == FF_PROFILE_H264_CAVLC_444);
    if (!enc->coder && pps->entropy_coding_mode_flag)
        pps->entropy_coding_mode_flag = 0;

    pps->num_ref_idx_l0_default_active_minus1 = 0;
    pps->num_ref_idx_l1_default_active_minus1 = 0;

    pps->pic_init_qp_minus26 = 0; // TODO - fix, I have no idea

    if (sps->profile_idc == FF_PROFILE_H264_BASELINE ||
        sps->profile_idc == FF_PROFILE_H264_EXTENDED ||
        sps->profile_idc == FF_PROFILE_H264_MAIN) {
        pps->more_rbsp_data = 0;
    } else {
        pps->more_rbsp_data = 1;

        pps->transform_8x8_mode_flag = 1;
    }

    *vksps_scaling = (StdVideoH264ScalingLists) {
        .scaling_list_present_mask = sps->seq_scaling_matrix_present_flag,
        .use_default_scaling_matrix_mask = 1,
    };

    *vksps_vui_header = (StdVideoH264HrdParameters) {
        .cpb_cnt_minus1 = hrd->cpb_cnt_minus1,
        .bit_rate_scale = hrd->bit_rate_scale,
        .initial_cpb_removal_delay_length_minus1 = hrd->initial_cpb_removal_delay_length_minus1,
        .cpb_removal_delay_length_minus1 = hrd->cpb_removal_delay_length_minus1,
        .dpb_output_delay_length_minus1 = hrd->dpb_output_delay_length_minus1,
        .time_offset_length = hrd->time_offset_length,
    };

    for (int i = 0; i < H264_MAX_CPB_CNT; i++) {
        vksps_vui_header->bit_rate_value_minus1[i] = hrd->bit_rate_value_minus1[i];
        vksps_vui_header->cpb_size_value_minus1[i] = hrd->cpb_size_value_minus1[i];
        vksps_vui_header->cbr_flag[i] = hrd->cbr_flag[i];
    }

    *vksps_vui = (StdVideoH264SequenceParameterSetVui) {
        .aspect_ratio_idc = sps->vui.aspect_ratio_idc,
        .sar_width = sps->vui.sar_width,
        .sar_height = sps->vui.sar_height,
        .video_format = sps->vui.video_format,
        .colour_primaries = sps->vui.colour_primaries,
        .transfer_characteristics = sps->vui.transfer_characteristics,
        .matrix_coefficients = sps->vui.matrix_coefficients,
        .num_units_in_tick = sps->vui.num_units_in_tick,
        .time_scale = sps->vui.time_scale,
        .pHrdParameters = vksps_vui_header,
        .max_num_reorder_frames = sps->vui.max_num_reorder_frames,
        .max_dec_frame_buffering = sps->vui.max_dec_frame_buffering,
        .flags = (StdVideoH264SpsVuiFlags) {
            .aspect_ratio_info_present_flag = sps->vui.aspect_ratio_info_present_flag,
            .overscan_info_present_flag = sps->vui.overscan_info_present_flag,
            .overscan_appropriate_flag = sps->vui.overscan_appropriate_flag,
            .video_signal_type_present_flag = sps->vui.video_signal_type_present_flag,
            .video_full_range_flag = sps->vui.video_full_range_flag,
            .color_description_present_flag = sps->vui.colour_description_present_flag,
            .chroma_loc_info_present_flag = sps->vui.chroma_loc_info_present_flag,
            .timing_info_present_flag = sps->vui.timing_info_present_flag,
            .fixed_frame_rate_flag = sps->vui.fixed_frame_rate_flag,
            .bitstream_restriction_flag = sps->vui.bitstream_restriction_flag,
            .nal_hrd_parameters_present_flag = sps->vui.nal_hrd_parameters_present_flag,
            .vcl_hrd_parameters_present_flag = sps->vui.vcl_hrd_parameters_present_flag,
        },
    };

    *vksps = (StdVideoH264SequenceParameterSet) {
        .profile_idc = sps->profile_idc,
        .level_idc = sps->level_idc,
        .seq_parameter_set_id = sps->seq_parameter_set_id,
        .chroma_format_idc = sps->chroma_format_idc,
        .bit_depth_luma_minus8 = sps->bit_depth_luma_minus8,
        .bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8,
        .log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4,
        .pic_order_cnt_type = sps->pic_order_cnt_type,
        .log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4,
        .offset_for_non_ref_pic = sps->offset_for_non_ref_pic,
        .offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field,
        .num_ref_frames_in_pic_order_cnt_cycle = sps->num_ref_frames_in_pic_order_cnt_cycle,
        .max_num_ref_frames = sps->max_num_ref_frames,
        .pic_width_in_mbs_minus1 = sps->pic_width_in_mbs_minus1,
        .pic_height_in_map_units_minus1 = sps->pic_height_in_map_units_minus1,
        .frame_crop_left_offset = sps->frame_crop_left_offset,
        .frame_crop_right_offset = sps->frame_crop_right_offset,
        .frame_crop_top_offset = sps->frame_crop_top_offset,
        .frame_crop_bottom_offset = sps->frame_crop_bottom_offset,
        .flags = (StdVideoH264SpsFlags) {
            .constraint_set0_flag = sps->constraint_set0_flag,
            .constraint_set1_flag = sps->constraint_set1_flag,
            .constraint_set2_flag = sps->constraint_set2_flag,
            .constraint_set3_flag = sps->constraint_set3_flag,
            .constraint_set4_flag = sps->constraint_set4_flag,
            .constraint_set5_flag = sps->constraint_set5_flag,
            .direct_8x8_inference_flag = sps->direct_8x8_inference_flag,
            .mb_adaptive_frame_field_flag = sps->mb_adaptive_frame_field_flag,
            .frame_mbs_only_flag = sps->frame_mbs_only_flag,
            .delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag,
            .separate_colour_plane_flag = sps->separate_colour_plane_flag,
            .gaps_in_frame_num_value_allowed_flag = sps->gaps_in_frame_num_allowed_flag,
            .qpprime_y_zero_transform_bypass_flag = sps->qpprime_y_zero_transform_bypass_flag,
            .frame_cropping_flag = sps->frame_cropping_flag,
            .seq_scaling_matrix_present_flag = sps->seq_scaling_matrix_present_flag,
            .vui_parameters_present_flag = sps->vui_parameters_present_flag,
        },
        .pOffsetForRefFrame = sps->offset_for_ref_frame,
        .pSequenceParameterSetVui = vksps_vui,
    };

    *vkpps_scaling = (StdVideoH264ScalingLists) {
        .scaling_list_present_mask = pps->pic_scaling_matrix_present_flag,
        .use_default_scaling_matrix_mask = 1,
    };

    *vkpps = (StdVideoH264PictureParameterSet) {
        .seq_parameter_set_id = pps->seq_parameter_set_id,
        .pic_parameter_set_id = pps->pic_parameter_set_id,
        .num_ref_idx_l0_default_active_minus1 = pps->num_ref_idx_l0_default_active_minus1,
        .num_ref_idx_l1_default_active_minus1 = pps->num_ref_idx_l1_default_active_minus1,
        .weighted_bipred_idc = pps->weighted_bipred_idc,
        .pic_init_qp_minus26 = pps->pic_init_qp_minus26,
        .pic_init_qs_minus26 = pps->pic_init_qs_minus26,
        .chroma_qp_index_offset = pps->chroma_qp_index_offset,
        .second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset,
        .flags = (StdVideoH264PpsFlags) {
            .transform_8x8_mode_flag = pps->transform_8x8_mode_flag,
            .redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag,
            .constrained_intra_pred_flag = pps->constrained_intra_pred_flag,
            .deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag,
            .weighted_pred_flag = pps->weighted_pred_flag,
            .bottom_field_pic_order_in_frame_present_flag = pps->bottom_field_pic_order_in_frame_present_flag,
            .entropy_coding_mode_flag = pps->entropy_coding_mode_flag,
            .pic_scaling_matrix_present_flag = pps->pic_scaling_matrix_present_flag,
        },
    };

    return 0;
}

static int vulkan_encode_h264_add_nal(AVCodecContext *avctx,
                                      CodedBitstreamFragment *au,
                                      void *nal_unit)
{
    H264RawNALUnitHeader *header = nal_unit;

    int err = ff_cbs_insert_unit_content(au, -1,
                                         header->nal_unit_type, nal_unit, NULL);
    if (err < 0)
        av_log(avctx, AV_LOG_ERROR, "Failed to add NAL unit: "
               "type = %d.\n", header->nal_unit_type);

    return err;
}

static int vulkan_encode_h264_write_access_unit(AVCodecContext *avctx,
                                                uint8_t *data, size_t *data_len,
                                                CodedBitstreamFragment *au)
{
    VulkanEncodeH264Context *enc = avctx->priv_data;

    int err = ff_cbs_write_fragment_data(enc->cbc, au);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to write packed header.\n");
        return err;
    }

    if (*data_len < au->data_size) {
        av_log(avctx, AV_LOG_ERROR, "Access unit too large: %zu < %zu.\n",
               *data_len, au->data_size);
        return AVERROR(ENOSPC);
    }

    memcpy(data, au->data, au->data_size);
    *data_len = au->data_size;

    return 0;
}

static int vulkan_encode_h264_write_sequence_header(AVCodecContext *avctx,
                                                    uint8_t *data, size_t *data_len)
{
    int err;
    VulkanEncodeH264Context *enc = avctx->priv_data;
    CodedBitstreamFragment   *au = &enc->current_access_unit;

    if (enc->write_units & UNIT_AUD) {
        err = vulkan_encode_h264_add_nal(avctx, au, &enc->raw_aud);
        if (err < 0)
            goto fail;
    }

    err = vulkan_encode_h264_add_nal(avctx, au, &enc->raw_sps);
    if (err < 0)
        goto fail;

    err = vulkan_encode_h264_add_nal(avctx, au, &enc->raw_pps);
    if (err < 0)
        goto fail;

    err = vulkan_encode_h264_write_access_unit(avctx, data, data_len, au);
fail:
    ff_cbs_fragment_reset(au);
    return err;
}

static int vulkan_encode_h264_write_filler(AVCodecContext *avctx, uint32_t filler,
                                           uint8_t *data, size_t *data_len)
{
    int err;
    VulkanEncodeH264Context *enc = avctx->priv_data;
    CodedBitstreamFragment   *au = &enc->current_access_unit;

    H264RawFiller raw_filler = {
        .nal_unit_header = {
            .nal_unit_type = H264_NAL_FILLER_DATA,
        },
        .filler_size = filler,
    };

    err = vulkan_encode_h264_add_nal(avctx, au, &raw_filler);
    if (err < 0)
        goto fail;

    err = vulkan_encode_h264_write_access_unit(avctx, data, data_len, au);
fail:
    ff_cbs_fragment_reset(au);
    return err;
}

static av_cold int vulkan_encode_h264_create_session(AVCodecContext *avctx)
{
    VkResult ret;
    VulkanEncodeH264Context *enc = avctx->priv_data;
    FFVulkanFunctions *vk = &enc->vkenc.s.vkfn;

    VkVideoEncodeH264SessionParametersAddInfoEXT h264_params_info;
    VkVideoEncodeH264SessionParametersCreateInfoEXT h264_params;
    VkVideoSessionParametersCreateInfoKHR session_params_create;

    h264_params_info = (VkVideoEncodeH264SessionParametersAddInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT,
        .pStdSPSs = &enc->vksps,
        .stdSPSCount = 1,
        .pStdPPSs = &enc->vkpps,
        .stdPPSCount = 1,
    };
    h264_params = (VkVideoEncodeH264SessionParametersCreateInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT,
        .maxStdSPSCount = 1,
        .maxStdPPSCount = 1,
        .pParametersAddInfo = &h264_params_info,
    };
    session_params_create = (VkVideoSessionParametersCreateInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
        .pNext = &h264_params,
        .videoSession = enc->vkenc.common.session,
        .videoSessionParametersTemplate = NULL,
    };

    /* Create session parameters */
    ret = vk->CreateVideoSessionParametersKHR(enc->vkenc.s.hwctx->act_dev, &session_params_create,
                                              enc->vkenc.s.hwctx->alloc, &enc->vkenc.session_params);
    if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to create Vulkan video session parameters: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int vulkan_encode_h264_init_pic_headers(AVCodecContext *avctx,
                                               FFVulkanEncodePicture *pic)
{
    VulkanEncodeH264Context    *enc = avctx->priv_data;
    VulkanEncodeH264Picture   *hpic = pic->priv_data;
    FFVulkanEncodePicture     *prev = pic->prev;
    VulkanEncodeH264Picture  *hprev = prev ? prev->priv_data : NULL;

    int qp = pic->qp;
    int cpb_delay;
    int dpb_delay;
    int primary_pic_type;
    int slice_type;

    if (pic->type == FF_VK_FRAME_KEY) {
        av_assert0(pic->display_order == pic->encode_order);

        hpic->frame_num      = 0;
        hpic->last_idr_frame = pic->display_order;
        hpic->idr_pic_id     = hprev ? hprev->idr_pic_id + 1 : 0;

        primary_pic_type = 0;
        slice_type       = 7; // SPEC: add slice types above 5
    } else {
        av_assert0(prev);

        hpic->frame_num      = hprev->frame_num + prev->is_reference;
        hpic->last_idr_frame = hprev->last_idr_frame;
        hpic->idr_pic_id     = hprev->idr_pic_id;

        /* SPEC: missing StdVideoH264PictureType entries */
        if (pic->type == FF_VK_FRAME_I) {
            slice_type       = 7;
            primary_pic_type = 0;
        } else if (pic->type == FF_VK_FRAME_P) {
            slice_type       = 5;
            primary_pic_type = 1;
        } else {
            slice_type       = 6;
            primary_pic_type = 2;
        }
    }

    hpic->pic_order_cnt = pic->display_order - hpic->last_idr_frame;
    if (enc->raw_sps.pic_order_cnt_type == 2)
        hpic->pic_order_cnt *= 2;

    dpb_delay     = pic->display_order - pic->encode_order + enc->max_b_depth;
    cpb_delay     = pic->encode_order - hpic->last_idr_frame;

    enc->write_units = 0x0;

    if (pic->display_order == 0 && enc->insert_units & UNIT_IDENTIFIER)
        enc->write_units |= UNIT_IDENTIFIER;

    if (enc->insert_units & UNIT_AUD) {
        enc->raw_aud = (H264RawAUD) {
            .nal_unit_header = {
                .nal_unit_type = H264_NAL_AUD,
            },
            .primary_pic_type = primary_pic_type,
        };
        enc->write_units |= UNIT_AUD;
    }
    if (enc->insert_units & UNIT_TIMING) {
        enc->sei_pic_timing = (H264RawSEIPicTiming) {
            .cpb_removal_delay = 2 * cpb_delay,
            .dpb_output_delay  = 2 * dpb_delay,
        };
        enc->write_units |= UNIT_TIMING;
    }
    if (enc->insert_units & UNIT_RECOVERY && pic->type == FF_VK_FRAME_I) {
        enc->sei_recovery_point = (H264RawSEIRecoveryPoint) {
            .recovery_frame_cnt = 0,
            .exact_match_flag   = 1,
            .broken_link_flag   = enc->b_per_p > 0,
        };
        enc->write_units |= UNIT_RECOVERY;
    }

    hpic->slice_wt = (StdVideoEncodeH264WeightTable) {
        .flags = (StdVideoEncodeH264WeightTableFlags) {
            .luma_weight_l0_flag = 0,
            .chroma_weight_l0_flag = 0,
            .luma_weight_l1_flag = 0,
            .chroma_weight_l1_flag = 0,
        },
        .luma_log2_weight_denom = 0,
        .chroma_log2_weight_denom = 0,
        .luma_weight_l0 = { 0 },
        .luma_offset_l0 = { 0 },
        .chroma_weight_l0 = { { 0 } },
        .chroma_offset_l0 = { { 0 } },
        .luma_weight_l1 = { 0 },
        .luma_offset_l1 = { 0 },
        .chroma_weight_l1 = { { 0 } },
        .chroma_offset_l1 = { { 0 } },
    };

    hpic->slice_hdr = (StdVideoEncodeH264SliceHeader) {
        .flags = (StdVideoEncodeH264SliceHeaderFlags) {
            .direct_spatial_mv_pred_flag = 0,
            .num_ref_idx_active_override_flag = 0,
            .no_output_of_prior_pics_flag = 0,
            .adaptive_ref_pic_marking_mode_flag = 0,
            .no_prior_references_available_flag = 0,
        },
        .first_mb_in_slice = 0,
        .slice_type = slice_type,
        .idr_pic_id = hpic->idr_pic_id,
        .num_ref_idx_l0_active_minus1 = 0,
        .num_ref_idx_l1_active_minus1 = 0,
        .cabac_init_idc = 0,
        .disable_deblocking_filter_idc = 1,
        .slice_alpha_c0_offset_div2 = 0,
        .slice_beta_offset_div2 = 0,
        .pWeightTable = &hpic->slice_wt,
    };

    hpic->vkslice = (VkVideoEncodeH264NaluSliceInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_EXT,
        .pNext = NULL,
        .mbCount = enc->mb_width * enc->mb_height,
        .pStdReferenceFinalLists = NULL,
        .pStdSliceHeader = &hpic->slice_hdr,
    };

    hpic->h264pic_info = (StdVideoEncodeH264PictureInfo) {
        .flags = (StdVideoEncodeH264PictureInfoFlags) {
            .idr_flag = pic->type == FF_VK_FRAME_KEY,
            .is_reference_flag = pic->is_reference,
            .used_for_long_term_reference = 0,
        },
        .seq_parameter_set_id = enc->raw_sps.seq_parameter_set_id,
        .pic_parameter_set_id = enc->raw_pps.pic_parameter_set_id,
        .pictureType = pic->type == FF_VK_FRAME_P ? STD_VIDEO_H264_PICTURE_TYPE_P :
                       pic->type == FF_VK_FRAME_B ? STD_VIDEO_H264_PICTURE_TYPE_B :
                       pic->type == FF_VK_FRAME_I ? STD_VIDEO_H264_PICTURE_TYPE_I :
                                                    STD_VIDEO_H264_PICTURE_TYPE_IDR,
        .frame_num = hpic->frame_num,
        .PicOrderCnt = hpic->pic_order_cnt,
    };

#if 0
    hpic->ref_list_info = (StdVideoEncodeH264ReferenceListsInfo) {
        .flags                    = (StdVideoEncodeH264ReferenceListsInfoFlags) {
            .ref_pic_list_modification_flag_l0 = 0,
            .ref_pic_list_modification_flag_l1 = 0,
        },
        .pRefList0ModOperations   = hpic->l0mods,
        .refList0ModOpCount       = 0,
        .pRefList1ModOperations   = hpic->l1mods,
        .refList1ModOpCount       = 0,
        .pRefPicMarkingOperations = hpic->marks,
        .refPicMarkingOpCount     = 0,
    };

    for (int i = 0; i < pic->nb_refs; i++) {
        FFVulkanEncodePicture *ref = pic->refs[i];
        VulkanEncodeH264Picture *href = ref->priv_data;

        hpic->l0ref_info[0] = (StdVideoEncodeH264ReferenceInfo) {
            .flags = (StdVideoEncodeH264ReferenceInfoFlags) {
                .used_for_long_term_reference = 0,
            },
            .FrameNum = href->frame_num,
            .PicOrderCnt = href->pic_order_cnt,
            .long_term_pic_num = 0,
            .long_term_frame_idx = 0,
        };

        hpic->l0refs[i] = (VkVideoEncodeH264DpbSlotInfoEXT) {
            .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_EXT,
            .pStdReferenceInfo = &hpic->l0ref_info[i],
        };
    }

    hpic->final_list = (StdVideoEncodeH264ReferenceListsInfo) {
        .pReferenceList0Entries   = hpic->l0refs,
        .referenceList0EntryCount = pic->nb_refs,
        .pReferenceList1Entries   = hpic->l1refs,
        .pReferenceList1Entries   = 0,
        .pMemMgmtCtrlOperations   = &hpic->mem_mgmt,
    };
#endif

    hpic->vkh264pic_info = (VkVideoEncodeH264VclFrameInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_VCL_FRAME_INFO_EXT,
        .pNext = NULL,
        .pStdReferenceFinalLists = &hpic->final_list,
        .naluSliceEntryCount = 1,
        .pNaluSliceEntries = &hpic->vkslice,
        .pStdPictureInfo = &hpic->h264pic_info,
    };

    hpic->vkrc_layer_info = (VkVideoEncodeH264RateControlLayerInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT,
        .minQp = (VkVideoEncodeH264QpEXT){ qp, qp, qp },
        .maxQp = (VkVideoEncodeH264QpEXT){ qp, qp, qp },
        .useMinQp = 1,
        .useMaxQp = 1,
    };

    pic->codec_info     = &hpic->vkh264pic_info;
    pic->codec_rc_layer = &hpic->vkrc_layer_info;

    return 0;
}

static int vulkan_encode_h264_write_extra_headers(AVCodecContext *avctx,
                                                  FFVulkanEncodePicture *pic,
                                                  uint8_t *data, size_t *data_len)
{
    int err;
    VulkanEncodeH264Context *enc = avctx->priv_data;
    CodedBitstreamFragment   *au = &enc->current_access_unit;

    if (enc->write_units) {
        if (enc->write_units & UNIT_AUD) {
            err = vulkan_encode_h264_add_nal(avctx, au, &enc->raw_aud);
            if (err < 0)
                goto fail;
        }

        if (enc->write_units & UNIT_IDENTIFIER) {
            err = ff_cbs_sei_add_message(enc->cbc, au, 1,
                                         SEI_TYPE_USER_DATA_UNREGISTERED,
                                         &enc->sei_identifier, NULL);
            if (err < 0)
                goto fail;
        }
        if (enc->write_units & UNIT_TIMING) {
            if (pic->type == FF_VK_FRAME_KEY) {
                err = ff_cbs_sei_add_message(enc->cbc, au, 1,
                                             SEI_TYPE_BUFFERING_PERIOD,
                                             &enc->sei_buffering_period, NULL);
                if (err < 0)
                    goto fail;
            }
            err = ff_cbs_sei_add_message(enc->cbc, au, 1,
                                         SEI_TYPE_PIC_TIMING,
                                         &enc->sei_pic_timing, NULL);
            if (err < 0)
                goto fail;
        }
        if (enc->write_units & UNIT_RECOVERY) {
            err = ff_cbs_sei_add_message(enc->cbc, au, 1,
                                         SEI_TYPE_RECOVERY_POINT,
                                         &enc->sei_recovery_point, NULL);
            if (err < 0)
                goto fail;
        }

        err = vulkan_encode_h264_write_access_unit(avctx, data, data_len, au);
        if (err < 0)
            goto fail;

        ff_cbs_fragment_reset(au);

        return 0;
    }

fail:
    ff_cbs_fragment_reset(au);
    return err;
}

static const FFVulkanEncoder encoder = {
    .pic_priv_data_size = sizeof(VulkanEncodeH264Picture),
    .write_stream_headers = vulkan_encode_h264_write_sequence_header,
    .init_pic_headers = vulkan_encode_h264_init_pic_headers,
    .write_filler = vulkan_encode_h264_write_filler,
    .filler_header_size = 6,
    .write_extra_headers = vulkan_encode_h264_write_extra_headers,
};

static av_cold int vulkan_encode_h264_init(AVCodecContext *avctx)
{
    int err;
    VulkanEncodeH264Context *enc = avctx->priv_data;

    enc->profile = (VkVideoEncodeH264ProfileInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_EXT,
        .stdProfileIdc = enc->vkenc.opts.profile,
    };

    enc->caps = (VkVideoEncodeH264CapabilitiesEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT,
    };

    err = ff_cbs_init(&enc->cbc, AV_CODEC_ID_H264, avctx);
    if (err < 0)
        return err;

    enc->mb_width  = FFALIGN(avctx->width,  16) / 16;
    enc->mb_height = FFALIGN(avctx->height, 16) / 16;

    enc->bit_rate    = avctx->bit_rate;
    enc->gop_size    = 3; /* avctx->gop_size; */
    enc->b_per_p     = 0; /* avctx->max_b_frames; */
    enc->max_b_depth = 0; /* FFMIN(enc->desired_b_depth,
                             av_log2(enc->b_per_p) + 1); */

    enc->vkenc.gop_size = enc->gop_size;
    enc->vkenc.bitrate =enc->bit_rate;

    err = ff_vulkan_encode_init(avctx, &enc->vkenc, &enc->profile, &enc->caps,
                                &encoder, enc->b_per_p, enc->max_b_depth);
    if (err < 0)
        return err;

    av_log(avctx, AV_LOG_VERBOSE, "H264 encoder capabilities:\n");
    av_log(avctx, AV_LOG_VERBOSE, "    Capability flags:\n");
    av_log(avctx, AV_LOG_VERBOSE, "        8x8_inference:%s%s\n",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DIRECT_8X8_INFERENCE_ENABLED_BIT_EXT ?
               " enabling" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DIRECT_8X8_INFERENCE_DISABLED_BIT_EXT ?
               " disabling" : "");
    av_log(avctx, AV_LOG_VERBOSE, "        separate_color_plane: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_SEPARATE_COLOUR_PLANE_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        qprime_y_zero_transform_bypass: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_QPPRIME_Y_ZERO_TRANSFORM_BYPASS_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        scaling_lists: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_SCALING_LISTS_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        hrd_compliance: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_HRD_COMPLIANCE_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        chroma_qp_offset: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_CHROMA_QP_OFFSET_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        second_chroma_qp_offset: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_SECOND_CHROMA_QP_OFFSET_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        pic_init_qp: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_PIC_INIT_QP_MINUS26_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        weighted:%s%s%s%s\n",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_WEIGHTED_PRED_BIT_EXT ?
               " pred" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_WEIGHTED_BIPRED_EXPLICIT_BIT_EXT ?
               " bipred_explicit" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_WEIGHTED_BIPRED_IMPLICIT_BIT_EXT ?
               " bipred_implicit" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_WEIGHTED_PRED_NO_TABLE_BIT_EXT ?
               " pred_no_table" : "");
    av_log(avctx, AV_LOG_VERBOSE, "        8x8_transforms: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_TRANSFORM_8X8_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        coder:%s%s\n",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_CABAC_BIT_EXT ?
               " cabac" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_CAVLC_BIT_EXT ?
               " cavlc" : "");
    av_log(avctx, AV_LOG_VERBOSE, "        deblock:%s%s%s\n",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DEBLOCKING_FILTER_DISABLED_BIT_EXT ?
               " filter_disabling" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DEBLOCKING_FILTER_ENABLED_BIT_EXT ?
               " filter_enabling" : "",
           enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DEBLOCKING_FILTER_PARTIAL_BIT_EXT ?
               " filter_partial" : "");
    av_log(avctx, AV_LOG_VERBOSE, "        disable_direct_spatial_mv_pred: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DISABLE_DIRECT_SPATIAL_MV_PRED_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        multiple_slices: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_MULTIPLE_SLICE_PER_FRAME_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        slice_mb_count: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_SLICE_MB_COUNT_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        row_unaligned_slice: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_ROW_UNALIGNED_SLICE_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        different_slice_type: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_DIFFERENT_SLICE_TYPE_BIT_EXT));
    av_log(avctx, AV_LOG_VERBOSE, "        b_frame_in_l1_list: %i\n",
           !!(enc->caps.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_B_FRAME_IN_L1_LIST_BIT_EXT));

    av_log(avctx, AV_LOG_VERBOSE, "    L0_refs: %i P's; %i B's\n",
           enc->caps.maxPPictureL0ReferenceCount,
           enc->caps.maxBPictureL0ReferenceCount);

    av_log(avctx, AV_LOG_VERBOSE, "    L1_refs: %i\n",
           enc->caps.maxL1ReferenceCount);

    av_log(avctx, AV_LOG_VERBOSE, "    bitstream_restriction:\n");
    av_log(avctx, AV_LOG_VERBOSE, "        mvs_over_pic_boundaries: %i\n",
           enc->caps.motionVectorsOverPicBoundariesFlag);
    av_log(avctx, AV_LOG_VERBOSE, "        max_bytes_per_pic_denom: %i\n",
           enc->caps.maxBytesPerPicDenom);
    av_log(avctx, AV_LOG_VERBOSE, "        max_bits_per_mb_denom: %i\n",
           enc->caps.maxBitsPerMbDenom);
    av_log(avctx, AV_LOG_VERBOSE, "        log2_max_mv_length_hor: %i\n",
           enc->caps.log2MaxMvLengthHorizontal);
    av_log(avctx, AV_LOG_VERBOSE, "        log2_max_mv_length_ver: %i\n",
           enc->caps.log2MaxMvLengthVertical);

    if (enc->insert_units & UNIT_IDENTIFIER) {
        int len;

        memcpy(enc->sei_identifier.uuid_iso_iec_11578,
               vulkan_encode_h264_sei_identifier_uuid,
               sizeof(enc->sei_identifier.uuid_iso_iec_11578));

        len = snprintf(NULL, 0,
                       "%s / Vulkan video %i.%i.%i / %s %i.%i.%i / %s",
                       LIBAVCODEC_IDENT,
                       CODEC_VER(ff_vk_enc_ext[avctx->codec_id].specVersion),
                       enc->vkenc.s.driver_props.driverName,
                       CODEC_VER(enc->vkenc.s.props.properties.driverVersion),
                       enc->vkenc.s.props.properties.deviceName);

        if (len >= 0) {
            enc->sei_identifier_string = av_malloc(len + 1);
            if (!enc->sei_identifier_string)
                return AVERROR(ENOMEM);

            len = snprintf(enc->sei_identifier_string, len + 1,
                           "%s / Vulkan video %i.%i.%i / %s %i.%i.%i / %s",
                           LIBAVCODEC_IDENT,
                           CODEC_VER(ff_vk_enc_ext[avctx->codec_id].specVersion),
                           enc->vkenc.s.driver_props.driverName,
                           CODEC_VER(enc->vkenc.s.props.properties.driverVersion),
                           enc->vkenc.s.props.properties.deviceName);

            enc->sei_identifier.data        = enc->sei_identifier_string;
            enc->sei_identifier.data_length = len + 1;
        }
    }

    err = vulkan_encode_h264_init_seq_params(avctx);
    if (err < 0)
        return err;

    err = vulkan_encode_h264_create_session(avctx);
    if (err < 0)
        return err;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        uint8_t data[4096];
        size_t data_len = sizeof(data);

        err = vulkan_encode_h264_write_sequence_header(avctx, data, &data_len);
        if (err < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write sequence header "
                   "for extradata: %d.\n", err);
            return err;
        } else {
            avctx->extradata_size = data_len;
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata) {
                err = AVERROR(ENOMEM);
                return err;
            }
            memcpy(avctx->extradata, data, avctx->extradata_size);
        }
    }

    return 0;
}

static av_cold int vulkan_encode_h264_close(AVCodecContext *avctx)
{
    VulkanEncodeH264Context *enc = avctx->priv_data;
    ff_vulkan_encode_uninit(&enc->vkenc);
    return 0;
}

static int video_encode_h264_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    VulkanEncodeH264Context *enc = avctx->priv_data;
    return ff_vulkan_encode_receive_packet(avctx, &enc->vkenc, pkt);
}

static void vulkan_encode_h264_flush(AVCodecContext *avctx)
{

}

#define OFFSET(x) offsetof(VulkanEncodeH264Context, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM)
static const AVOption vulkan_encode_h264_options[] = {
    { "profile", "Select profile", OFFSET(vkenc.opts.profile), AV_OPT_TYPE_INT, { .i64 = FF_PROFILE_H264_MAIN }, 0, INT_MAX, FLAGS, "profile" },
        { "baseline", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE            }, INT_MIN, INT_MAX, FLAGS, "profile" },
        { "main",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN                }, INT_MIN, INT_MAX, FLAGS, "profile" },
        { "high",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH                }, INT_MIN, INT_MAX, FLAGS, "profile" },
        { "high444p", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH_444_PREDICTIVE }, INT_MIN, INT_MAX, FLAGS, "profile" },

    { "b_depth", "Maximum B-frame reference depth", OFFSET(desired_b_depth), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, INT_MAX, FLAGS },

    { "coder", "Entropy coder type", OFFSET(coder), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, FLAGS, "coder" },
        { "cabac", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, INT_MIN, INT_MAX, FLAGS, "coder" },
        { "vlc",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, INT_MIN, INT_MAX, FLAGS, "coder" },

    { "units", "Set units to include", OFFSET(insert_units), AV_OPT_TYPE_FLAGS, { .i64 = UNIT_IDENTIFIER | UNIT_AUD | UNIT_RECOVERY }, 0, INT_MAX, FLAGS, "units" },
        { "identifier", "Include encoder version identifier", 0, AV_OPT_TYPE_CONST, { .i64 = UNIT_IDENTIFIER }, INT_MIN, INT_MAX, FLAGS, "units" },
        { "aud",        "Include AUD units", 0, AV_OPT_TYPE_CONST, { .i64 = UNIT_AUD }, INT_MIN, INT_MAX, FLAGS, "units" },
        { "timing",     "Include timing parameters (buffering_period and pic_timing)", 0, AV_OPT_TYPE_CONST, { .i64 = UNIT_TIMING }, INT_MIN, INT_MAX, FLAGS, "units" },
        { "recovery",   "Include recovery points where appropriate", 0, AV_OPT_TYPE_CONST, { .i64 = UNIT_RECOVERY }, INT_MIN, INT_MAX, FLAGS, "units" },

    FF_VK_ENCODE_COMMON_OPTS

    { NULL },
};

static const FFCodecDefault vulkan_encode_h264_defaults[] = {
    { "b",              "0"   },
    { "g",              "120" },
    { NULL },
};

static const AVClass vulkan_encode_h264_class = {
    .class_name = "h264_vulkan",
    .item_name  = av_default_item_name,
    .option     = vulkan_encode_h264_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_vulkan_encoder = {
    .p.name         = "h264_vulkan",
    CODEC_LONG_NAME("H.264/AVC (Vulkan)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(VulkanEncodeH264Context),
    .init           = &vulkan_encode_h264_init,
    FF_CODEC_RECEIVE_PACKET_CB(&video_encode_h264_receive_packet),
    .flush          = &vulkan_encode_h264_flush,
    .close          = &vulkan_encode_h264_close,
    .p.priv_class   = &vulkan_encode_h264_class,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE |
                      AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_ENCODER_FLUSH /* | AV_CODEC_CAP_EXPERIMENTAL */,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .defaults       = vulkan_encode_h264_defaults,
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_VULKAN,
        AV_PIX_FMT_NONE,
    },
    .hw_configs     = ff_vulkan_encode_hw_configs,
    .p.wrapper_name = "vulkan",
};
