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

#define TX_FLOAT
#include "libavutil/tx_priv.h"
#include "libavutil/attributes.h"
#include "libavutil/aarch64/cpu.h"

TX_DECL_FN(fft4_fwd,  neon)

static av_cold int rvv_init(AVTXContext *s, const FFTXCodelet *cd,
                            uint64_t flags, FFTXCodeletOptions *opts,
                            int len, int inv, const void *scale)
{
    ff_tx_init_tabs_float(len);
    if (cd->max_len == 2)
        return ff_tx_gen_ptwo_revtab(s, opts);
    else
        return ff_tx_gen_split_radix_parity_revtab(s, len, inv, opts, 8, 0);
}

const FFTXCodelet * const ff_tx_codelet_list_float_rvv[] = {
    TX_DEF(fft4_fwd,  FFT,  4,  4, 2, 0, 128, NULL,      neon, NEON, AV_TX_INPLACE | FF_TX_FORWARD_ONLY, 0),

    NULL,
};
