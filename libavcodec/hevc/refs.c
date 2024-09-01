/*
 * HEVC video decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
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

#include "libavutil/mem.h"

#include "container_fifo.h"
#include "decode.h"
#include "hevc.h"
#include "hevcdec.h"
#include "progressframe.h"
#include "refstruct.h"

void ff_hevc_unref_frame(HEVCFrame *frame, int flags)
{
    frame->flags &= ~flags;
    if (!frame->flags) {
        ff_progress_frame_unref(&frame->tf);
        av_frame_unref(frame->frame_grain);
        frame->needs_fg = 0;

        ff_refstruct_unref(&frame->pps);
        ff_refstruct_unref(&frame->tab_mvf);

        ff_refstruct_unref(&frame->rpl);
        frame->nb_rpl_elems = 0;
        ff_refstruct_unref(&frame->rpl_tab);
        frame->refPicList = NULL;

        ff_refstruct_unref(&frame->hwaccel_picture_private);
    }
}

const RefPicList *ff_hevc_get_ref_list(const HEVCFrame *ref, int x0, int y0)
{
    const HEVCSPS *sps = ref->pps->sps;
    int x_cb         = x0 >> sps->log2_ctb_size;
    int y_cb         = y0 >> sps->log2_ctb_size;
    int pic_width_cb = sps->ctb_width;
    int ctb_addr_ts  = ref->pps->ctb_addr_rs_to_ts[y_cb * pic_width_cb + x_cb];
    return &ref->rpl_tab[ctb_addr_ts]->refPicList[0];
}

void ff_hevc_clear_refs(HEVCLayerContext *l)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
        ff_hevc_unref_frame(&l->DPB[i],
                            HEVC_FRAME_FLAG_SHORT_REF |
                            HEVC_FRAME_FLAG_LONG_REF);
}

void ff_hevc_flush_dpb(HEVCContext *s)
{
    for (int layer = 0; layer < FF_ARRAY_ELEMS(s->layers); layer++) {
        HEVCLayerContext *l = &s->layers[layer];
        for (int i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
            ff_hevc_unref_frame(&l->DPB[i], ~0);
    }
}

static HEVCFrame *alloc_frame(HEVCContext *s, HEVCLayerContext *l)
{
    int i, j, ret;
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];
        if (frame->f)
            continue;

        ret = ff_progress_frame_get_buffer(s->avctx, &frame->tf,
                                           AV_GET_BUFFER_FLAG_REF);
        if (ret < 0)
            return NULL;

        frame->rpl = ff_refstruct_allocz(s->pkt.nb_nals * sizeof(*frame->rpl));
        if (!frame->rpl)
            goto fail;
        frame->nb_rpl_elems = s->pkt.nb_nals;

        frame->tab_mvf = ff_refstruct_pool_get(l->tab_mvf_pool);
        if (!frame->tab_mvf)
            goto fail;

        frame->rpl_tab = ff_refstruct_pool_get(l->rpl_tab_pool);
        if (!frame->rpl_tab)
            goto fail;
        frame->ctb_count = l->sps->ctb_width * l->sps->ctb_height;
        for (j = 0; j < frame->ctb_count; j++)
            frame->rpl_tab[j] = frame->rpl;

        if (s->sei.picture_timing.picture_struct == AV_PICTURE_STRUCTURE_TOP_FIELD)
            frame->f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
        if ((s->sei.picture_timing.picture_struct == AV_PICTURE_STRUCTURE_TOP_FIELD) ||
            (s->sei.picture_timing.picture_struct == AV_PICTURE_STRUCTURE_BOTTOM_FIELD))
            frame->f->flags |= AV_FRAME_FLAG_INTERLACED;

        ret = ff_hwaccel_frame_priv_alloc(s->avctx, &frame->hwaccel_picture_private);
        if (ret < 0)
            goto fail;

        frame->pps = ff_refstruct_ref_c(s->pps);

        return frame;
fail:
        ff_hevc_unref_frame(frame, ~0);
        return NULL;
    }
    av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, DPB full.\n");
    return NULL;
}

int ff_hevc_set_new_ref(HEVCContext *s, HEVCLayerContext *l, int poc)
{
    HEVCFrame *ref;
    int i;

    /* check that this POC doesn't already exist */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];

        if (frame->f && frame->poc == poc) {
            av_log(s->avctx, AV_LOG_ERROR, "Duplicate POC in a sequence: %d.\n",
                   poc);
            return AVERROR_INVALIDDATA;
        }
    }

    ref = alloc_frame(s, l);
    if (!ref)
        return AVERROR(ENOMEM);

    s->cur_frame = ref;
    s->collocated_ref = NULL;

    if (s->sh.pic_output_flag)
        ref->flags = HEVC_FRAME_FLAG_OUTPUT | HEVC_FRAME_FLAG_SHORT_REF;
    else
        ref->flags = HEVC_FRAME_FLAG_SHORT_REF;

    ref->poc      = poc;
    ref->f->crop_left   = l->sps->output_window.left_offset;
    ref->f->crop_right  = l->sps->output_window.right_offset;
    ref->f->crop_top    = l->sps->output_window.top_offset;
    ref->f->crop_bottom = l->sps->output_window.bottom_offset;

    return 0;
}

static void unref_missing_refs(HEVCLayerContext *l)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
         HEVCFrame *frame = &l->DPB[i];
         if (frame->flags & HEVC_FRAME_FLAG_UNAVAILABLE) {
             ff_hevc_unref_frame(frame, ~0);
         }
    }
}

int ff_hevc_output_frames(HEVCContext *s, HEVCLayerContext *l,
                          unsigned max_output, unsigned max_dpb, int discard)
{
    while (1) {
        int nb_dpb    = 0;
        int nb_output = 0;
        int min_poc   = INT_MAX;
        int i, min_idx, ret;

        for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
            HEVCFrame *frame = &l->DPB[i];
            if (frame->flags & HEVC_FRAME_FLAG_OUTPUT) {
                nb_output++;
                if (frame->poc < min_poc || nb_output == 1) {
                    min_poc = frame->poc;
                    min_idx = i;
                }
            }
            nb_dpb += !!frame->flags;
        }

        if (nb_output > max_output ||
            (nb_output && nb_dpb > max_dpb)) {
            HEVCFrame *frame = &l->DPB[min_idx];

            ret = discard ? 0 :
                  ff_container_fifo_write(s->output_fifo,
                                          frame->needs_fg ? frame->frame_grain : frame->f);
            ff_hevc_unref_frame(frame, HEVC_FRAME_FLAG_OUTPUT);
            if (ret < 0)
                return ret;

            av_log(s->avctx, AV_LOG_DEBUG, "%s frame with POC %d.\n",
                   discard ? "Discarded" : "Output", frame->poc);
            continue;
        }
        return 0;
    }
}

static int init_slice_rpl(HEVCContext *s)
{
    HEVCFrame *frame = s->cur_frame;
    int ctb_count    = frame->ctb_count;
    int ctb_addr_ts  = s->pps->ctb_addr_rs_to_ts[s->sh.slice_segment_addr];
    int i;

    if (s->slice_idx >= frame->nb_rpl_elems)
        return AVERROR_INVALIDDATA;

    for (i = ctb_addr_ts; i < ctb_count; i++)
        frame->rpl_tab[i] = frame->rpl + s->slice_idx;

    frame->refPicList = (RefPicList *)frame->rpl_tab[ctb_addr_ts];

    return 0;
}

int ff_hevc_slice_rpl(HEVCContext *s)
{
    SliceHeader *sh = &s->sh;

    uint8_t nb_list = sh->slice_type == HEVC_SLICE_B ? 2 : 1;
    uint8_t list_idx;
    int i, j, ret;

    ret = init_slice_rpl(s);
    if (ret < 0)
        return ret;

    if (!(s->rps[ST_CURR_BEF].nb_refs + s->rps[ST_CURR_AFT].nb_refs +
          s->rps[LT_CURR].nb_refs) && !s->pps->pps_curr_pic_ref_enabled_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Zero refs in the frame RPS.\n");
        return AVERROR_INVALIDDATA;
    }

    for (list_idx = 0; list_idx < nb_list; list_idx++) {
        RefPicList  rpl_tmp = { { 0 } };
        RefPicList *rpl     = &s->cur_frame->refPicList[list_idx];

        /* The order of the elements is
         * ST_CURR_BEF - ST_CURR_AFT - LT_CURR for the L0 and
         * ST_CURR_AFT - ST_CURR_BEF - LT_CURR for the L1 */
        int cand_lists[3] = { list_idx ? ST_CURR_AFT : ST_CURR_BEF,
                              list_idx ? ST_CURR_BEF : ST_CURR_AFT,
                              LT_CURR };

        /* concatenate the candidate lists for the current frame */
        while (rpl_tmp.nb_refs < sh->nb_refs[list_idx]) {
            for (i = 0; i < FF_ARRAY_ELEMS(cand_lists); i++) {
                RefPicList *rps = &s->rps[cand_lists[i]];
                for (j = 0; j < rps->nb_refs && rpl_tmp.nb_refs < HEVC_MAX_REFS; j++) {
                    rpl_tmp.list[rpl_tmp.nb_refs]       = rps->list[j];
                    rpl_tmp.ref[rpl_tmp.nb_refs]        = rps->ref[j];
                    rpl_tmp.isLongTerm[rpl_tmp.nb_refs] = i == 2;
                    rpl_tmp.nb_refs++;
                }
            }
            // Construct RefPicList0, RefPicList1 (8-8, 8-10)
            if (s->pps->pps_curr_pic_ref_enabled_flag && rpl_tmp.nb_refs < HEVC_MAX_REFS) {
                rpl_tmp.list[rpl_tmp.nb_refs]           = s->cur_frame->poc;
                rpl_tmp.ref[rpl_tmp.nb_refs]            = s->cur_frame;
                rpl_tmp.isLongTerm[rpl_tmp.nb_refs]     = 1;
                rpl_tmp.nb_refs++;
            }
        }

        /* reorder the references if necessary */
        if (sh->rpl_modification_flag[list_idx]) {
            for (i = 0; i < sh->nb_refs[list_idx]; i++) {
                int idx = sh->list_entry_lx[list_idx][i];

                if (idx >= rpl_tmp.nb_refs) {
                    av_log(s->avctx, AV_LOG_ERROR, "Invalid reference index.\n");
                    return AVERROR_INVALIDDATA;
                }

                rpl->list[i]       = rpl_tmp.list[idx];
                rpl->ref[i]        = rpl_tmp.ref[idx];
                rpl->isLongTerm[i] = rpl_tmp.isLongTerm[idx];
                rpl->nb_refs++;
            }
        } else {
            memcpy(rpl, &rpl_tmp, sizeof(*rpl));
            rpl->nb_refs = FFMIN(rpl->nb_refs, sh->nb_refs[list_idx]);
        }

        // 8-9
        if (s->pps->pps_curr_pic_ref_enabled_flag &&
            !sh->rpl_modification_flag[list_idx] &&
            rpl_tmp.nb_refs > sh->nb_refs[L0]) {
            rpl->list[sh->nb_refs[L0] - 1] = s->cur_frame->poc;
            rpl->ref[sh->nb_refs[L0] - 1]  = s->cur_frame;
        }

        if (sh->collocated_list == list_idx &&
            sh->collocated_ref_idx < rpl->nb_refs)
            s->collocated_ref = rpl->ref[sh->collocated_ref_idx];
    }

    return 0;
}

static HEVCFrame *find_ref_idx(HEVCContext *s, HEVCLayerContext *l,
                               int poc, uint8_t use_msb)
{
    int mask = use_msb ? ~0 : (1 << l->sps->log2_max_poc_lsb) - 1;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *ref = &l->DPB[i];
        if (ref->f) {
            if ((ref->poc & mask) == poc && (use_msb || ref->poc != s->poc))
                return ref;
        }
    }

    if (s->nal_unit_type != HEVC_NAL_CRA_NUT && !IS_BLA(s))
        av_log(s->avctx, AV_LOG_ERROR,
               "Could not find ref with POC %d\n", poc);
    return NULL;
}

static void mark_ref(HEVCFrame *frame, int flag)
{
    frame->flags &= ~(HEVC_FRAME_FLAG_LONG_REF | HEVC_FRAME_FLAG_SHORT_REF);
    frame->flags |= flag;
}

static HEVCFrame *generate_missing_ref(HEVCContext *s, HEVCLayerContext *l, int poc)
{
    HEVCFrame *frame;
    int i, y;

    frame = alloc_frame(s, l);
    if (!frame)
        return NULL;

    if (!s->avctx->hwaccel) {
        if (!l->sps->pixel_shift) {
            for (i = 0; frame->f->data[i]; i++)
                memset(frame->f->data[i], 1 << (l->sps->bit_depth - 1),
                       frame->f->linesize[i] * AV_CEIL_RSHIFT(l->sps->height, l->sps->vshift[i]));
        } else {
            for (i = 0; frame->f->data[i]; i++)
                for (y = 0; y < (l->sps->height >> l->sps->vshift[i]); y++) {
                    uint8_t *dst = frame->f->data[i] + y * frame->f->linesize[i];
                    AV_WN16(dst, 1 << (l->sps->bit_depth - 1));
                    av_memcpy_backptr(dst + 2, 2, 2*(l->sps->width >> l->sps->hshift[i]) - 2);
                }
        }
    }

    frame->poc      = poc;
    frame->flags    = HEVC_FRAME_FLAG_UNAVAILABLE;

    if (s->avctx->active_thread_type == FF_THREAD_FRAME)
        ff_progress_frame_report(&frame->tf, INT_MAX);

    return frame;
}

/* add a reference with the given poc to the list and mark it as used in DPB */
static int add_candidate_ref(HEVCContext *s, HEVCLayerContext *l,
                             RefPicList *list,
                             int poc, int ref_flag, uint8_t use_msb)
{
    HEVCFrame *ref = find_ref_idx(s, l, poc, use_msb);

    if (ref == s->cur_frame || list->nb_refs >= HEVC_MAX_REFS)
        return AVERROR_INVALIDDATA;

    if (!ref) {
        ref = generate_missing_ref(s, l, poc);
        if (!ref)
            return AVERROR(ENOMEM);
    }

    list->list[list->nb_refs] = ref->poc;
    list->ref[list->nb_refs]  = ref;
    list->nb_refs++;

    mark_ref(ref, ref_flag);
    return 0;
}

int ff_hevc_frame_rps(HEVCContext *s, HEVCLayerContext *l)
{
    const ShortTermRPS *short_rps = s->sh.short_term_rps;
    const LongTermRPS  *long_rps  = &s->sh.long_term_rps;
    RefPicList               *rps = s->rps;
    int i, ret = 0;

    if (!short_rps) {
        rps[0].nb_refs = rps[1].nb_refs = 0;
        return 0;
    }

    unref_missing_refs(l);

    /* clear the reference flags on all frames except the current one */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];

        if (frame == s->cur_frame)
            continue;

        mark_ref(frame, 0);
    }

    for (i = 0; i < NB_RPS_TYPE; i++)
        rps[i].nb_refs = 0;

    /* add the short refs */
    for (i = 0; i < short_rps->num_delta_pocs; i++) {
        int poc = s->poc + short_rps->delta_poc[i];
        int list;

        if (!(short_rps->used & (1 << i)))
            list = ST_FOLL;
        else if (i < short_rps->num_negative_pics)
            list = ST_CURR_BEF;
        else
            list = ST_CURR_AFT;

        ret = add_candidate_ref(s, l, &rps[list], poc,
                                HEVC_FRAME_FLAG_SHORT_REF, 1);
        if (ret < 0)
            goto fail;
    }

    /* add the long refs */
    for (i = 0; i < long_rps->nb_refs; i++) {
        int poc  = long_rps->poc[i];
        int list = long_rps->used[i] ? LT_CURR : LT_FOLL;

        ret = add_candidate_ref(s, l, &rps[list], poc,
                                HEVC_FRAME_FLAG_LONG_REF, long_rps->poc_msb_present[i]);
        if (ret < 0)
            goto fail;
    }

fail:
    /* release any frames that are now unused */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
        ff_hevc_unref_frame(&l->DPB[i], 0);

    return ret;
}

int ff_hevc_frame_nb_refs(const SliceHeader *sh, const HEVCPPS *pps)
{
    int ret = 0;
    int i;
    const ShortTermRPS     *rps = sh->short_term_rps;
    const LongTermRPS *long_rps = &sh->long_term_rps;

    if (rps) {
        for (i = 0; i < rps->num_negative_pics; i++)
            ret += !!(rps->used & (1 << i));
        for (; i < rps->num_delta_pocs; i++)
            ret += !!(rps->used & (1 << i));
    }

    if (long_rps) {
        for (i = 0; i < long_rps->nb_refs; i++)
            ret += !!long_rps->used[i];
    }

    if (pps->pps_curr_pic_ref_enabled_flag)
        ret++;

    return ret;
}