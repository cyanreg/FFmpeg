/*
 * AAC decoder definitions and structures
 * Copyright (c) 2005-2006 Oded Shimon ( ods15 ods15 dyndns org )
 * Copyright (c) 2006-2007 Maxim Gavrilov ( maxim.gavrilov gmail com )
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

#ifndef AVCODEC_AAC_AACDEC_H
#define AVCODEC_AAC_AACDEC_H

#include "libavcodec/avcodec.h"
#include "aacdec_context.h"

int ff_aac_decode_init_common(AVCodecContext *avctx);
int ff_aac_decode_close(AVCodecContext *avctx);

extern const struct AVClass ff_aac_decoder_class;

extern const AACDspType aac_dsp;
extern const AACDspType aac_dsp_fixed;

#endif /* AVCODEC_AAC_AACDEC_H */
