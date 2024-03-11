/*
 * AAC decoder
 * Copyright (c) 2005-2006 Oded Shimon ( ods15 ods15 dyndns org )
 * Copyright (c) 2006-2007 Maxim Gavrilov ( maxim.gavrilov gmail com )
 * Copyright (c) 2008-2013 Alex Converse <alex.converse@gmail.com>
 *
 * AAC LATM decoder
 * Copyright (c) 2008-2010 Paul Kendall <paul@kcbbs.gen.nz>
 * Copyright (c) 2010      Janne Grunau <janne-libav@jannau.net>
 *
 * AAC decoder fixed-point implementation
 * Copyright (c) 2013
 *      MIPS Technologies, Inc., California.
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

#include "aacdec.h"

/**
 * Decode coupling_channel_element; reference: table 4.8.
 *
 * @return  Returns error status. 0 - OK, !0 - error
 */
static int decode_cce(AACDecContext *ac, GetBitContext *gb, ChannelElement *che)
{
    int num_gain = 0;
    int c, g, sfb, ret;
    int sign;
    INTFLOAT scale;
    SingleChannelElement *sce = &che->ch[0];
    ChannelCoupling     *coup = &che->coup;

    coup->coupling_point = 2 * get_bits1(gb);
    coup->num_coupled = get_bits(gb, 3);
    for (c = 0; c <= coup->num_coupled; c++) {
        num_gain++;
        coup->type[c] = get_bits1(gb) ? TYPE_CPE : TYPE_SCE;
        coup->id_select[c] = get_bits(gb, 4);
        if (coup->type[c] == TYPE_CPE) {
            coup->ch_select[c] = get_bits(gb, 2);
            if (coup->ch_select[c] == 3)
                num_gain++;
        } else
            coup->ch_select[c] = 2;
    }
    coup->coupling_point += get_bits1(gb) || (coup->coupling_point >> 1);

    sign  = get_bits(gb, 1);
#if USE_FIXED
    scale = get_bits(gb, 2);
#else
    scale = cce_scale[get_bits(gb, 2)];
#endif

    if ((ret = decode_ics(ac, sce, gb, 0, 0)))
        return ret;

    for (c = 0; c < num_gain; c++) {
        int idx  = 0;
        int cge  = 1;
        int gain = 0;
        INTFLOAT gain_cache = FIXR10(1.);
        if (c) {
            cge = coup->coupling_point == AFTER_IMDCT ? 1 : get_bits1(gb);
            gain = cge ? get_vlc2(gb, ff_vlc_scalefactors, 7, 3) - 60: 0;
            gain_cache = GET_GAIN(scale, gain);
#if USE_FIXED
            if ((abs(gain_cache)-1024) >> 3 > 30)
                return AVERROR(ERANGE);
#endif
        }
        if (coup->coupling_point == AFTER_IMDCT) {
            coup->gain[c][0] = gain_cache;
        } else {
            for (g = 0; g < sce->ics.num_window_groups; g++) {
                for (sfb = 0; sfb < sce->ics.max_sfb; sfb++, idx++) {
                    if (sce->band_type[idx] != ZERO_BT) {
                        if (!cge) {
                            int t = get_vlc2(gb, ff_vlc_scalefactors, 7, 3) - 60;
                            if (t) {
                                int s = 1;
                                t = gain += t;
                                if (sign) {
                                    s  -= 2 * (t & 0x1);
                                    t >>= 1;
                                }
                                gain_cache = GET_GAIN(scale, t) * s;
#if USE_FIXED
                                if ((abs(gain_cache)-1024) >> 3 > 30)
                                    return AVERROR(ERANGE);
#endif
                            }
                        }
                        coup->gain[c][idx] = gain_cache;
                    }
                }
            }
        }
    }
    return 0;
}

/**
 * Convert integer scalefactors to the decoder's native expected
 * scalefactor values.
 */
static void convert_scalefactors(INTFLOAT sf[120], int sfo[120],
                                 IndividualChannelStream *ics,
                                 enum BandType band_type[120],
                                 int band_type_run_end[120])
{
    int g, i, idx = 0;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb;) {
            int run_end = band_type_run_end[idx];
            switch (band_type[idx]) {
            case ZERO_BT:
                sf[idx] = FIXR(0.);
                break;
            case INTENSITY_BT: /* fallthrough */
            case INTENSITY_BT2:
                for (; i < run_end; i++, idx++) {
#if USE_FIXED
                    sf[idx] = 100 - sfo[idx];
#else
                    sf[idx] = ff_aac_pow2sf_tab[-sfo[idx] + POW_SF2_ZERO];
#endif /* USE_FIXED */
                }
                break;
            default:
                for (; i < run_end; i++, idx++) {
#if USE_FIXED
                    sf[idx] = -sfo[idx];
#else
                    sf[idx] = -ff_aac_pow2sf_tab[sfo[idx] - 100 + POW_SF2_ZERO];
#endif /* USE_FIXED */
                }
                break;
            }
        }
    }
    return 0;
}

/**
 * Decode spectral data; reference: table 4.50.
 * Dequantize and scale spectral data; reference: 4.6.3.3.
 *
 * @param   coef            array of dequantized, scaled spectral data
 * @param   sf              array of scalefactors or intensity stereo positions
 * @param   pulse_present   set if pulses are present
 * @param   pulse           pointer to pulse data struct
 * @param   band_type       array of the used band type
 *
 * @return  Returns error status. 0 - OK, !0 - error
 */
static int decode_spectrum_and_dequant(AACDecContext *ac, void *_coef,
                                       GetBitContext *gb, const int sfo[120],
                                       int pulse_present, const Pulse *pulse,
                                       const IndividualChannelStream *ics,
                                       enum BandType band_type[120],
                                       int band_type_run_end[120])
{
    int i, k, g, idx = 0;
    const int c = 1024 / ics->num_windows;
    const uint16_t *offsets = ics->swb_offset;
    INTFLOAT *coef_base = coef;

    INTFLOAT *coef = _coef,

    /* Convert scalefactor offsets to native scalefactors */
    INTFLOAT sf[120];
    convert_scalefactors(sf, sfo, ics, band_type, band_type_run_end);

    for (g = 0; g < ics->num_windows; g++)
        memset(coef + g * 128 + offsets[ics->max_sfb], 0,
               sizeof(INTFLOAT) * (c - offsets[ics->max_sfb]));

    for (g = 0; g < ics->num_window_groups; g++) {
        unsigned g_len = ics->group_len[g];

        for (i = 0; i < ics->max_sfb; i++, idx++) {
            const unsigned cbt_m1 = band_type[idx] - 1;
            INTFLOAT *cfo = coef + offsets[i];
            int off_len = offsets[i + 1] - offsets[i];
            int group;

            if (cbt_m1 >= INTENSITY_BT2 - 1) {
                for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                    memset(cfo, 0, off_len * sizeof(*cfo));
                }
            } else if (cbt_m1 == NOISE_BT - 1) {
                for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                    INTFLOAT band_energy;
#if USE_FIXED
                    for (k = 0; k < off_len; k++) {
                        ac->random_state  = lcg_random(ac->random_state);
                        cfo[k] = ac->random_state >> 3;
                    }

                    band_energy = ac->fdsp->scalarproduct_fixed(cfo, cfo, off_len);
                    band_energy = fixed_sqrt(band_energy, 31);
                    noise_scale(cfo, sf[idx], band_energy, off_len);
#else
                    float scale;

                    for (k = 0; k < off_len; k++) {
                        ac->random_state  = lcg_random(ac->random_state);
                        cfo[k] = ac->random_state;
                    }

                    band_energy = ac->fdsp->scalarproduct_float(cfo, cfo, off_len);
                    scale = sf[idx] / sqrtf(band_energy);
                    ac->fdsp->vector_fmul_scalar(cfo, cfo, scale, off_len);
#endif /* USE_FIXED */
                }
            } else {
#if !USE_FIXED
                const float *vq = ff_aac_codebook_vector_vals[cbt_m1];
#endif /* !USE_FIXED */
                const VLCElem *vlc_tab = ff_vlc_spectral[cbt_m1];
                OPEN_READER(re, gb);

                switch (cbt_m1 >> 1) {
                case 0:
                    for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                        INTFLOAT *cf = cfo;
                        int len = off_len;

                        do {
                            int code;
                            unsigned cb_idx;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = code;
#if USE_FIXED
                            cf = DEC_SQUAD(cf, cb_idx);
#else
                            cf = VMUL4(cf, vq, cb_idx, sf + idx);
#endif /* USE_FIXED */
                        } while (len -= 4);
                    }
                    break;

                case 1:
                    for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                        INTFLOAT *cf = cfo;
                        int len = off_len;

                        do {
                            int code;
                            unsigned nnz;
                            unsigned cb_idx;
                            uint32_t bits;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = code;
                            nnz = cb_idx >> 8 & 15;
                            bits = nnz ? GET_CACHE(re, gb) : 0;
                            LAST_SKIP_BITS(re, gb, nnz);
#if USE_FIXED
                            cf = DEC_UQUAD(cf, cb_idx, bits);
#else
                            cf = VMUL4S(cf, vq, cb_idx, bits, sf + idx);
#endif /* USE_FIXED */
                        } while (len -= 4);
                    }
                    break;

                case 2:
                    for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                        INTFLOAT *cf = cfo;
                        int len = off_len;

                        do {
                            int code;
                            unsigned cb_idx;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = code;
#if USE_FIXED
                            cf = DEC_SPAIR(cf, cb_idx);
#else
                            cf = VMUL2(cf, vq, cb_idx, sf + idx);
#endif /* USE_FIXED */
                        } while (len -= 2);
                    }
                    break;

                case 3:
                case 4:
                    for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
                        INTFLOAT *cf = cfo;
                        int len = off_len;

                        do {
                            int code;
                            unsigned nnz;
                            unsigned cb_idx;
                            unsigned sign;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = code;
                            nnz = cb_idx >> 8 & 15;
                            sign = nnz ? SHOW_UBITS(re, gb, nnz) << (cb_idx >> 12) : 0;
                            LAST_SKIP_BITS(re, gb, nnz);
#if USE_FIXED
                            cf = DEC_UPAIR(cf, cb_idx, sign);
#else
                            cf = VMUL2S(cf, vq, cb_idx, sign, sf + idx);
#endif /* USE_FIXED */
                        } while (len -= 2);
                    }
                    break;

                default:
                    for (group = 0; group < (AAC_SIGNE)g_len; group++, cfo+=128) {
#if USE_FIXED
                        int *icf = cfo;
                        int v;
#else
                        float *cf = cfo;
                        uint32_t *icf = (uint32_t *) cf;
#endif /* USE_FIXED */
                        int len = off_len;

                        do {
                            int code;
                            unsigned nzt, nnz;
                            unsigned cb_idx;
                            uint32_t bits;
                            int j;

                            UPDATE_CACHE(re, gb);
                            GET_VLC(code, re, gb, vlc_tab, 8, 2);
                            cb_idx = code;

                            if (cb_idx == 0x0000) {
                                *icf++ = 0;
                                *icf++ = 0;
                                continue;
                            }

                            nnz = cb_idx >> 12;
                            nzt = cb_idx >> 8;
                            bits = SHOW_UBITS(re, gb, nnz) << (32-nnz);
                            LAST_SKIP_BITS(re, gb, nnz);

                            for (j = 0; j < 2; j++) {
                                if (nzt & 1<<j) {
                                    uint32_t b;
                                    int n;
                                    /* The total length of escape_sequence must be < 22 bits according
                                       to the specification (i.e. max is 111111110xxxxxxxxxxxx). */
                                    UPDATE_CACHE(re, gb);
                                    b = GET_CACHE(re, gb);
                                    b = 31 - av_log2(~b);

                                    if (b > 8) {
                                        av_log(ac->avctx, AV_LOG_ERROR, "error in spectral data, ESC overflow\n");
                                        return AVERROR_INVALIDDATA;
                                    }

                                    SKIP_BITS(re, gb, b + 1);
                                    b += 4;
                                    n = (1 << b) + SHOW_UBITS(re, gb, b);
                                    LAST_SKIP_BITS(re, gb, b);
#if USE_FIXED
                                    v = n;
                                    if (bits & 1U<<31)
                                        v = -v;
                                    *icf++ = v;
#else
                                    *icf++ = ff_cbrt_tab[n] | (bits & 1U<<31);
#endif /* USE_FIXED */
                                    bits <<= 1;
                                } else {
#if USE_FIXED
                                    v = cb_idx & 15;
                                    if (bits & 1U<<31)
                                        v = -v;
                                    *icf++ = v;
#else
                                    unsigned v = ((const uint32_t*)vq)[cb_idx & 15];
                                    *icf++ = (bits & 1U<<31) | v;
#endif /* USE_FIXED */
                                    bits <<= !!v;
                                }
                                cb_idx >>= 4;
                            }
                        } while (len -= 2);
#if !USE_FIXED
                        ac->fdsp->vector_fmul_scalar(cfo, cfo, sf[idx], off_len);
#endif /* !USE_FIXED */
                    }
                }

                CLOSE_READER(re, gb);
            }
        }
        coef += g_len << 7;
    }

    if (pulse_present) {
        idx = 0;
        for (i = 0; i < pulse->num_pulse; i++) {
            INTFLOAT co = coef_base[ pulse->pos[i] ];
            while (offsets[idx + 1] <= pulse->pos[i])
                idx++;
            if (band_type[idx] != NOISE_BT && sf[idx]) {
                INTFLOAT ico = -pulse->amp[i];
#if USE_FIXED
                if (co) {
                    ico = co + (co > 0 ? -ico : ico);
                }
                coef_base[ pulse->pos[i] ] = ico;
#else
                if (co) {
                    co /= sf[idx];
                    ico = co / sqrtf(sqrtf(fabsf(co))) + (co > 0 ? -ico : ico);
                }
                coef_base[ pulse->pos[i] ] = cbrtf(fabsf(ico)) * ico * sf[idx];
#endif /* USE_FIXED */
            }
        }
    }

#if USE_FIXED
    coef = coef_base;
    idx = 0;
    for (g = 0; g < ics->num_window_groups; g++) {
        unsigned g_len = ics->group_len[g];

        for (i = 0; i < ics->max_sfb; i++, idx++) {
            const unsigned cbt_m1 = band_type[idx] - 1;
            int *cfo = coef + offsets[i];
            int off_len = offsets[i + 1] - offsets[i];
            int group;

            if (cbt_m1 < NOISE_BT - 1) {
                for (group = 0; group < (int)g_len; group++, cfo+=128) {
                    ac->vector_pow43(cfo, off_len);
                    ac->subband_scale(cfo, cfo, sf[idx], 34, off_len, ac->avctx);
                }
            }
        }
        coef += g_len << 7;
    }
#endif /* USE_FIXED */

    return 0;
}

/**
 * Mid/Side stereo decoding; reference: 4.6.8.1.3.
 */
static void apply_mid_side_stereo(AACDecContext *ac, ChannelElement *cpe)
{
    const IndividualChannelStream *ics = &cpe->ch[0].ics;
    INTFLOAT *ch0 = cpe->ch[0].AAC_RENAME(coeffs);
    INTFLOAT *ch1 = cpe->ch[1].AAC_RENAME(coeffs);
    int g, i, group, idx = 0;
    const uint16_t *offsets = ics->swb_offset;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb; i++, idx++) {
            if (cpe->ms_mask[idx] &&
                cpe->ch[0].band_type[idx] < NOISE_BT &&
                cpe->ch[1].band_type[idx] < NOISE_BT) {
#if USE_FIXED
                for (group = 0; group < ics->group_len[g]; group++) {
                    ac->fdsp->butterflies_fixed(ch0 + group * 128 + offsets[i],
                                                ch1 + group * 128 + offsets[i],
                                                offsets[i+1] - offsets[i]);
#else
                for (group = 0; group < ics->group_len[g]; group++) {
                    ac->fdsp->butterflies_float(ch0 + group * 128 + offsets[i],
                                               ch1 + group * 128 + offsets[i],
                                               offsets[i+1] - offsets[i]);
#endif /* USE_FIXED */
                }
            }
        }
        ch0 += ics->group_len[g] * 128;
        ch1 += ics->group_len[g] * 128;
    }
}

/**
 * intensity stereo decoding; reference: 4.6.8.2.3
 *
 * @param   ms_present  Indicates mid/side stereo presence. [0] mask is all 0s;
 *                      [1] mask is decoded from bitstream; [2] mask is all 1s;
 *                      [3] reserved for scalable AAC
 */
static void apply_intensity_stereo(AACDecContext *ac,
                                   ChannelElement *cpe, int ms_present)
{
    const IndividualChannelStream *ics = &cpe->ch[1].ics;
    SingleChannelElement         *sce1 = &cpe->ch[1];
    INTFLOAT *coef0 = cpe->ch[0].AAC_RENAME(coeffs), *coef1 = cpe->ch[1].AAC_RENAME(coeffs);
    const uint16_t *offsets = ics->swb_offset;
    int g, group, i, idx = 0;
    int c;
    INTFLOAT scale;
    for (g = 0; g < ics->num_window_groups; g++) {
        for (i = 0; i < ics->max_sfb;) {
            if (sce1->band_type[idx] == INTENSITY_BT ||
                sce1->band_type[idx] == INTENSITY_BT2) {
                const int bt_run_end = sce1->band_type_run_end[idx];
                for (; i < bt_run_end; i++, idx++) {
                    c = -1 + 2 * (sce1->band_type[idx] - 14);
                    if (ms_present)
                        c *= 1 - 2 * cpe->ms_mask[idx];
                    scale = c * sce1->AAC_RENAME(sf)[idx];
                    for (group = 0; group < ics->group_len[g]; group++)
#if USE_FIXED
                        ac->subband_scale(coef1 + group * 128 + offsets[i],
                                      coef0 + group * 128 + offsets[i],
                                      scale,
                                      23,
                                      offsets[i + 1] - offsets[i] ,ac->avctx);
#else
                        ac->fdsp->vector_fmul_scalar(coef1 + group * 128 + offsets[i],
                                                    coef0 + group * 128 + offsets[i],
                                                    scale,
                                                    offsets[i + 1] - offsets[i]);
#endif /* USE_FIXED */
                }
            } else {
                int bt_run_end = sce1->band_type_run_end[idx];
                idx += bt_run_end - i;
                i    = bt_run_end;
            }
        }
        coef0 += ics->group_len[g] * 128;
        coef1 += ics->group_len[g] * 128;
    }
}

/**
 * Decode Temporal Noise Shaping filter coefficients and apply all-pole filters; reference: 4.6.9.3.
 *
 * @param   decode  1 if tool is used normally, 0 if tool is used in LTP.
 * @param   coef    spectral coefficients
 */
static void apply_tns(void *_coef_param, TemporalNoiseShaping *tns,
                      IndividualChannelStream *ics, int decode)
{
    const int mmm = FFMIN(ics->tns_max_bands, ics->max_sfb);
    int w, filt, m, i;
    int bottom, top, order, start, end, size, inc;
    INTFLOAT lpc[TNS_MAX_ORDER];
    INTFLOAT tmp[TNS_MAX_ORDER+1];
    UINTFLOAT *coef = coef_param;

    INTFLOAT coef_param = coef_param;

    if(!mmm)
        return;

    for (w = 0; w < ics->num_windows; w++) {
        bottom = ics->num_swb;
        for (filt = 0; filt < tns->n_filt[w]; filt++) {
            top    = bottom;
            bottom = FFMAX(0, top - tns->length[w][filt]);
            order  = tns->order[w][filt];
            if (order == 0)
                continue;

            // tns_decode_coef
            compute_lpc_coefs(tns->AAC_RENAME(coef)[w][filt], order, lpc, 0, 0, 0);

            start = ics->swb_offset[FFMIN(bottom, mmm)];
            end   = ics->swb_offset[FFMIN(   top, mmm)];
            if ((size = end - start) <= 0)
                continue;
            if (tns->direction[w][filt]) {
                inc = -1;
                start = end - 1;
            } else {
                inc = 1;
            }
            start += w * 128;

            if (decode) {
                // ar filter
                for (m = 0; m < size; m++, start += inc)
                    for (i = 1; i <= FFMIN(m, order); i++)
                        coef[start] -= AAC_MUL26((INTFLOAT)coef[start - i * inc], lpc[i - 1]);
            } else {
                // ma filter
                for (m = 0; m < size; m++, start += inc) {
                    tmp[0] = coef[start];
                    for (i = 1; i <= FFMIN(m, order); i++)
                        coef[start] += AAC_MUL26(tmp[i], lpc[i - 1]);
                    for (i = order; i > 0; i--)
                        tmp[i] = tmp[i - 1];
                }
            }
        }
    }
}

/**
 *  Apply windowing and MDCT to obtain the spectral
 *  coefficient from the predicted sample by LTP.
 */
static void windowing_and_mdct_ltp(AACDecContext *ac, INTFLOAT *out,
                                   INTFLOAT *in, IndividualChannelStream *ics)
{
    const INTFLOAT *lwindow      = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_long_1024) : AAC_RENAME2(sine_1024);
    const INTFLOAT *swindow      = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);
    const INTFLOAT *lwindow_prev = ics->use_kb_window[1] ? AAC_RENAME2(aac_kbd_long_1024) : AAC_RENAME2(sine_1024);
    const INTFLOAT *swindow_prev = ics->use_kb_window[1] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);

    if (ics->window_sequence[0] != LONG_STOP_SEQUENCE) {
        ac->fdsp->vector_fmul(in, in, lwindow_prev, 1024);
    } else {
        memset(in, 0, 448 * sizeof(*in));
        ac->fdsp->vector_fmul(in + 448, in + 448, swindow_prev, 128);
    }
    if (ics->window_sequence[0] != LONG_START_SEQUENCE) {
        ac->fdsp->vector_fmul_reverse(in + 1024, in + 1024, lwindow, 1024);
    } else {
        ac->fdsp->vector_fmul_reverse(in + 1024 + 448, in + 1024 + 448, swindow, 128);
        memset(in + 1024 + 576, 0, 448 * sizeof(*in));
    }
    ac->mdct_ltp_fn(ac->mdct_ltp, out, in, sizeof(INTFLOAT));
}

/**
 * Apply the long term prediction
 */
static void apply_ltp(AACDecContext *ac, SingleChannelElement *sce)
{
    const LongTermPrediction *ltp = &sce->ics.ltp;
    const uint16_t *offsets = sce->ics.swb_offset;
    int i, sfb;

    if (sce->ics.window_sequence[0] != EIGHT_SHORT_SEQUENCE) {
        INTFLOAT *predTime = sce->AAC_RENAME(output);
        INTFLOAT *predFreq = ac->AAC_RENAME(buf_mdct);
        int16_t num_samples = 2048;

        if (ltp->lag < 1024)
            num_samples = ltp->lag + 1024;
        for (i = 0; i < num_samples; i++)
            predTime[i] = AAC_MUL30(sce->AAC_RENAME(ltp_state)[i + 2048 - ltp->lag], ltp->AAC_RENAME(coef));
        memset(&predTime[i], 0, (2048 - i) * sizeof(*predTime));

        windowing_and_mdct_ltp(ac, predFreq, predTime, &sce->ics);

        if (sce->tns.present)
            ac->dsp.apply_tns(predFreq, &sce->tns, &sce->ics, 0);

        for (sfb = 0; sfb < FFMIN(sce->ics.max_sfb, MAX_LTP_LONG_SFB); sfb++)
            if (ltp->used[sfb])
                for (i = offsets[sfb]; i < offsets[sfb + 1]; i++)
                    sce->AAC_RENAME(coeffs)[i] += (UINTFLOAT)predFreq[i];
    }
}

/**
 * Update the LTP buffer for next frame
 */
static void update_ltp(AACDecContext *ac, SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    INTFLOAT *saved     = sce->AAC_RENAME(saved);
    INTFLOAT *saved_ltp = sce->AAC_RENAME(coeffs);
    const INTFLOAT *lwindow = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_long_1024) : AAC_RENAME2(sine_1024);
    const INTFLOAT *swindow = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);
    int i;

    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        memcpy(saved_ltp,       saved, 512 * sizeof(*saved_ltp));
        memset(saved_ltp + 576, 0,     448 * sizeof(*saved_ltp));
        ac->fdsp->vector_fmul_reverse(saved_ltp + 448, ac->AAC_RENAME(buf_mdct) + 960,     &swindow[64],      64);

        for (i = 0; i < 64; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], swindow[63 - i]);
    } else if (ics->window_sequence[0] == LONG_START_SEQUENCE) {
        memcpy(saved_ltp,       ac->AAC_RENAME(buf_mdct) + 512, 448 * sizeof(*saved_ltp));
        memset(saved_ltp + 576, 0,                  448 * sizeof(*saved_ltp));
        ac->fdsp->vector_fmul_reverse(saved_ltp + 448, ac->AAC_RENAME(buf_mdct) + 960,     &swindow[64],      64);

        for (i = 0; i < 64; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], swindow[63 - i]);
    } else { // LONG_STOP or ONLY_LONG
        ac->fdsp->vector_fmul_reverse(saved_ltp, ac->AAC_RENAME(buf_mdct) + 512,     &lwindow[512],     512);

        for (i = 0; i < 512; i++)
            saved_ltp[i + 512] = AAC_MUL31(ac->AAC_RENAME(buf_mdct)[1023 - i], lwindow[511 - i]);
    }

    memcpy(sce->AAC_RENAME(ltp_state),      sce->AAC_RENAME(ltp_state)+1024,
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
    memcpy(sce->AAC_RENAME(ltp_state) + 1024, sce->AAC_RENAME(output),
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
    memcpy(sce->AAC_RENAME(ltp_state) + 2048, saved_ltp,
           1024 * sizeof(*sce->AAC_RENAME(ltp_state)));
}

/**
 * Conduct IMDCT and windowing.
 */
static void imdct_and_windowing(AACDecContext *ac, SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    INTFLOAT *in    = sce->AAC_RENAME(coeffs);
    INTFLOAT *out   = sce->AAC_RENAME(output);
    INTFLOAT *saved = sce->AAC_RENAME(saved);
    const INTFLOAT *swindow      = ics->use_kb_window[0] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);
    const INTFLOAT *lwindow_prev = ics->use_kb_window[1] ? AAC_RENAME2(aac_kbd_long_1024) : AAC_RENAME2(sine_1024);
    const INTFLOAT *swindow_prev = ics->use_kb_window[1] ? AAC_RENAME2(aac_kbd_short_128) : AAC_RENAME2(sine_128);
    INTFLOAT *buf  = ac->AAC_RENAME(buf_mdct);
    INTFLOAT *temp = ac->AAC_RENAME(temp);
    int i;

    // imdct
    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        for (i = 0; i < 1024; i += 128)
            ac->mdct128_fn(ac->mdct128, buf + i, in + i, sizeof(INTFLOAT));
    } else {
        ac->mdct1024_fn(ac->mdct1024, buf, in, sizeof(INTFLOAT));
    }

    /* window overlapping
     * NOTE: To simplify the overlapping code, all 'meaningless' short to long
     * and long to short transitions are considered to be short to short
     * transitions. This leaves just two cases (long to long and short to short)
     * with a little special sauce for EIGHT_SHORT_SEQUENCE.
     */
    if ((ics->window_sequence[1] == ONLY_LONG_SEQUENCE || ics->window_sequence[1] == LONG_STOP_SEQUENCE) &&
            (ics->window_sequence[0] == ONLY_LONG_SEQUENCE || ics->window_sequence[0] == LONG_START_SEQUENCE)) {
        ac->fdsp->vector_fmul_window(    out,               saved,            buf,         lwindow_prev, 512);
    } else {
        memcpy(                         out,               saved,            448 * sizeof(*out));

        if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
            ac->fdsp->vector_fmul_window(out + 448 + 0*128, saved + 448,      buf + 0*128, swindow_prev, 64);
            ac->fdsp->vector_fmul_window(out + 448 + 1*128, buf + 0*128 + 64, buf + 1*128, swindow,      64);
            ac->fdsp->vector_fmul_window(out + 448 + 2*128, buf + 1*128 + 64, buf + 2*128, swindow,      64);
            ac->fdsp->vector_fmul_window(out + 448 + 3*128, buf + 2*128 + 64, buf + 3*128, swindow,      64);
            ac->fdsp->vector_fmul_window(temp,              buf + 3*128 + 64, buf + 4*128, swindow,      64);
            memcpy(                     out + 448 + 4*128, temp, 64 * sizeof(*out));
        } else {
            ac->fdsp->vector_fmul_window(out + 448,         saved + 448,      buf,         swindow_prev, 64);
            memcpy(                     out + 576,         buf + 64,         448 * sizeof(*out));
        }
    }

    // buffer update
    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        memcpy(                     saved,       temp + 64,         64 * sizeof(*saved));
        ac->fdsp->vector_fmul_window(saved + 64,  buf + 4*128 + 64, buf + 5*128, swindow, 64);
        ac->fdsp->vector_fmul_window(saved + 192, buf + 5*128 + 64, buf + 6*128, swindow, 64);
        ac->fdsp->vector_fmul_window(saved + 320, buf + 6*128 + 64, buf + 7*128, swindow, 64);
        memcpy(                     saved + 448, buf + 7*128 + 64,  64 * sizeof(*saved));
    } else if (ics->window_sequence[0] == LONG_START_SEQUENCE) {
        memcpy(                     saved,       buf + 512,        448 * sizeof(*saved));
        memcpy(                     saved + 448, buf + 7*128 + 64,  64 * sizeof(*saved));
    } else { // LONG_STOP or ONLY_LONG
        memcpy(                     saved,       buf + 512,        512 * sizeof(*saved));
    }
}

/**
 * Conduct IMDCT and windowing.
 */
static void imdct_and_windowing_960(AACDecContext *ac, SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    INTFLOAT *in    = sce->AAC_RENAME(coeffs);
    INTFLOAT *out   = sce->AAC_RENAME(output);
    INTFLOAT *saved = sce->AAC_RENAME(saved);
    const INTFLOAT *swindow      = ics->use_kb_window[0] ? AAC_RENAME(aac_kbd_short_120) : AAC_RENAME(sine_120);
    const INTFLOAT *lwindow_prev = ics->use_kb_window[1] ? AAC_RENAME(aac_kbd_long_960) : AAC_RENAME(sine_960);
    const INTFLOAT *swindow_prev = ics->use_kb_window[1] ? AAC_RENAME(aac_kbd_short_120) : AAC_RENAME(sine_120);
    INTFLOAT *buf  = ac->AAC_RENAME(buf_mdct);
    INTFLOAT *temp = ac->AAC_RENAME(temp);
    int i;

    // imdct
    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        for (i = 0; i < 8; i++)
            ac->mdct120_fn(ac->mdct120, buf + i * 120, in + i * 128, sizeof(INTFLOAT));
    } else {
        ac->mdct960_fn(ac->mdct960, buf, in, sizeof(INTFLOAT));
    }

    /* window overlapping
     * NOTE: To simplify the overlapping code, all 'meaningless' short to long
     * and long to short transitions are considered to be short to short
     * transitions. This leaves just two cases (long to long and short to short)
     * with a little special sauce for EIGHT_SHORT_SEQUENCE.
     */

    if ((ics->window_sequence[1] == ONLY_LONG_SEQUENCE || ics->window_sequence[1] == LONG_STOP_SEQUENCE) &&
        (ics->window_sequence[0] == ONLY_LONG_SEQUENCE || ics->window_sequence[0] == LONG_START_SEQUENCE)) {
        ac->fdsp->vector_fmul_window(    out,               saved,            buf,         lwindow_prev, 480);
    } else {
        memcpy(                          out,               saved,            420 * sizeof(*out));

        if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
            ac->fdsp->vector_fmul_window(out + 420 + 0*120, saved + 420,      buf + 0*120, swindow_prev, 60);
            ac->fdsp->vector_fmul_window(out + 420 + 1*120, buf + 0*120 + 60, buf + 1*120, swindow,      60);
            ac->fdsp->vector_fmul_window(out + 420 + 2*120, buf + 1*120 + 60, buf + 2*120, swindow,      60);
            ac->fdsp->vector_fmul_window(out + 420 + 3*120, buf + 2*120 + 60, buf + 3*120, swindow,      60);
            ac->fdsp->vector_fmul_window(temp,              buf + 3*120 + 60, buf + 4*120, swindow,      60);
            memcpy(                      out + 420 + 4*120, temp, 60 * sizeof(*out));
        } else {
            ac->fdsp->vector_fmul_window(out + 420,         saved + 420,      buf,         swindow_prev, 60);
            memcpy(                      out + 540,         buf + 60,         420 * sizeof(*out));
        }
    }

    // buffer update
    if (ics->window_sequence[0] == EIGHT_SHORT_SEQUENCE) {
        memcpy(                      saved,       temp + 60,         60 * sizeof(*saved));
        ac->fdsp->vector_fmul_window(saved + 60,  buf + 4*120 + 60, buf + 5*120, swindow, 60);
        ac->fdsp->vector_fmul_window(saved + 180, buf + 5*120 + 60, buf + 6*120, swindow, 60);
        ac->fdsp->vector_fmul_window(saved + 300, buf + 6*120 + 60, buf + 7*120, swindow, 60);
        memcpy(                      saved + 420, buf + 7*120 + 60,  60 * sizeof(*saved));
    } else if (ics->window_sequence[0] == LONG_START_SEQUENCE) {
        memcpy(                      saved,       buf + 480,        420 * sizeof(*saved));
        memcpy(                      saved + 420, buf + 7*120 + 60,  60 * sizeof(*saved));
    } else { // LONG_STOP or ONLY_LONG
        memcpy(                      saved,       buf + 480,        480 * sizeof(*saved));
    }
}
static void imdct_and_windowing_ld(AACDecContext *ac, SingleChannelElement *sce)
{
    IndividualChannelStream *ics = &sce->ics;
    INTFLOAT *in    = sce->AAC_RENAME(coeffs);
    INTFLOAT *out   = sce->AAC_RENAME(output);
    INTFLOAT *saved = sce->AAC_RENAME(saved);
    INTFLOAT *buf   = ac->AAC_RENAME(buf_mdct);

    // imdct
    ac->mdct512_fn(ac->mdct512, buf, in, sizeof(INTFLOAT));

    // window overlapping
    if (ics->use_kb_window[1]) {
        // AAC LD uses a low overlap sine window instead of a KBD window
        memcpy(out, saved, 192 * sizeof(*out));
        ac->fdsp->vector_fmul_window(out + 192, saved + 192, buf, AAC_RENAME2(sine_128), 64);
        memcpy(                     out + 320, buf + 64, 192 * sizeof(*out));
    } else {
        ac->fdsp->vector_fmul_window(out, saved, buf, AAC_RENAME2(sine_512), 256);
    }

    // buffer update
    memcpy(saved, buf + 256, 256 * sizeof(*saved));
}

static void imdct_and_windowing_eld(AACDecContext *ac, SingleChannelElement *sce)
{
    UINTFLOAT *in   = sce->AAC_RENAME(coeffs);
    INTFLOAT *out   = sce->AAC_RENAME(output);
    INTFLOAT *saved = sce->AAC_RENAME(saved);
    INTFLOAT *buf   = ac->AAC_RENAME(buf_mdct);
    int i;
    const int n  = ac->oc[1].m4ac.frame_length_short ? 480 : 512;
    const int n2 = n >> 1;
    const int n4 = n >> 2;
    const INTFLOAT *const window = n == 480 ? AAC_RENAME(ff_aac_eld_window_480) :
                                           AAC_RENAME(ff_aac_eld_window_512);

    // Inverse transform, mapped to the conventional IMDCT by
    // Chivukula, R.K.; Reznik, Y.A.; Devarajan, V.,
    // "Efficient algorithms for MPEG-4 AAC-ELD, AAC-LD and AAC-LC filterbanks,"
    // International Conference on Audio, Language and Image Processing, ICALIP 2008.
    // URL: http://ieeexplore.ieee.org/stamp/stamp.jsp?tp=&arnumber=4590245&isnumber=4589950
    for (i = 0; i < n2; i+=2) {
        INTFLOAT temp;
        temp =  in[i    ]; in[i    ] = -in[n - 1 - i]; in[n - 1 - i] = temp;
        temp = -in[i + 1]; in[i + 1] =  in[n - 2 - i]; in[n - 2 - i] = temp;
    }

    if (n == 480)
        ac->mdct480_fn(ac->mdct480, buf, in, sizeof(INTFLOAT));
    else
        ac->mdct512_fn(ac->mdct512, buf, in, sizeof(INTFLOAT));

    for (i = 0; i < n; i+=2) {
        buf[i + 0] = -(UINTFLOAT)(USE_FIXED + 1)*buf[i + 0];
        buf[i + 1] =  (UINTFLOAT)(USE_FIXED + 1)*buf[i + 1];
    }
    // Like with the regular IMDCT at this point we still have the middle half
    // of a transform but with even symmetry on the left and odd symmetry on
    // the right

    // window overlapping
    // The spec says to use samples [0..511] but the reference decoder uses
    // samples [128..639].
    for (i = n4; i < n2; i ++) {
        out[i - n4] = AAC_MUL31(   buf[    n2 - 1 - i] , window[i       - n4]) +
                      AAC_MUL31( saved[        i + n2] , window[i +   n - n4]) +
                      AAC_MUL31(-saved[n + n2 - 1 - i] , window[i + 2*n - n4]) +
                      AAC_MUL31(-saved[  2*n + n2 + i] , window[i + 3*n - n4]);
    }
    for (i = 0; i < n2; i ++) {
        out[n4 + i] = AAC_MUL31(   buf[              i] , window[i + n2       - n4]) +
                      AAC_MUL31(-saved[      n - 1 - i] , window[i + n2 +   n - n4]) +
                      AAC_MUL31(-saved[          n + i] , window[i + n2 + 2*n - n4]) +
                      AAC_MUL31( saved[2*n + n - 1 - i] , window[i + n2 + 3*n - n4]);
    }
    for (i = 0; i < n4; i ++) {
        out[n2 + n4 + i] = AAC_MUL31(   buf[    i + n2] , window[i +   n - n4]) +
                           AAC_MUL31(-saved[n2 - 1 - i] , window[i + 2*n - n4]) +
                           AAC_MUL31(-saved[n + n2 + i] , window[i + 3*n - n4]);
    }

    // buffer update
    memmove(saved + n, saved, 2 * n * sizeof(*saved));
    memcpy( saved,       buf,     n * sizeof(*saved));
}

const AACDspType AAC_RENAME(aac_dsp) = {
    .decode_cce = decode_cce,
    .decode_spectrum_and_dequant = decode_spectrum_and_dequant,
    .apply_mid_side_stereo = apply_mid_side_stereo,
    .apply_intensity_stereo = apply_intensity_stereo,
    .apply_tns = apply_tns,
    .apply_ltp = apply_ltp,
    .update_ltp = update_ltp,
    .imdct_and_windowing = imdct_and_windowing,
    .imdct_and_windowing_960 = imdct_and_windowing_960,
    .imdct_and_windowing_ld = imdct_and_windowing_ld,
    .imdct_and_windowing_eld = imdct_and_windowing_eld,
};
