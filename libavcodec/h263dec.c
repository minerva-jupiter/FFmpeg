/*
 * H.263 decoder
 * Copyright (c) 2001 Fabrice Bellard
 * Copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
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

/**
 * @file
 * H.263 decoder.
 */

#define UNCHECKED_BITSTREAM_READER 1

#include "config_components.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "error_resilience.h"
#include "flvdec.h"
#include "h263.h"
#include "h263dec.h"
#include "hwaccel_internal.h"
#include "hwconfig.h"
#include "mpeg_er.h"
#include "mpeg4video.h"
#include "mpeg4videodec.h"
#include "mpegvideo.h"
#include "mpegvideodata.h"
#include "mpegvideodec.h"
#include "mpegvideo_unquantize.h"
#include "msmpeg4dec.h"
#include "thread.h"
#include "wmv2dec.h"

static const enum AVPixelFormat h263_hwaccel_pixfmt_list_420[] = {
#if CONFIG_H263_VAAPI_HWACCEL || CONFIG_MPEG4_VAAPI_HWACCEL
    AV_PIX_FMT_VAAPI,
#endif
#if CONFIG_MPEG4_NVDEC_HWACCEL
    AV_PIX_FMT_CUDA,
#endif
#if CONFIG_MPEG4_VDPAU_HWACCEL
    AV_PIX_FMT_VDPAU,
#endif
#if CONFIG_H263_VIDEOTOOLBOX_HWACCEL || CONFIG_MPEG4_VIDEOTOOLBOX_HWACCEL
    AV_PIX_FMT_VIDEOTOOLBOX,
#endif
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE
};

static enum AVPixelFormat h263_get_format(AVCodecContext *avctx)
{
    /* MPEG-4 Studio Profile only, not supported by hardware */
    if (avctx->bits_per_raw_sample > 8) {
        av_assert1(((MpegEncContext *)avctx->priv_data)->studio_profile);
        return avctx->pix_fmt;
    }

    if (CONFIG_GRAY && (avctx->flags & AV_CODEC_FLAG_GRAY)) {
        if (avctx->color_range == AVCOL_RANGE_UNSPECIFIED)
            avctx->color_range = AVCOL_RANGE_MPEG;
        return AV_PIX_FMT_GRAY8;
    }

    if (avctx->codec_id == AV_CODEC_ID_H263  ||
        avctx->codec_id == AV_CODEC_ID_H263P ||
        avctx->codec_id == AV_CODEC_ID_MPEG4)
        return avctx->pix_fmt = ff_get_format(avctx, h263_hwaccel_pixfmt_list_420);

    return AV_PIX_FMT_YUV420P;
}

av_cold int ff_h263_decode_init(AVCodecContext *avctx)
{
    MpegEncContext *s = avctx->priv_data;
    MPVUnquantDSPContext unquant_dsp_ctx;
    int ret;

    s->out_format      = FMT_H263;

    // set defaults
    ret = ff_mpv_decode_init(s, avctx);
    if (ret < 0)
        return ret;

    s->decode_mb       = ff_h263_decode_mb;
    s->low_delay       = 1;

    s->y_dc_scale_table =
    s->c_dc_scale_table = ff_mpeg1_dc_scale_table;

    ff_mpv_unquantize_init(&unquant_dsp_ctx,
                           avctx->flags & AV_CODEC_FLAG_BITEXACT, 0);
    // dct_unquantize defaults for H.263;
    // they might change on a per-frame basis for MPEG-4;
    // dct_unquantize_inter will be unset for MSMPEG4 codecs later.
    s->dct_unquantize_intra = unquant_dsp_ctx.dct_unquantize_h263_intra;
    s->dct_unquantize_inter = unquant_dsp_ctx.dct_unquantize_h263_inter;

    /* select sub codec */
    switch (avctx->codec->id) {
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
        avctx->chroma_sample_location = AVCHROMA_LOC_CENTER;
        break;
    case AV_CODEC_ID_MPEG4:
        break;
    case AV_CODEC_ID_MSMPEG4V1:
        s->h263_pred       = 1;
        s->msmpeg4_version = MSMP4_V1;
        break;
    case AV_CODEC_ID_MSMPEG4V2:
        s->h263_pred       = 1;
        s->msmpeg4_version = MSMP4_V2;
        break;
    case AV_CODEC_ID_MSMPEG4V3:
        s->h263_pred       = 1;
        s->msmpeg4_version = MSMP4_V3;
        break;
    case AV_CODEC_ID_WMV1:
        s->h263_pred       = 1;
        s->msmpeg4_version = MSMP4_WMV1;
        break;
    case AV_CODEC_ID_WMV2:
        s->h263_pred       = 1;
        s->msmpeg4_version = MSMP4_WMV2;
        break;
    case AV_CODEC_ID_H263I:
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
        break;
    case AV_CODEC_ID_FLV1:
        s->h263_flv = 1;
        break;
    default:
        av_unreachable("Switch contains a case for every codec using ff_h263_decode_init()");
    }

    if (avctx->codec_tag == AV_RL32("L263") || avctx->codec_tag == AV_RL32("S263"))
        if (avctx->extradata_size == 56 && avctx->extradata[0] == 1)
            s->ehc_mode = 1;

    /* for H.263, we allocate the images after having read the header */
    if (avctx->codec->id != AV_CODEC_ID_H263 &&
        avctx->codec->id != AV_CODEC_ID_H263P &&
        avctx->codec->id != AV_CODEC_ID_MPEG4) {
        avctx->pix_fmt = h263_get_format(avctx);
        if ((ret = ff_mpv_common_init(s)) < 0)
            return ret;
    }

    ff_h263dsp_init(&s->h263dsp);
    ff_h263_decode_init_vlc();

    return 0;
}

static void report_decode_progress(MpegEncContext *s)
{
    if (s->pict_type != AV_PICTURE_TYPE_B && !s->partitioned_frame && !s->er.error_occurred)
        ff_thread_progress_report(&s->cur_pic.ptr->progress, s->mb_y);
}

static int decode_slice(MpegEncContext *s)
{
    const int part_mask = s->partitioned_frame
                          ? (ER_AC_END | ER_AC_ERROR) : 0x7F;
    const int mb_size   = 16 >> s->avctx->lowres;
    int ret;

    s->last_resync_gb   = s->gb;
    s->first_slice_line = 1;
    s->resync_mb_x      = s->mb_x;
    s->resync_mb_y      = s->mb_y;

    ff_set_qscale(s, s->qscale);

    if (s->studio_profile) {
        if ((ret = ff_mpeg4_decode_studio_slice_header(s->avctx->priv_data)) < 0)
            return ret;
    }

    if (s->avctx->hwaccel) {
        const uint8_t *start = s->gb.buffer + get_bits_count(&s->gb) / 8;
        ret = FF_HW_CALL(s->avctx, decode_slice, start, s->gb.buffer_end - start);
        // ensure we exit decode loop
        s->mb_y = s->mb_height;
        return ret;
    }

    if (s->partitioned_frame) {
        const int qscale = s->qscale;

        if (CONFIG_MPEG4_DECODER && s->codec_id == AV_CODEC_ID_MPEG4)
            if ((ret = ff_mpeg4_decode_partitions(s->avctx->priv_data)) < 0)
                return ret;

        /* restore variables which were modified */
        s->first_slice_line = 1;
        s->mb_x             = s->resync_mb_x;
        s->mb_y             = s->resync_mb_y;
        ff_set_qscale(s, qscale);
    }

    for (; s->mb_y < s->mb_height; s->mb_y++) {
        /* per-row end of slice checks */
        if (s->msmpeg4_version != MSMP4_UNUSED) {
            if (s->resync_mb_y + s->slice_height == s->mb_y) {
                ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                                s->mb_x - 1, s->mb_y, ER_MB_END);

                return 0;
            }
        }

        if (s->msmpeg4_version == MSMP4_V1) {
            s->last_dc[0] =
            s->last_dc[1] =
            s->last_dc[2] = 128;
        }

        ff_init_block_index(s);
        for (; s->mb_x < s->mb_width; s->mb_x++) {
            int ret;

            ff_update_block_index(s, s->avctx->bits_per_raw_sample,
                                  s->avctx->lowres, s->chroma_x_shift);

            if (s->resync_mb_x == s->mb_x && s->resync_mb_y + 1 == s->mb_y)
                s->first_slice_line = 0;

            /* DCT & quantize */

            s->mv_dir  = MV_DIR_FORWARD;
            s->mv_type = MV_TYPE_16X16;
            ff_dlog(s->avctx, "%d %06X\n",
                    get_bits_count(&s->gb), show_bits(&s->gb, 24));

            ff_tlog(NULL, "Decoding MB at %dx%d\n", s->mb_x, s->mb_y);
            ret = s->decode_mb(s, s->block);

            if (s->h263_pred || s->h263_aic) {
                int mb_xy = s->mb_y * s->mb_stride + s->mb_x;
                if (!s->mb_intra) {
                    ff_h263_clean_intra_table_entries(s, mb_xy);
                } else
                    s->mbintra_table[mb_xy] = 1;
            }

            if (s->pict_type != AV_PICTURE_TYPE_B)
                ff_h263_update_motion_val(s);

            if (ret < 0) {
                const int xy = s->mb_x + s->mb_y * s->mb_stride;
                if (ret == SLICE_END) {
                    ff_mpv_reconstruct_mb(s, s->block);
                    if (s->loop_filter)
                        ff_h263_loop_filter(s);

                    ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                                    s->mb_x, s->mb_y, ER_MB_END & part_mask);

                    s->padding_bug_score--;

                    if (++s->mb_x >= s->mb_width) {
                        s->mb_x = 0;
                        report_decode_progress(s);
                        ff_mpeg_draw_horiz_band(s, s->mb_y * mb_size, mb_size);
                        s->mb_y++;
                    }
                    return 0;
                } else if (ret == SLICE_NOEND) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Slice mismatch at MB: %d\n", xy);
                    ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                                    s->mb_x + 1, s->mb_y,
                                    ER_MB_END & part_mask);
                    return AVERROR_INVALIDDATA;
                }
                av_log(s->avctx, AV_LOG_ERROR, "Error at MB: %d\n", xy);
                ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                                s->mb_x, s->mb_y, ER_MB_ERROR & part_mask);

                if ((s->avctx->err_recognition & AV_EF_IGNORE_ERR) && get_bits_left(&s->gb) > 0)
                    continue;
                return AVERROR_INVALIDDATA;
            }

            ff_mpv_reconstruct_mb(s, s->block);
            if (s->loop_filter)
                ff_h263_loop_filter(s);
        }

        report_decode_progress(s);
        ff_mpeg_draw_horiz_band(s, s->mb_y * mb_size, mb_size);

        s->mb_x = 0;
    }

    av_assert1(s->mb_x == 0 && s->mb_y == s->mb_height);

    // Detect incorrect padding with wrong stuffing codes used by NEC N-02B
    if (s->codec_id == AV_CODEC_ID_MPEG4         &&
        (s->workaround_bugs & FF_BUG_AUTODETECT) &&
        get_bits_left(&s->gb) >= 48              &&
        show_bits(&s->gb, 24) == 0x4010          &&
        !s->data_partitioning)
        s->padding_bug_score += 32;

    /* try to detect the padding bug */
    if (s->codec_id == AV_CODEC_ID_MPEG4         &&
        (s->workaround_bugs & FF_BUG_AUTODETECT) &&
        get_bits_left(&s->gb) >= 0               &&
        get_bits_left(&s->gb) < 137              &&
        !s->data_partitioning) {
        const int bits_count = get_bits_count(&s->gb);
        const int bits_left  = s->gb.size_in_bits - bits_count;

        if (bits_left == 0) {
            s->padding_bug_score += 16;
        } else if (bits_left != 1) {
            int v = show_bits(&s->gb, 8);
            v |= 0x7F >> (7 - (bits_count & 7));

            if (v == 0x7F && bits_left <= 8)
                s->padding_bug_score--;
            else if (v == 0x7F && ((get_bits_count(&s->gb) + 8) & 8) &&
                     bits_left <= 16)
                s->padding_bug_score += 4;
            else
                s->padding_bug_score++;
        }
    }

    if (s->codec_id == AV_CODEC_ID_H263          &&
        (s->workaround_bugs & FF_BUG_AUTODETECT) &&
        get_bits_left(&s->gb) >= 8               &&
        get_bits_left(&s->gb) < 300              &&
        s->pict_type == AV_PICTURE_TYPE_I        &&
        show_bits(&s->gb, 8) == 0                &&
        !s->data_partitioning) {

        s->padding_bug_score += 32;
    }

    if (s->codec_id == AV_CODEC_ID_H263          &&
        (s->workaround_bugs & FF_BUG_AUTODETECT) &&
        get_bits_left(&s->gb) >= 64              &&
        AV_RB64(s->gb.buffer_end - 8) == 0xCDCDCDCDFC7F0000) {

        s->padding_bug_score += 32;
    }

    if (s->workaround_bugs & FF_BUG_AUTODETECT) {
        if (
            (s->padding_bug_score > -2 && !s->data_partitioning))
            s->workaround_bugs |= FF_BUG_NO_PADDING;
        else
            s->workaround_bugs &= ~FF_BUG_NO_PADDING;
    }

    // handle formats which don't have unique end markers
    if (s->msmpeg4_version != MSMP4_UNUSED || (s->workaround_bugs & FF_BUG_NO_PADDING)) { // FIXME perhaps solve this more cleanly
        int left      = get_bits_left(&s->gb);
        int max_extra = 7;

        /* no markers in M$ crap */
        if (s->msmpeg4_version != MSMP4_UNUSED && s->pict_type == AV_PICTURE_TYPE_I)
            max_extra += 17;

        /* buggy padding but the frame should still end approximately at
         * the bitstream end */
        if ((s->workaround_bugs & FF_BUG_NO_PADDING) &&
            (s->avctx->err_recognition & (AV_EF_BUFFER|AV_EF_AGGRESSIVE)))
            max_extra += 48;
        else if ((s->workaround_bugs & FF_BUG_NO_PADDING))
            max_extra += 256 * 256 * 256 * 64;

        if (left > max_extra)
            av_log(s->avctx, AV_LOG_ERROR,
                   "discarding %d junk bits at end, next would be %X\n",
                   left, show_bits(&s->gb, 24));
        else if (left < 0)
            av_log(s->avctx, AV_LOG_ERROR, "overreading %d bits\n", -left);
        else
            ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y,
                            s->mb_x - 1, s->mb_y, ER_MB_END);

        return 0;
    }

    av_log(s->avctx, AV_LOG_ERROR,
           "slice end not reached but screenspace end (%d left %06X, score= %d)\n",
           get_bits_left(&s->gb), show_bits(&s->gb, 24), s->padding_bug_score);

    ff_er_add_slice(&s->er, s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y,
                    ER_MB_END & part_mask);

    return AVERROR_INVALIDDATA;
}

int ff_h263_decode_frame(AVCodecContext *avctx, AVFrame *pict,
                         int *got_frame, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    MpegEncContext *s  = avctx->priv_data;
    int ret;
    int slice_ret = 0;
    int bak_width, bak_height;

    /* no supplementary picture */
    if (buf_size == 0) {
        /* special case for last picture */
        if ((!s->low_delay || s->skipped_last_frame) && s->next_pic.ptr) {
            if ((ret = av_frame_ref(pict, s->next_pic.ptr->f)) < 0)
                return ret;
            if (s->skipped_last_frame) {
                /* If the stream ended with an NVOP, we output the last frame
                 * in display order, but with the props from the last input
                 * packet so that the stream's end time is correct. */
                ret = ff_decode_frame_props(avctx, pict);
                if (ret < 0)
                    return ret;
            }

            ff_mpv_unref_picture(&s->next_pic);

            *got_frame = 1;
        }

        return 0;
    }

    // s->gb might be overridden in ff_mpeg4_decode_picture_header() below.
    ret = init_get_bits8(&s->gb, buf, buf_size);
    if (ret < 0)
        return ret;

    bak_width  = s->width;
    bak_height = s->height;

    /* let's go :-) */
    if (CONFIG_WMV2_DECODER && s->msmpeg4_version == MSMP4_WMV2) {
        ret = ff_wmv2_decode_picture_header(s);
#if CONFIG_MSMPEG4DEC
    } else if (s->msmpeg4_version != MSMP4_UNUSED) {
        ret = ff_msmpeg4_decode_picture_header(s);
#endif
    } else if (CONFIG_MPEG4_DECODER && avctx->codec_id == AV_CODEC_ID_MPEG4) {
        ret = ff_mpeg4_decode_picture_header(s);
    } else if (CONFIG_H263I_DECODER && s->codec_id == AV_CODEC_ID_H263I) {
        ret = ff_intel_h263_decode_picture_header(s);
    } else if (CONFIG_FLV_DECODER && s->h263_flv) {
        ret = ff_flv_decode_picture_header(s);
    } else {
        ret = ff_h263_decode_picture_header(s);
    }

    if (ret < 0 || ret == FRAME_SKIPPED) {
        if (   s->width  != bak_width
            || s->height != bak_height) {
                av_log(s->avctx, AV_LOG_WARNING, "Reverting picture dimensions change due to header decoding failure\n");
                s->width = bak_width;
                s->height= bak_height;

        }
    }
    if (ret == FRAME_SKIPPED)
        return buf_size;

    /* skip if the header was thrashed */
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "header damaged\n");
        return ret;
    }

    if (!s->context_initialized) {
        avctx->pix_fmt = h263_get_format(avctx);
        if ((ret = ff_mpv_common_init(s)) < 0)
            return ret;
    }

    avctx->has_b_frames = !s->low_delay;

    if (CONFIG_MPEG4_DECODER && avctx->codec_id == AV_CODEC_ID_MPEG4) {
        if (s->pict_type != AV_PICTURE_TYPE_B && s->mb_num/2 > get_bits_left(&s->gb))
            return AVERROR_INVALIDDATA;
        ff_mpeg4_workaround_bugs(avctx);
        if (s->studio_profile != (s->idsp.idct == NULL))
            ff_mpv_idct_init(s);
    }

    /* After H.263 & MPEG-4 header decode we have the height, width,
     * and other parameters. So then we could init the picture.
     * FIXME: By the way H.263 decoder is evolving it should have
     * an H263EncContext */
    if (s->width  != avctx->coded_width  ||
        s->height != avctx->coded_height ||
        s->context_reinit) {
        /* H.263 could change picture size any time */
        s->context_reinit = 0;

        ret = ff_set_dimensions(avctx, s->width, s->height);
        if (ret < 0)
            return ret;

        ff_set_sar(avctx, avctx->sample_aspect_ratio);

        if ((ret = ff_mpv_common_frame_size_change(s)))
            return ret;

        if (avctx->pix_fmt != h263_get_format(avctx)) {
            av_log(avctx, AV_LOG_ERROR, "format change not supported\n");
            avctx->pix_fmt = AV_PIX_FMT_NONE;
            return AVERROR_UNKNOWN;
        }
    }

    /* skip B-frames if we don't have reference frames */
    if (!s->last_pic.ptr &&
        (s->pict_type == AV_PICTURE_TYPE_B || s->droppable))
        return buf_size;
    if ((avctx->skip_frame >= AVDISCARD_NONREF &&
         s->pict_type == AV_PICTURE_TYPE_B)    ||
        (avctx->skip_frame >= AVDISCARD_NONKEY &&
         s->pict_type != AV_PICTURE_TYPE_I)    ||
        avctx->skip_frame >= AVDISCARD_ALL)
        return buf_size;

    if ((ret = ff_mpv_frame_start(s, avctx)) < 0)
        return ret;

    if (!s->divx_packed)
        ff_thread_finish_setup(avctx);

    if (avctx->hwaccel) {
        ret = FF_HW_CALL(avctx, start_frame, NULL,
                         s->gb.buffer, s->gb.buffer_end - s->gb.buffer);
        if (ret < 0 )
            return ret;
    }

    ff_mpeg_er_frame_start(s);

    /* the second part of the wmv2 header contains the MB skip bits which
     * are stored in current_picture->mb_type which is not available before
     * ff_mpv_frame_start() */
#if CONFIG_WMV2_DECODER
    if (s->msmpeg4_version == MSMP4_WMV2) {
        ret = ff_wmv2_decode_secondary_picture_header(s);
        if (ret < 0)
            return ret;
        if (ret == 1)
            goto frame_end;
    }
#endif

    /* decode each macroblock */
    s->mb_x = 0;
    s->mb_y = 0;

    slice_ret = decode_slice(s);
    while (s->mb_y < s->mb_height) {
        if (s->msmpeg4_version != MSMP4_UNUSED) {
            if (s->slice_height == 0 || s->mb_x != 0 || slice_ret < 0 ||
                (s->mb_y % s->slice_height) != 0 || get_bits_left(&s->gb) < 0)
                break;
        } else {
            int prev_x = s->mb_x, prev_y = s->mb_y;
            if (ff_h263_resync(s) < 0)
                break;
            if (prev_y * s->mb_width + prev_x < s->mb_y * s->mb_width + s->mb_x)
                s->er.error_occurred = 1;
        }

        if (s->msmpeg4_version < MSMP4_WMV1 && s->h263_pred)
            ff_mpeg4_clean_buffers(s);

        if (decode_slice(s) < 0)
            slice_ret = AVERROR_INVALIDDATA;
    }

    if (s->msmpeg4_version != MSMP4_UNUSED && s->msmpeg4_version < MSMP4_WMV1 &&
        s->pict_type == AV_PICTURE_TYPE_I)
        if (!CONFIG_MSMPEG4DEC ||
            ff_msmpeg4_decode_ext_header(s, buf_size) < 0)
            s->er.error_status_table[s->mb_num - 1] = ER_MB_ERROR;

frame_end:
    if (!s->studio_profile)
        ff_er_frame_end(&s->er, NULL);

    if (avctx->hwaccel) {
        ret = FF_HW_SIMPLE_CALL(avctx, end_frame);
        if (ret < 0)
            return ret;
    }

    ff_mpv_frame_end(s);

    if (CONFIG_MPEG4_DECODER && avctx->codec_id == AV_CODEC_ID_MPEG4)
        ff_mpeg4_frame_end(avctx, avpkt);

    av_assert1(s->pict_type == s->cur_pic.ptr->f->pict_type);
    if (s->pict_type == AV_PICTURE_TYPE_B || s->low_delay) {
        if ((ret = av_frame_ref(pict, s->cur_pic.ptr->f)) < 0)
            return ret;
        ff_print_debug_info(s, s->cur_pic.ptr, pict);
        ff_mpv_export_qp_table(s, pict, s->cur_pic.ptr, FF_MPV_QSCALE_TYPE_MPEG1);
    } else if (s->last_pic.ptr) {
        if ((ret = av_frame_ref(pict, s->last_pic.ptr->f)) < 0)
            return ret;
        ff_print_debug_info(s, s->last_pic.ptr, pict);
        ff_mpv_export_qp_table(s, pict, s->last_pic.ptr, FF_MPV_QSCALE_TYPE_MPEG1);
    }

    if (s->last_pic.ptr || s->low_delay) {
        if (   pict->format == AV_PIX_FMT_YUV420P
            && (s->codec_tag == AV_RL32("GEOV") || s->codec_tag == AV_RL32("GEOX"))) {
            for (int p = 0; p < 3; p++) {
                int h = AV_CEIL_RSHIFT(pict->height, !!p);

                pict->data[p]     += (h - 1) * pict->linesize[p];
                pict->linesize[p] *= -1;
            }
        }
        *got_frame = 1;
    }

    if (slice_ret < 0 && (avctx->err_recognition & AV_EF_EXPLODE))
        return slice_ret;
    else
        return buf_size;
}

static const AVCodecHWConfigInternal *const h263_hw_config_list[] = {
#if CONFIG_H263_VAAPI_HWACCEL
    HWACCEL_VAAPI(h263),
#endif
#if CONFIG_MPEG4_NVDEC_HWACCEL
    HWACCEL_NVDEC(mpeg4),
#endif
#if CONFIG_MPEG4_VDPAU_HWACCEL
    HWACCEL_VDPAU(mpeg4),
#endif
#if CONFIG_H263_VIDEOTOOLBOX_HWACCEL
    HWACCEL_VIDEOTOOLBOX(h263),
#endif
    NULL
};

const FFCodec ff_h263_decoder = {
    .p.name         = "h263",
    CODEC_LONG_NAME("H.263 / H.263-1996, H.263+ / H.263-1998 / H.263 version 2"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H263,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_h263_decode_init,
    FF_CODEC_DECODE_CB(ff_h263_decode_frame),
    .close          = ff_mpv_decode_close,
    .p.capabilities = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP |
                      FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
    .flush          = ff_mpeg_flush,
    .p.max_lowres   = 3,
    .hw_configs     = h263_hw_config_list,
};

const FFCodec ff_h263p_decoder = {
    .p.name         = "h263p",
    CODEC_LONG_NAME("H.263 / H.263-1996, H.263+ / H.263-1998 / H.263 version 2"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H263P,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_h263_decode_init,
    FF_CODEC_DECODE_CB(ff_h263_decode_frame),
    .close          = ff_mpv_decode_close,
    .p.capabilities = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP |
                      FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
    .flush          = ff_mpeg_flush,
    .p.max_lowres   = 3,
    .hw_configs     = h263_hw_config_list,
};
