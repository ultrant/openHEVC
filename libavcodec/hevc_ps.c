/*
 * HEVC Parameter Set decoding
 *
 * Copyright (C) 2012 - 2103 Guillaume Martres
 * Copyright (C) 2012 - 2103 Mickael Raulet
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
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

#include "libavutil/imgutils.h"
#include "golomb.h"
#include "hevc.h"

static const uint8_t default_scaling_list_intra[] = {
    16, 16, 16, 16, 17, 18, 21, 24,
    16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29,
    16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47,
    18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88,
    24, 25, 29, 36, 47, 65, 88, 115
};

static const uint8_t default_scaling_list_inter[] = {
    16, 16, 16, 16, 17, 18, 20, 24,
    16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28,
    16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41,
    18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71,
    24, 25, 28, 33, 41, 54, 71, 91
};

static const AVRational vui_sar[] = {
    {  0,   1 },
    {  1,   1 },
    { 12,  11 },
    { 10,  11 },
    { 16,  11 },
    { 40,  33 },
    { 24,  11 },
    { 20,  11 },
    { 32,  11 },
    { 80,  33 },
    { 18,  11 },
    { 15,  11 },
    { 64,  33 },
    { 160, 99 },
    {  4,   3 },
    {  3,   2 },
    {  2,   1 },
};

int ff_hevc_decode_short_term_rps(HEVCContext *s, ShortTermRPS *rps,
                                  const HEVCSPS *sps, int is_slice_header)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t rps_predict = 0;
    int delta_poc;
    int k0 = 0;
    int k1 = 0;
    int k  = 0;
    int i;

    GetBitContext *gb = &lc->gb;

    if (rps != sps->st_rps && sps->nb_st_rps)
        rps_predict = get_bits1(gb);

    if (rps_predict) {
        const ShortTermRPS *rps_ridx;
        int delta_rps, abs_delta_rps;
        uint8_t use_delta_flag = 0;
        uint8_t delta_rps_sign;

        if (is_slice_header) {
            unsigned int delta_idx = get_ue_golomb_long(gb) + 1;
            if (delta_idx > sps->nb_st_rps) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Invalid value of delta_idx in slice header RPS: %d > %d.\n",
                       delta_idx, sps->nb_st_rps);
                return AVERROR_INVALIDDATA;
            }
            rps_ridx = &sps->st_rps[sps->nb_st_rps - delta_idx];
        } else
            rps_ridx = &sps->st_rps[rps - sps->st_rps - 1];

        delta_rps_sign = get_bits1(gb);
        abs_delta_rps  = get_ue_golomb_long(gb) + 1;
        delta_rps      = (1 - (delta_rps_sign << 1)) * abs_delta_rps;
        for (i = 0; i <= rps_ridx->num_delta_pocs; i++) {
            int used = rps->used[k] = get_bits1(gb);

            if (!used)
                use_delta_flag = get_bits1(gb);

            if (used || use_delta_flag) {
                if (i < rps_ridx->num_delta_pocs)
                    delta_poc = delta_rps + rps_ridx->delta_poc[i];
                else
                    delta_poc = delta_rps;
                rps->delta_poc[k] = delta_poc;
                if (delta_poc < 0)
                    k0++;
                else
                    k1++;
                k++;
            }
        }

        rps->num_delta_pocs    = k;
        rps->num_negative_pics = k0;
        // sort in increasing order (smallest first)
        if (rps->num_delta_pocs != 0) {
            int used, tmp;
            for (i = 1; i < rps->num_delta_pocs; i++) {
                delta_poc = rps->delta_poc[i];
                used      = rps->used[i];
                for (k = i - 1; k >= 0; k--) {
                    tmp = rps->delta_poc[k];
                    if (delta_poc < tmp) {
                        rps->delta_poc[k + 1] = tmp;
                        rps->used[k + 1]      = rps->used[k];
                        rps->delta_poc[k]     = delta_poc;
                        rps->used[k]          = used;
                    }
                }
            }
        }
        if ((rps->num_negative_pics >> 1) != 0) {
            int used;
            k = rps->num_negative_pics - 1;
            // flip the negative values to largest first
            for (i = 0; i < rps->num_negative_pics >> 1; i++) {
                delta_poc         = rps->delta_poc[i];
                used              = rps->used[i];
                rps->delta_poc[i] = rps->delta_poc[k];
                rps->used[i]      = rps->used[k];
                rps->delta_poc[k] = delta_poc;
                rps->used[k]      = used;
                k--;
            }
        }
    } else {
        unsigned int prev, nb_positive_pics;
        rps->num_negative_pics = get_ue_golomb_long(gb);
        nb_positive_pics       = get_ue_golomb_long(gb);

        if (rps->num_negative_pics >= MAX_REFS ||
            nb_positive_pics >= MAX_REFS) {
            av_log(s->avctx, AV_LOG_ERROR, "Too many refs in a short term RPS.\n");
            return AVERROR_INVALIDDATA;
        }

        rps->num_delta_pocs = rps->num_negative_pics + nb_positive_pics;
        if (rps->num_delta_pocs) {
            prev = 0;
            for (i = 0; i < rps->num_negative_pics; i++) {
                delta_poc = get_ue_golomb_long(gb) + 1;
                prev -= delta_poc;
                rps->delta_poc[i] = prev;
                rps->used[i]      = get_bits1(gb);
            }
            prev = 0;
            for (i = 0; i < nb_positive_pics; i++) {
                delta_poc = get_ue_golomb_long(gb) + 1;
                prev += delta_poc;
                rps->delta_poc[rps->num_negative_pics + i] = prev;
                rps->used[rps->num_negative_pics + i]      = get_bits1(gb);
            }
        }
    }
    return 0;
}


static void decode_profile_tier_level(HEVCContext *s, PTLCommon *ptl)
{
    int i;
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb = &lc->gb;
    print_cabac(" --- parse tier level --- ", s->nuh_layer_id);
    ptl->profile_space = get_bits(gb, 2);
    ptl->tier_flag     = get_bits1(gb);
    ptl->profile_idc   = get_bits(gb, 5);

    print_cabac("XXX_profile_space", ptl->profile_space );
    print_cabac("XXX_tier_flag", ptl->tier_flag);
    print_cabac("XXX_profile_idc", ptl->profile_idc);

    if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN)
        av_log(s->avctx, AV_LOG_DEBUG, "Main profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN_10)
        av_log(s->avctx, AV_LOG_DEBUG, "Main 10 profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_MAIN_STILL_PICTURE)
        av_log(s->avctx, AV_LOG_DEBUG, "Main Still Picture profile bitstream\n");
    else if (ptl->profile_idc == FF_PROFILE_HEVC_REXT)
        av_log(s->avctx, AV_LOG_DEBUG, "Range Extension profile bitstream\n");
    else
        av_log(s->avctx, AV_LOG_WARNING, "Unknown HEVC profile: %d\n", ptl->profile_idc);

    for (i = 0; i < 32; i++)
        ptl->profile_compatibility_flag[i] = get_bits1(gb);
    ptl->progressive_source_flag    = get_bits1(gb);
    ptl->interlaced_source_flag     = get_bits1(gb);
    ptl->non_packed_constraint_flag = get_bits1(gb);
    ptl->frame_only_constraint_flag = get_bits1(gb);

    print_cabac("general_progressive_source_flag", ptl->progressive_source_flag);
    print_cabac("general_interlaced_source_flag", ptl->interlaced_source_flag);
    print_cabac("general_non_packed_constraint_flag", ptl->non_packed_constraint_flag);
    print_cabac("general_frame_only_constraint_flag", ptl->frame_only_constraint_flag);

    skip_bits(gb, 16); // XXX_reserved_zero_44bits[0..15]
    skip_bits(gb, 16); // XXX_reserved_zero_44bits[16..31]
    skip_bits(gb, 12); // XXX_reserved_zero_44bits[32..43]
}

static void parse_ptl(HEVCContext *s, PTL *ptl, int max_num_sub_layers)
{
    int i;
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb = &lc->gb;

    print_cabac(" --- parse ptl --- ", s->nuh_layer_id);
    decode_profile_tier_level(s, &ptl->general_ptl);
    ptl->general_ptl.level_idc = get_bits(gb, 8);
    print_cabac("general_level_idc", ptl->general_ptl.level_idc);

    for (i = 0; i < max_num_sub_layers - 1; i++) {
        ptl->sub_layer_profile_present_flag[i] = get_bits1(gb);
        print_cabac("sub_layer_profile_present_flag", ptl->sub_layer_profile_present_flag[i]);  
        ptl->sub_layer_level_present_flag[i]   = get_bits1(gb);
        print_cabac("sub_layer_level_present_flag", ptl->sub_layer_level_present_flag[i]); 
    }
    if (max_num_sub_layers - 1 > 0)
        for (i = max_num_sub_layers - 1; i < 8; i++) {
            skip_bits(gb, 2); // reserved_zero_2bits[i]
            print_cabac("reserved_zero_2bits", 0);
        }
    for (i = 0; i < max_num_sub_layers - 1; i++) {
        if (ptl->sub_layer_profile_present_flag[i])
            decode_profile_tier_level(s, &ptl->sub_layer_ptl[i]);
        if (ptl->sub_layer_level_present_flag[i]) {
            ptl->sub_layer_ptl[i].level_idc = get_bits(gb, 8);
            print_cabac("sub_layer_level_idc", ptl->sub_layer_ptl[i].level_idc);
        }
    }
}

static void decode_sublayer_hrd(HEVCContext *s, unsigned int nb_cpb,
                                int subpic_params_present)
{
    GetBitContext *gb = &s->HEVClc->gb;
    int i;

    for (i = 0; i < nb_cpb; i++) {
        get_ue_golomb_long(gb); // bit_rate_value_minus1
        get_ue_golomb_long(gb); // cpb_size_value_minus1

        if (subpic_params_present) {
            get_ue_golomb_long(gb); // cpb_size_du_value_minus1
            get_ue_golomb_long(gb); // bit_rate_du_value_minus1
        }
        skip_bits1(gb); // cbr_flag
    }
}

static void decode_hrd(HEVCContext *s, int common_inf_present,
                       int max_sublayers)
{
    GetBitContext *gb = &s->HEVClc->gb;
    int nal_params_present = 0, vcl_params_present = 0;
    int subpic_params_present = 0;
    int i;

    if (common_inf_present) {
        nal_params_present = get_bits1(gb);
        vcl_params_present = get_bits1(gb);

        if (nal_params_present || vcl_params_present) {
            subpic_params_present = get_bits1(gb);

            if (subpic_params_present) {
                skip_bits(gb, 8); // tick_divisor_minus2
                skip_bits(gb, 5); // du_cpb_removal_delay_increment_length_minus1
                skip_bits(gb, 1); // sub_pic_cpb_params_in_pic_timing_sei_flag
                skip_bits(gb, 5); // dpb_output_delay_du_length_minus1
            }

            skip_bits(gb, 4); // bit_rate_scale
            skip_bits(gb, 4); // cpb_size_scale

            if (subpic_params_present)
                skip_bits(gb, 4);  // cpb_size_du_scale

            skip_bits(gb, 5); // initial_cpb_removal_delay_length_minus1
            skip_bits(gb, 5); // au_cpb_removal_delay_length_minus1
            skip_bits(gb, 5); // dpb_output_delay_length_minus1
        }
    }

    for (i = 0; i < max_sublayers; i++) {
        int low_delay = 0;
        unsigned int nb_cpb = 1;
        int fixed_rate = get_bits1(gb);

        if (!fixed_rate)
            fixed_rate = get_bits1(gb);

        if (fixed_rate)
            get_ue_golomb_long(gb);  // elemental_duration_in_tc_minus1
        else
            low_delay = get_bits1(gb);

        if (!low_delay)
            nb_cpb = get_ue_golomb_long(gb) + 1;

        if (nal_params_present)
            decode_sublayer_hrd(s, nb_cpb, subpic_params_present);
        if (vcl_params_present)
            decode_sublayer_hrd(s, nb_cpb, subpic_params_present);
    }
}

static int scal_type_to_scal_idx(HEVCVPS *vps, enum ScalabilityType scal_type)
{
    int scal_idx = 0;
    int i;

    for (i = 0; i < scal_type; i++) {
        scal_idx += (vps->scalability_mask[i] ? 1 : 0);

    }

    return scal_idx;
}

static int get_scalability_id(HEVCVPS * vps, int layer_id_in_vps, enum ScalabilityType scal_type) {
    return vps->scalability_mask[scal_type] ? vps->dimension_id[layer_id_in_vps][scal_type_to_scal_idx(vps, scal_type)] : 0;
}

static int get_view_index(HEVCVPS *vps, int id){
    return get_scalability_id(vps, vps->layer_id_in_vps[id], VIEW_ORDER_INDEX);
}

static int get_num_views(HEVCVPS *vps)
{
    int num_views = 1;
    int i;

    for (i = 0; i < vps->vps_max_layers; i++) {
        int lId = vps->layer_id_in_nuh[i];
        if (i > 0 && (get_view_index(vps, lId) != get_scalability_id(vps, i - 1, VIEW_ORDER_INDEX))) {
            num_views++;
        }
    }

    return num_views;
}

static void parse_rep_format(RepFormat *rep_format, GetBitContext *gb)
{
    int chroma_and_bit_depth_flag;

    print_cabac(" --- parse RepFormat  --- ", 0);
    rep_format->pic_width_vps_in_luma_samples = get_bits(gb, 16);
    print_cabac("pic_width_in_luma_samples", rep_format->pic_width_vps_in_luma_samples);
    rep_format->pic_height_vps_in_luma_samples = get_bits(gb, 16);;
    print_cabac("pic_height_in_luma_samples", rep_format->pic_height_vps_in_luma_samples);
    chroma_and_bit_depth_flag = get_bits1(gb);
    print_cabac("chroma_and_bit_depth_vps_present_flag", chroma_and_bit_depth_flag);
    if (chroma_and_bit_depth_flag) {
        rep_format->chroma_format_vps_idc = get_bits(gb, 2);
        print_cabac("chroma_format_idc", rep_format->chroma_format_vps_idc);

        if (rep_format->chroma_format_vps_idc == 3) {
            int separate_colour_plane_flag = get_bits1(gb);
            print_cabac("separate_colour_plane_flag", separate_colour_plane_flag);
        }

        rep_format->bit_depth_vps_luma =   get_bits(gb, 4) + 8;
        print_cabac("bit_depth_luma_minus8", rep_format->bit_depth_vps_luma - 8);
        rep_format->bit_depth_vps_chroma = get_bits(gb, 4) + 8;
        print_cabac("bit_depth_chroma_minus8", rep_format->bit_depth_vps_chroma - 8);
    }
}

static void deriveLayerIdListVariables(HEVCVPS *vps)
{
    int i, m, n;
    // For layer 0
    vps->num_layer_in_id_list[0] = 1;

    for (i = 1; i < vps->vps_num_layer_sets; i++) {
        n = 0;
        for (m = 0; m <= vps->vps_max_layer_id; m++) {
            if (vps->layer_id_included_flag[i][m]) {
                n++;
            }
        }
        vps->num_layer_in_id_list[i] = n;
    }
}

static void derive_number_of_sub_dpbs(HEVCVPS *vps) {
    int i; 
    vps->m_numSubDpbs[0] = 1;

    for (i = 1; i < vps->num_output_layer_sets; i++) {
        vps->m_numSubDpbs[i] = vps->num_layer_in_id_list[vps->layer_set_idx_for_ols[i]];
    }
}

static void setTilesNotInUseFlag(HEVCVPS *vps, unsigned int x)
{
    int i, j;
    vps->m_tilesNotInUseFlag = x;
    if (vps->m_tilesNotInUseFlag) {
        for (i = 0; i < vps->vps_max_layers; i++) {
            vps->m_tilesInUseFlag[i] = vps->m_loopFilterNotAcrossTilesFlag[i] = vps->m_tilesNotInUseFlag;
        }
    }
    if (vps->m_tilesNotInUseFlag) {
        for (i = 1; i < vps->vps_max_layers; i++) {
            for (j = 0; j < vps->num_direct_ref_layers[vps->layer_id_in_nuh[i]]; j++) {
                vps->m_tileBoundariesAlignedFlag[i][j]  = vps->m_tilesNotInUseFlag;
            }
        }
    }
}

static void parse_vps_vui(GetBitContext *gb, HEVCVPS *vps)
{
    int i,j;
    print_cabac(" \n --- parse vps vui --- \n", 0);
    vps->m_crossLayerPictureTypeAlignFlag = get_bits1(gb);
    print_cabac("cross_layer_pic_type_aligned_flag", vps->m_crossLayerPictureTypeAlignFlag); 
    if (!vps->m_crossLayerPictureTypeAlignFlag) {
        vps->m_crossLayerIrapAlignFlag = get_bits1(gb);
        print_cabac("cross_layer_irap_aligned_flag", vps->m_crossLayerIrapAlignFlag);
    } else
        vps->m_crossLayerPictureTypeAlignFlag = 1;

    vps->m_bitRatePresentVpsFlag = get_bits1(gb);
    vps->m_picRatePresentVpsFlag = get_bits1(gb);
    print_cabac("bit_rate_present_vps_flag", vps->m_bitRatePresentVpsFlag);
    print_cabac("pic_rate_present_vps_flag", vps->m_picRatePresentVpsFlag);
    int parseFlag = vps->m_bitRatePresentVpsFlag || vps->m_picRatePresentVpsFlag;
    {
        for (i = 0; i < vps->vps_num_layer_sets; i++) {
            for (j = 0; j < vps->vps_max_sub_layers; j++) {
                if (parseFlag && vps->m_bitRatePresentVpsFlag) {
                    vps->m_bitRatePresentFlag[i][j] = get_bits1(gb);
                    print_cabac("bit_rate_present_vps_flag", vps->m_bitRatePresentFlag[i][j]);
                } else
                    vps->m_bitRatePresentFlag[i][j] = 0;
                if (parseFlag && vps->m_picRatePresentVpsFlag) {
                    vps->m_bitRatePresentFlag[i][j] = get_bits1(gb);
                    print_cabac("pic_rate_present_vps_flag", vps->m_bitRatePresentFlag[i][j]);
                } else
                    vps->m_bitRatePresentFlag[i][j] = 0;
                if (parseFlag && vps->m_bitRatePresentFlag[i][j]) {
                    vps->m_avgBitRate[i][j] = get_bits(gb, 16);
                    vps->m_maxBitRate[i][j] = get_bits(gb, 16);
                    print_cabac("avg_bit_rate", vps->m_avgBitRate[i][j]);
                    print_cabac("max_bit_rate", vps->m_maxBitRate[i][j]);
                } else {
                    vps->m_avgBitRate[i][j] = 0;
                    vps->m_maxBitRate[i][j] = 0;
                }
                if (parseFlag && vps->m_picRatePresentFlag[i][j]) {
                    vps->m_constPicRateIdc[i][j] = get_bits(gb, 2);
                    vps->m_avgPicRate[i][j] = get_bits(gb, 16);
                    print_cabac("constant_pic_rate_idc", vps->m_constPicRateIdc[i][j]);
                    print_cabac("avg_pic_rate", vps->m_avgPicRate[i][j]);
                } else {
                    vps->m_constPicRateIdc[i][j] = 0;
                    vps->m_avgPicRate[i][j]      = 0;
                }
            }
        }
    }
    unsigned int layerIdx;
    int refCode = get_bits1(gb);
    print_cabac("tiles_not_in_use_flag", refCode);
    if (!refCode) {
        for (i = 0; i < vps->vps_max_layers; i++) {

            refCode = get_bits1(gb);
            vps->m_tilesInUseFlag[i] = (refCode == 1);
            print_cabac("tiles_in_use_flag", refCode);
            if (refCode) {
                vps->m_loopFilterNotAcrossTilesFlag[i] = (get_bits1(gb)==1);
                print_cabac("loop_filter_not_across_tiles_flag", vps->m_loopFilterNotAcrossTilesFlag[i]);
            } else
                vps->m_loopFilterNotAcrossTilesFlag[i] = 0;
        }

        for (i = 1; i < vps->vps_max_layers; i++) {

            for (j = 0; j < vps->num_direct_ref_layers[vps->layer_id_in_nuh[i]]; j++) {
                layerIdx = vps->layer_id_in_vps[vps->ref_layer_id[vps->layer_id_in_nuh[i]][j]];

                if (vps->m_tilesInUseFlag[i] && vps->m_tilesInUseFlag[layerIdx]) {
                    refCode = get_bits1(gb);
                    vps->m_tileBoundariesAlignedFlag[i][j] = (refCode==1);
                    print_cabac("tile_boundaries_aligned_flag", refCode);  
                }
            }
        }
    }

    refCode = get_bits1(gb);
    print_cabac("wpp_not_in_use_flag", refCode);  
    vps->m_wppNotInUseFlag = (refCode==1);
    if (!vps->m_wppNotInUseFlag)
        for (i = 0; i < vps->vps_max_layers; i++) {
            vps->m_wppInUseFlag[i] = get_bits1(gb) ;
            print_cabac("wpp_in_use_flag", vps->m_wppInUseFlag[i]);  
        }
    vps->m_numIlpRestrictedRefLayers = (get_bits1(gb) == 1);
    print_cabac("num_ilp_restricted_ref_layers", vps->m_numIlpRestrictedRefLayers); 

    if( vps->m_numIlpRestrictedRefLayers) {
        for (i = 1; i < vps->vps_max_layers; i++) {
            for (j = 0; j <  vps->num_direct_ref_layers[vps->layer_id_in_nuh[i]]; j++) {
                vps->m_minSpatialSegmentOffsetPlus1[i][j] = get_ue_golomb_long(gb);
                print_cabac("min_spatial_segment_offset_plus1", vps->m_minSpatialSegmentOffsetPlus1[i][j]); 

                if (vps->m_minSpatialSegmentOffsetPlus1[i][j] > 0) {
                    vps->m_ctuBasedOffsetEnabledFlag[i][j] = get_bits1(gb);
                    print_cabac("ctu_based_offset_enabled_flag", vps->m_ctuBasedOffsetEnabledFlag[i][j]); 
                    if (vps->m_ctuBasedOffsetEnabledFlag[i][j]) {
                        vps->m_minHorizontalCtuOffsetPlus1[i][j] = get_ue_golomb_long(gb);
                        print_cabac("min_horizontal_ctu_offset_plus1", vps->m_minHorizontalCtuOffsetPlus1[i][j]);
                    }
                }
            }
        }
    }

    vps->m_vidSigPresentVpsFlag = get_bits1(gb);
    print_cabac("video_signal_info_idx_present_flag",  vps->m_vidSigPresentVpsFlag);
    if (vps->m_vidSigPresentVpsFlag) {
        vps->m_vpsVidSigInfo = get_bits(gb, 4) + 1;
        print_cabac("vps_num_video_signal_info_minus1", vps->m_vpsVidSigInfo - 1);
    }
    else
        vps->m_vpsVidSigInfo = vps->vps_max_layers;

    for (i = 0; i < vps->m_vpsVidSigInfo; i++) {
        vps->m_vpsVidFormat[i]      = get_bits(gb, 3);
        vps->m_vpsFullRangeFlag[i]  = get_bits1(gb) ;
        vps->m_vpsColorPrimaries[i] = get_bits(gb, 8);
        vps->m_vpsTransChar[i]      = get_bits(gb, 8);
        vps->m_vpsMatCoeff[i]       = get_bits(gb, 8);

        print_cabac("video_vps_format", vps->m_vpsVidFormat[i]);
        print_cabac("video_full_range_vps_flag", vps->m_vpsFullRangeFlag[i]);
        print_cabac("color_primaries_vps", vps->m_vpsColorPrimaries[i]);
        print_cabac("transfer_characteristics_vps", vps->m_vpsTransChar[i]);
        print_cabac("matrix_coeffs_vps", vps->m_vpsMatCoeff[i]);
    }
    if (!vps->m_vidSigPresentVpsFlag) {
        for (i = 0; i < vps->vps_max_layers; i++)
            vps->m_vpsVidSigIdx[i] = i;
    } else {
        vps->m_vpsVidSigIdx[0] = 0;
        if (vps->m_vpsVidSigInfo > 1) {
            for (i = 1; i < vps->vps_max_layers; i++) {
                vps->m_vpsVidSigIdx[i] = get_bits(gb, 4);
                print_cabac("vps_video_signal_info_idx", vps->m_vpsVidSigIdx[i]);
            }
        }
        else {
            for (i = 1; i < vps->vps_max_layers; i++)
                vps->m_vpsVidSigIdx[i] = 0;
        }
    }
}

static void parse_vps_extension (HEVCContext *s, HEVCVPS *vps)  {
    int i, j, k;
    GetBitContext *gb = &s->HEVClc->gb;
    int num_scalability_types = 0;
    int splitting_flag;
    uint8_t nuh_layer_id_present_flag;
    uint8_t view_id_len;
    int dimension_id_len[MAX_VPS_NUM_SCALABILITY_TYPES];
    int counted_layer_idx_flag[MAX_VPS_LAYER_ID_PLUS1] = { 0 };
    uint8_t vps_sub_layers_max_minus1_present_flag;
    uint8_t sub_layers_vps_max_minus1[MAX_VPS_LAYER_ID_PLUS1 - 1];
    uint8_t max_tid_ref_present_flag;
    uint16_t num_add_layer_sets = 0;
    uint16_t num_layer_sets;
    int num_independent_layers;
    int rep_format_idx_present_flag;



    print_cabac(" \n --- parse vps extention  --- \n ", s->nuh_layer_id);

#if !HM_MV // SHVC hack
    vps->avc_base_layer_flag = get_bits1(gb);
    print_cabac("avc_base_layer_flag", vps->avc_base_layer_flag);
#endif
    splitting_flag = get_bits1(gb);
    print_cabac("splitting_flag", splitting_flag);
    for (i = 0; i < MAX_VPS_NUM_SCALABILITY_TYPES; i++) {
        vps->scalability_mask[i] = get_bits1(gb); /* scalability_mask[i] */
        print_cabac("vps->scalability_mask[i]", vps->scalability_mask[i]);
        num_scalability_types += vps->scalability_mask[i];
    }

    for (i = 0; i < num_scalability_types - splitting_flag; i++) {
        dimension_id_len[i] = get_bits(gb, 3) + 1;
        print_cabac("dimension_id_len_minus1", dimension_id_len[i] - 1);
    }

    nuh_layer_id_present_flag = get_bits1(gb);
    print_cabac("vps_nuh_layer_id_present_flag", nuh_layer_id_present_flag);


    vps->layer_id_in_vps[0] = 0;

    for (i = 1; i < vps->vps_max_layers; i++) {
        if (nuh_layer_id_present_flag) {
            vps->layer_id_in_nuh[i] = get_bits(gb, 6);
            print_cabac("layer_id_in_nuh", vps->layer_id_in_nuh[i]);
        } else
            vps->layer_id_in_nuh[i] = i;

        vps->layer_id_in_vps[vps->layer_id_in_nuh[i]] = i;
        if (!splitting_flag)
            for (j = 0; j < num_scalability_types; j++)    {
                vps->dimension_id[i][j] = get_bits(gb, dimension_id_len[j]); /* dimension_id[i][j] */
                print_cabac("dimension_id", vps->dimension_id[i][j]);
            }
    }

    view_id_len = get_bits(gb, 4);
    print_cabac("view_id_len", view_id_len);

    if (view_id_len)
        for (i = 0; i < get_num_views(vps); i++) {
            vps->view_id_val[i] = get_bits(gb, view_id_len);
            print_cabac("view_id_val", vps->view_id_val[i]);
        }


    vps->num_direct_ref_layers[0] = 0;
    for (i = 1; i < vps->vps_max_layers; i++) {
        int num_direct_ref_layers = 0;
        for (j = 0; j < i; j++) {
            vps->direct_dependency_flag[i][j] = get_bits1(gb);
            print_cabac("direct_dependency_flag[i][j]", vps->direct_dependency_flag[i][j]);
            if (vps->direct_dependency_flag[i][j]) {
                vps->ref_layer_id[i][num_direct_ref_layers] = j;
                num_direct_ref_layers++;
            }
        }
        vps->num_direct_ref_layers[i] = num_direct_ref_layers;
    }

    for (i = 0; i < vps->vps_max_layers; i++) {
        int curr_layer_id = vps->layer_id_in_nuh[i];
        for (j = 0; j < vps->num_direct_ref_layers[curr_layer_id]; j++) {
            int refLayerId = vps->ref_layer_id[curr_layer_id][j];
            vps->recursive_ref_layer_flag[curr_layer_id][refLayerId] = 1;
            for( k = 0; k < MAX_LAYERS; k++ )
                vps->recursive_ref_layer_flag[curr_layer_id][k] =
                vps->recursive_ref_layer_flag[curr_layer_id][k] || vps->recursive_ref_layer_flag[refLayerId][k];
        }
    }

    for (i = 0, k = 0; i < vps->vps_max_layers; i++ ) {
        int nuh_id = vps->layer_id_in_nuh[i];
        int pred_idx = 0;

        for (j = nuh_id + 1; j < MAX_LAYERS; j++)
            if (vps->recursive_ref_layer_flag[j][nuh_id])
                vps->predicted_layer_id[nuh_id][pred_idx++] = j;
        vps->num_predicted_layers[nuh_id] = pred_idx;
    }

    for (i = 0, k = 0; i < vps->vps_max_layers; i++ ) {
        int nuh_id = vps->layer_id_in_nuh[i];
        if (!vps->num_direct_ref_layers[nuh_id]) {
            vps->tree_partition_layer_id_list[k][0] = nuh_id;
            vps->num_layers_in_tree_partition[k] = 1;

            for (j = 0; j < vps->num_predicted_layers[nuh_id]; j++) {
                int idx = vps->layer_id_in_vps[vps->predicted_layer_id[nuh_id][j]];
                if (!counted_layer_idx_flag[idx]) {
                    vps->tree_partition_layer_id_list[k][vps->num_layers_in_tree_partition[k]] = vps->predicted_layer_id[nuh_id][j];
                    vps->num_layers_in_tree_partition[k]++;
                    counted_layer_idx_flag[idx] = 1;
                }
            }
            k++;
        }
        num_independent_layers = k;
    }



    num_layer_sets = vps->vps_num_layer_sets + num_add_layer_sets;

    vps_sub_layers_max_minus1_present_flag = get_bits1(gb);
    print_cabac("vps_sub_layers_max_minus1_present_flag", vps_sub_layers_max_minus1_present_flag);
    if (vps_sub_layers_max_minus1_present_flag) {
        for (i = 0; i < vps->vps_max_layers; i++) {
            sub_layers_vps_max_minus1[i] = get_bits(gb, 3);
            print_cabac("sub_layers_vps_max_minus1[i]", sub_layers_vps_max_minus1[i]);
        }
    } else {
        for (i = 0; i < vps->vps_max_layers; i++) {
            sub_layers_vps_max_minus1[i] = vps->vps_max_sub_layers - 1;
        }
    }

    max_tid_ref_present_flag = get_bits1(gb);
    print_cabac("max_tid_ref_present_flag", max_tid_ref_present_flag);
    if (max_tid_ref_present_flag) {
        for (i = 0; i < vps->vps_max_layers - 1; i++) {
            for (j = i + 1; j < vps->vps_max_layers; j++) {
                if (vps->direct_dependency_flag[j][i]) {
                    vps->max_tid_il_ref_pics_plus1[i][j] =  get_bits(gb, 3);
                    print_cabac("max_tid_il_ref_pics_plus1", vps->max_tid_il_ref_pics_plus1[i][j]);
                }
            }
        }
    } else {
        for (i = 0; i < vps->vps_max_layers - 1; i++) {
            for (j = i + 1; j < vps->vps_max_layers; j++) {
                vps->max_tid_il_ref_pics_plus1[i][j] = 7;
            }
        }
    }

    vps->all_ref_layers_active_flag = get_bits1(gb);
    print_cabac("all_ref_layers_active_flag", vps->all_ref_layers_active_flag);

    vps->vps_num_profile_tier_level = get_ue_golomb_31(gb) + 1;
    print_cabac("vps_num_profile_tier_level_minus1", vps->vps_num_profile_tier_level - 1);

    if (num_independent_layers > 1)
        num_add_layer_sets = get_ue_golomb(gb);
    //   for( i = 0; i < num_add_layer_sets; i++ )
    //        for( j = 1; j < NumIndependentLayers; j++ )
    //            highest_layer_idx_plus1[ i ][ j ]	u(v)

    //vps->PTLExt = av_malloc(sizeof(PTL*)*vps->vps_num_profile_tier_level); // TODO : add free

    for (i = 1; i < vps->vps_num_profile_tier_level; i++) {
        vps->vps_profile_present_flag[i] = get_bits1(gb);
        print_cabac("vps_profile_present_flag", vps->vps_profile_present_flag[i]);
        if (!vps->vps_profile_present_flag[i]) {

            vps->profile_ref[i] = get_bits(gb, 6) + 1;
            print_cabac("profile_ref_minus1", vps->profile_ref[i] - 1);
            //vps->PTLExt[i] = av_malloc(sizeof(PTL));  // TO DO add free
            memcpy(&vps->PTLExt[i], &vps->PTLExt[vps->profile_ref[i]], sizeof(PTL));
        }
        //vps->PTLExt[i] = av_malloc(sizeof(PTL));  // TODO : add free
        parse_ptl(s, &vps->PTLExt[i], vps->vps_max_sub_layers);
    }

    if (num_layer_sets > 1) {
        vps->num_add_olss = get_ue_golomb_31(gb);
        print_cabac("num_add_olss", vps->num_add_olss);
        vps->num_output_layer_sets = vps->vps_num_layer_sets + vps->num_add_olss;
        vps->default_output_layer_idc = get_bits(gb, 2);
        print_cabac("default_output_layer_idc", vps->default_output_layer_idc);
    }

    for (i = 1; i < vps->num_output_layer_sets; i++) {
        int num_output_layers_in_output_layer_set = 0;
        int idx;
        if( i >= num_layer_sets) {
            vps->layer_set_idx_for_ols[i] = get_ue_golomb_31(gb) + 1;
            print_cabac("layer_set_idx_for_ols_minus1", vps->layer_set_idx_for_ols[i] - 1);
        } else {
            vps->layer_set_idx_for_ols[i] = i;
        }

        idx = vps->layer_set_idx_for_ols[i];

        if (i >= vps->vps_num_layer_sets || vps->default_output_layer_idc == 2) {
            for (j = 0; j < vps->num_layer_in_id_list[idx]; j++) {
                vps->output_layer_flag[i][j] = get_bits1(gb);
                print_cabac("output_layer_flag", vps->output_layer_flag[i][j]);
                num_output_layers_in_output_layer_set += vps->output_layer_flag[i][j];
            }
        } else {
            if (vps->default_output_layer_idc) {
                for (j = 0; j < vps->num_layer_in_id_list[idx]; j++) {
                    vps->output_layer_flag[i][j] = (j == (vps->num_layer_in_id_list[idx] - 1));
                    num_output_layers_in_output_layer_set += vps->output_layer_flag[i][j];
                }
            } else {
                for (j = 0; j < vps->num_layer_in_id_list[idx]; j++) {
                    vps->output_layer_flag[i][j] = 1;
                    num_output_layers_in_output_layer_set += vps->output_layer_flag[i][j];
                }
            }
        }
        if (av_log2(vps->vps_num_profile_tier_level) > 0) { //TODO: SHVC hack
            int num_bits = 1;
            while ((1 << num_bits) < (vps->vps_num_profile_tier_level))  {
                num_bits++;
            }
            vps->profile_level_tier_idx[i] = get_bits(gb, num_bits);
            print_cabac("profile_level_tier_idx", vps->profile_level_tier_idx[i]);
        }
        if (num_output_layers_in_output_layer_set == 1) {
            uint8_t alt_output_layer_flag = get_bits1(gb);
            print_cabac("alt_output_layer_flag", alt_output_layer_flag);
        }
    }

    vps->vps_num_rep_formats = get_ue_golomb_31(gb) + 1;
    print_cabac("vps_num_rep_formats_minus1", vps->vps_num_rep_formats - 1);

    for (i = 0; i < vps->vps_num_rep_formats; i++) {
        parse_rep_format(&vps->vps_rep_format[i], gb);
    }

    if (vps->vps_num_rep_formats > 1) {
        rep_format_idx_present_flag = get_bits1(gb);
        print_cabac("rep_format_idx_present_flag", rep_format_idx_present_flag);
    } else {
        rep_format_idx_present_flag = 0;
    }

    vps->vps_rep_format_idx[0] = 0;
    if (rep_format_idx_present_flag) {
        for (i = 1; i < vps->vps_max_layers; i++) {
            if (vps->vps_num_rep_formats > 1) {
                vps->vps_rep_format_idx[i] = get_bits(gb, 1);
                print_cabac("vps_rep_format_idx", vps->vps_rep_format_idx[i]);
            } else {
                vps->vps_rep_format_idx[i] = 0;
            }
        }
    } else {
        for (i = 1; i < vps->vps_max_layers; i++) {
            vps->vps_rep_format_idx[i] = i;
        }
    }

    vps->max_one_active_ref_layer_flag = get_bits1(gb);
    print_cabac("max_one_active_ref_layer_flag", vps->max_one_active_ref_layer_flag);

    for (i = 1; i< vps->vps_max_layers; i++) {
        if(vps->num_direct_ref_layers[vps->layer_id_in_nuh[i]] == 0) {
            vps->m_pocLsbNotPresentFlag[i] =  get_bits1(gb);
            print_cabac("poc_lsb_not_present_flag", vps->m_pocLsbNotPresentFlag[i]);
        }
    }
    vps->phase_align_flag = get_bits1(gb);
    print_cabac("cross_layer_phase_alignment_flag", vps->phase_align_flag); 

    derive_number_of_sub_dpbs(vps);
    for (i = 1; i < vps->num_output_layer_sets; i++) {
        vps->m_subLayerFlagInfoPresentFlag[i] = get_bits1(gb);
        print_cabac("sub_layer_flag_info_present_flag", vps->m_subLayerFlagInfoPresentFlag[i]);    
        for (j = 0; j < vps->vps_max_sub_layers; j++) {
            if( j > 0 && vps->m_subLayerFlagInfoPresentFlag[i] ){
                vps->m_subLayerDpbInfoPresentFlag[i][j] = get_bits1(gb);
                print_cabac("sub_layer_dpb_info_present_flag", vps->m_subLayerDpbInfoPresentFlag[i][j]);
            }   else    {
                if( j == 0 ) { // Always signal for the first sub-layer
                    vps->m_subLayerDpbInfoPresentFlag[i][j] = 1;
                } else { // if (j != 0) && !vps->getSubLayerFlagInfoPresentFlag(i)
                    vps->m_subLayerDpbInfoPresentFlag[i][j] = 0;
                }
            }
            if (vps->m_subLayerDpbInfoPresentFlag[i][j]) { // If sub-layer DPB information is present
                for (k = 0; k < vps->m_numSubDpbs[i]; k++) {
                    vps->max_vps_dec_pic_buffering_minus1[i][k][j] = get_ue_golomb_long(gb);
                    print_cabac("max_vps_dec_pic_buffering_minus1", vps->max_vps_dec_pic_buffering_minus1[i][k][j]);
                }
                vps->max_vps_num_reorder_pics[i][j] = get_ue_golomb_long(gb);
                vps->max_vps_latency_increase_plus1[i][j]= get_ue_golomb_long(gb);
                print_cabac("max_vps_num_reorder_pics", vps->max_vps_num_reorder_pics[i][j]);
                print_cabac("max_vps_latency_increase_plus1", vps->max_vps_latency_increase_plus1[i][j]);
            }
        }
    }

    vps->direct_dep_type_len = get_ue_golomb_long(gb) + 2;
    print_cabac("direct_dep_type_len_minus2", vps->direct_dep_type_len - 2);
    vps->m_defaultDirectDependencyTypeFlag = get_bits1(gb);
    print_cabac("default_direct_dependency_type_flag", vps->m_defaultDirectDependencyTypeFlag);

    if (vps->m_defaultDirectDependencyTypeFlag) {
        vps->m_defaultDirectDependencyType = get_bits(gb, vps->direct_dep_type_len);
        print_cabac("default_direct_dependency_type", vps->m_defaultDirectDependencyType);
    }

    for (i = 1; i < vps->vps_max_layers; i++) {
        for (j = 0; j < i; j++) {
            if (vps->direct_dependency_flag[i][j]) {
                if (vps->m_defaultDirectDependencyTypeFlag) {
                    vps->m_directDependencyType[i][j] =  vps->m_defaultDirectDependencyType;
                } else {
                    vps->m_directDependencyType[i][j] = get_bits1(gb);
                    print_cabac("direct_dependency_type", vps->m_directDependencyType[i][j]);
                }
            }
        }
    }

    get_ue_golomb_31(gb); //vps_non_vui_extension_length

    if(get_bits1(gb)) {
        align_get_bits(gb);
        parse_vps_vui(gb, vps);
    }
}

int ff_hevc_decode_nal_vps(HEVCContext *s)
{
    int i,j;
    GetBitContext *gb = &s->HEVClc->gb;
    int vps_id = 0;
    HEVCVPS *vps;
    AVBufferRef *vps_buf = av_buffer_allocz(sizeof(*vps));
    print_cabac(" \n --- parse vps --- \n ", s->nuh_layer_id);
    if (!vps_buf)
        return AVERROR(ENOMEM);
    vps = (HEVCVPS*)vps_buf->data;

    av_log(s->avctx, AV_LOG_DEBUG, "Decoding VPS\n");

    vps_id = get_bits(gb, 4);
    print_cabac("vps_video_parameter_set_id", vps_id);

    if (get_bits(gb, 2) != 3) { // vps_reserved_three_2bits
        av_log(s->avctx, AV_LOG_ERROR, "vps_reserved_three_2bits is not three\n");
        goto err;
    }
    print_cabac("vps_reserved_three_2bits", 3);

    vps->vps_max_layers               = get_bits(gb, 6) + 1;
    print_cabac("vps_max_layers_minus1", vps->vps_max_layers-1);
    vps->vps_max_sub_layers           = get_bits(gb, 3) + 1;
    print_cabac("vps_max_sub_layers_minus1", vps->vps_max_sub_layers-1);
    vps->vps_temporal_id_nesting_flag = get_bits1(gb);
    print_cabac("vps_temporal_id_nesting_flag", vps->vps_temporal_id_nesting_flag);

    if (get_bits(gb, 16) != 0xffff) { // vps_reserved_ffff_16bits
        av_log(s->avctx, AV_LOG_ERROR, "vps_reserved_ffff_16bits is not 0xffff\n");
        goto err;
    }

    parse_ptl(s, &vps->ptl, vps->vps_max_sub_layers);

    vps->vps_sub_layer_ordering_info_present_flag = get_bits1(gb);
    print_cabac("vps_sub_layer_ordering_info_present_flag", vps->vps_sub_layer_ordering_info_present_flag);
    i = vps->vps_sub_layer_ordering_info_present_flag ? 0 : vps->vps_max_sub_layers - 1;
    for (; i < vps->vps_max_sub_layers; i++) {
        vps->vps_max_dec_pic_buffering[i] = get_ue_golomb_long(gb) + 1;
        vps->vps_num_reorder_pics[i]      = get_ue_golomb_long(gb);
        vps->vps_max_latency_increase[i]  = get_ue_golomb_long(gb) - 1;
        print_cabac("vps_max_dec_pic_buffering_minus1", vps->vps_max_dec_pic_buffering[i]-1);
        print_cabac("vps_num_reorder_pics", vps->vps_num_reorder_pics[i]);
        print_cabac("vps_max_latency_increase_plus1", vps->vps_max_latency_increase[i]+1);

        if (vps->vps_max_dec_pic_buffering[i] > MAX_DPB_SIZE) {
            av_log(s->avctx, AV_LOG_ERROR, "vps_max_dec_pic_buffering_minus1 out of range: %d\n",
                   vps->vps_max_dec_pic_buffering[i] - 1);
            goto err;
        }
        if (vps->vps_num_reorder_pics[i] > vps->vps_max_dec_pic_buffering[i] - 1) {
            av_log(s->avctx, AV_LOG_ERROR, "vps_max_num_reorder_pics out of range: %d\n",
                   vps->vps_num_reorder_pics[i]);
            goto err;
        }
    }

    vps->vps_max_layer_id   = get_bits(gb, 6);
    vps->vps_num_layer_sets = get_ue_golomb_long(gb) + 1;
    if (vps->vps_num_layer_sets >= 1024) {
        av_log(s->avctx, AV_LOG_ERROR, "vps_num_layer_sets out of range: %d\n",
               vps->vps_num_layer_sets - 1);
        goto err;
    }
    print_cabac("vps_max_layer_id", vps->vps_max_layer_id);
    print_cabac("vps_num_layer_sets_minus1", vps->vps_num_layer_sets - 1);

    for (i = 1; i < vps->vps_num_layer_sets; i++)
        for (j = 0; j <= vps->vps_max_layer_id; j++) {
            vps->layer_id_included_flag[i][j] = (get_bits1(gb) == 1);
            print_cabac("layer_id_included_flag", vps->layer_id_included_flag[i][j]);
        }
    deriveLayerIdListVariables(vps);

    vps->vps_timing_info_present_flag = get_bits1(gb);
    print_cabac("vps_timing_info_present_flag", vps->vps_timing_info_present_flag);
    if (vps->vps_timing_info_present_flag) {
        vps->vps_num_units_in_tick               = get_bits_long(gb, 32);
        print_cabac("vps_num_units_in_tick", vps->vps_num_units_in_tick);
        vps->vps_time_scale                      = get_bits_long(gb, 32);
        print_cabac("vps_time_scale", vps->vps_time_scale);
        vps->vps_poc_proportional_to_timing_flag = get_bits1(gb);
        print_cabac("vps_poc_proportional_to_timing_flag", vps->vps_poc_proportional_to_timing_flag);
        if (vps->vps_poc_proportional_to_timing_flag) {
            vps->vps_num_ticks_poc_diff_one = get_ue_golomb_long(gb) + 1;
            print_cabac("vps_num_ticks_poc_diff_one_minus1", vps->vps_num_ticks_poc_diff_one);
        }
        vps->vps_num_hrd_parameters = get_ue_golomb_long(gb);
        print_cabac("vps_num_hrd_parameters", vps->vps_num_hrd_parameters);
        if (vps->vps_num_layer_sets >= 1024) {
            av_log(s->avctx, AV_LOG_ERROR, "vps_num_hrd_parameters out of range: %d\n",
                   vps->vps_num_layer_sets - 1);
            goto err;
        }

        for (i = 0; i < vps->vps_num_hrd_parameters; i++) {
            int common_inf_present = 1;
            vps->m_hrdOpSetIdx[i] = get_ue_golomb_long(gb);// hrd_layer_set_idx
            print_cabac("hrd_op_set_idx", vps->m_hrdOpSetIdx[i]);     
            if (i) {
                common_inf_present = get_bits1(gb);
                print_cabac("cprms_present_flag", common_inf_present); 
            }
            decode_hrd(s, common_inf_present, vps->vps_max_sub_layers);
        }
    }
    vps->vps_extension_flag = get_bits1(gb);
    print_cabac("vps_extension_flag", vps->vps_extension_flag);

    if(vps->vps_extension_flag){ // vps_extension_flag
        align_get_bits(gb);
        parse_vps_extension(s, vps);
    }

    if (s->vps_list[vps_id] &&
        !memcmp(s->vps_list[vps_id]->data, vps_buf->data, vps_buf->size)) {
        av_buffer_unref(&vps_buf);
        av_log(s->avctx, AV_LOG_DEBUG, "ignore VPS duplicated\n");
    } else {
        av_buffer_unref(&s->vps_list[vps_id]);
        s->vps_list[vps_id] = vps_buf;
    }
    return 0;

err:
    av_buffer_unref(&vps_buf);
    return AVERROR_INVALIDDATA;
}

static void decode_vui(HEVCContext *s, HEVCSPS *sps)
{
    VUI *vui          = &sps->vui;
    GetBitContext *gb = &s->HEVClc->gb;
    int sar_present;

    print_cabac("\n ---  parse vui  --- \n", s->nuh_layer_id);
    av_log(s->avctx, AV_LOG_DEBUG, "Decoding VUI\n");

    sar_present = get_bits1(gb);
    print_cabac("aspect_ratio_info_present_flag", sar_present);
    if (sar_present) {
        uint8_t sar_idx = get_bits(gb, 8);
        print_cabac("aspect_ratio_idc", sar_idx);
        if (sar_idx < FF_ARRAY_ELEMS(vui_sar))
            vui->sar = vui_sar[sar_idx];
        else if (sar_idx == 255) {
            vui->sar.num = get_bits(gb, 16);
            vui->sar.den = get_bits(gb, 16);
            print_cabac("sar_width", vui->sar.num);
            print_cabac("sar_height",  vui->sar.den);
        } else
            av_log(s->avctx, AV_LOG_WARNING,
                   "Unknown SAR index: %u.\n", sar_idx);
    }

    vui->overscan_info_present_flag = get_bits1(gb);
    print_cabac("overscan_info_present_flag", vui->overscan_info_present_flag);
    if (vui->overscan_info_present_flag){
        vui->overscan_appropriate_flag = get_bits1(gb);
        print_cabac("overscan_appropriate_flag", vui->overscan_appropriate_flag);
    }

    vui->video_signal_type_present_flag = get_bits1(gb);
    print_cabac("video_signal_type_present_flag", vui->video_signal_type_present_flag);
    if (vui->video_signal_type_present_flag) {
        vui->video_format                    = get_bits(gb, 3);
        vui->video_full_range_flag           = get_bits1(gb);
        vui->colour_description_present_flag = get_bits1(gb);
        print_cabac("video_format", vui->video_format);
        print_cabac("video_full_range_flag", vui->video_full_range_flag);
        print_cabac("colour_description_present_flag", vui->colour_description_present_flag);
        if (vui->video_full_range_flag && sps->pix_fmt == AV_PIX_FMT_YUV420P)
            sps->pix_fmt = AV_PIX_FMT_YUVJ420P;
        if (vui->colour_description_present_flag) {
            vui->colour_primaries        = get_bits(gb, 8);
            vui->transfer_characteristic = get_bits(gb, 8);
            vui->matrix_coeffs           = get_bits(gb, 8);
            print_cabac("colour_primaries", vui->colour_primaries);
            print_cabac("transfer_characteristics", vui->transfer_characteristic);
            print_cabac("matrix_coefficients", vui->matrix_coeffs);

            // Set invalid values to "unspecified"
            if (vui->colour_primaries >= AVCOL_PRI_NB)
                vui->colour_primaries = AVCOL_PRI_UNSPECIFIED;
            if (vui->transfer_characteristic >= AVCOL_TRC_NB)
                vui->transfer_characteristic = AVCOL_TRC_UNSPECIFIED;
            if (vui->matrix_coeffs >= AVCOL_SPC_NB)
                vui->matrix_coeffs = AVCOL_SPC_UNSPECIFIED;
        }
    }

    vui->chroma_loc_info_present_flag = get_bits1(gb);
    print_cabac("chroma_loc_info_present_flag", vui->chroma_loc_info_present_flag);
    if (vui->chroma_loc_info_present_flag) {
        vui->chroma_sample_loc_type_top_field    = get_ue_golomb_long(gb);
        vui->chroma_sample_loc_type_bottom_field = get_ue_golomb_long(gb);
        print_cabac("chroma_sample_loc_type_top_field", vui->chroma_sample_loc_type_top_field);
        print_cabac("chroma_sample_loc_type_bottom_field", vui->chroma_sample_loc_type_bottom_field);
    }

    vui->neutra_chroma_indication_flag = get_bits1(gb);
    vui->field_seq_flag                = get_bits1(gb);
    vui->frame_field_info_present_flag = get_bits1(gb);

    vui->default_display_window_flag = get_bits1(gb);

    print_cabac("neutral_chroma_indication_flag", vui->neutra_chroma_indication_flag);
    print_cabac("field_seq_flag", vui->field_seq_flag);
    print_cabac("frame_field_info_present_flag", vui->frame_field_info_present_flag);
    print_cabac("default_display_window_flag", vui->default_display_window_flag);
    if (vui->default_display_window_flag) {
        //TODO: * 2 is only valid for 420
        vui->def_disp_win.left_offset   = get_ue_golomb_long(gb) * 2;
        vui->def_disp_win.right_offset  = get_ue_golomb_long(gb) * 2;
        vui->def_disp_win.top_offset    = get_ue_golomb_long(gb) * 2;
        vui->def_disp_win.bottom_offset = get_ue_golomb_long(gb) * 2;
        print_cabac("def_disp_win_left_offset", vui->def_disp_win.left_offset);
        print_cabac("def_disp_win_right_offset", vui->def_disp_win.right_offset);
        print_cabac("def_disp_win_top_offset", vui->def_disp_win.top_offset);
        print_cabac("def_disp_win_bottom_offset", vui->def_disp_win.bottom_offset);

        if (s->apply_defdispwin &&
            s->avctx->flags2 & CODEC_FLAG2_IGNORE_CROP) {
            av_log(s->avctx, AV_LOG_DEBUG,
                   "discarding vui default display window, "
                   "original values are l:%u r:%u t:%u b:%u\n",
                   vui->def_disp_win.left_offset,
                   vui->def_disp_win.right_offset,
                   vui->def_disp_win.top_offset,
                   vui->def_disp_win.bottom_offset);

            vui->def_disp_win.left_offset   =
            vui->def_disp_win.right_offset  =
            vui->def_disp_win.top_offset    =
            vui->def_disp_win.bottom_offset = 0;
        }
    }

    vui->vui_timing_info_present_flag = get_bits1(gb);
    print_cabac("vui_timing_info_present_flag", vui->vui_timing_info_present_flag);
    if (vui->vui_timing_info_present_flag) {
        vui->vui_num_units_in_tick               = get_bits_long(gb, 32);
        vui->vui_time_scale                      = get_bits_long(gb, 32);
        vui->vui_poc_proportional_to_timing_flag = get_bits1(gb);

        print_cabac("vui_num_units_in_tick", vui->vui_num_units_in_tick);
        print_cabac("vui_time_scale", vui->vui_time_scale);
        print_cabac("vui_poc_proportional_to_timing_flag", vui->vui_poc_proportional_to_timing_flag);

        if (vui->vui_poc_proportional_to_timing_flag) {
            vui->vui_num_ticks_poc_diff_one_minus1 = get_ue_golomb_long(gb);
            print_cabac("vui_num_ticks_poc_diff_one_minus1", vui->vui_num_ticks_poc_diff_one_minus1);
        }
        vui->vui_hrd_parameters_present_flag = get_bits1(gb);
        print_cabac("hrd_parameters_present_flag", vui->vui_hrd_parameters_present_flag);
        if (vui->vui_hrd_parameters_present_flag)
            decode_hrd(s, 1, sps->max_sub_layers);
    }

    vui->bitstream_restriction_flag = get_bits1(gb);
    print_cabac("bitstream_restriction_flag", vui->bitstream_restriction_flag);
    if (vui->bitstream_restriction_flag) {
        vui->tiles_fixed_structure_flag              = get_bits1(gb);
        vui->motion_vectors_over_pic_boundaries_flag = get_bits1(gb);
        vui->restricted_ref_pic_lists_flag           = get_bits1(gb);
        vui->min_spatial_segmentation_idc            = get_ue_golomb_long(gb);
        vui->max_bytes_per_pic_denom                 = get_ue_golomb_long(gb);
        vui->max_bits_per_min_cu_denom               = get_ue_golomb_long(gb);
        vui->log2_max_mv_length_horizontal           = get_ue_golomb_long(gb);
        vui->log2_max_mv_length_vertical             = get_ue_golomb_long(gb);

        print_cabac("tiles_fixed_structure_flag", vui->tiles_fixed_structure_flag);
        print_cabac("motion_vectors_over_pic_boundaries_flag", vui->motion_vectors_over_pic_boundaries_flag );
        print_cabac("restricted_ref_pic_lists_flag", vui->restricted_ref_pic_lists_flag);
        print_cabac("min_spatial_segmentation_idc", vui->min_spatial_segmentation_idc);
        print_cabac("max_bytes_per_pic_denom", vui->max_bytes_per_pic_denom );
        print_cabac("max_bits_per_mincu_denom", vui->max_bits_per_min_cu_denom );
        print_cabac("log2_max_mv_length_horizontal", vui->log2_max_mv_length_horizontal );
        print_cabac("log2_max_mv_length_vertical",  vui->log2_max_mv_length_vertical);

    }
}

static void set_default_scaling_list_data(ScalingList *sl)
{
    int matrixId;

    for (matrixId = 0; matrixId < 6; matrixId++) {
        // 4x4 default is 16
        memset(sl->sl[0][matrixId], 16, 16);
        sl->sl_dc[0][matrixId] = 16; // default for 16x16
        sl->sl_dc[1][matrixId] = 16; // default for 32x32
    }
    memcpy(sl->sl[1][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[1][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[1][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[1][5], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[2][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[2][5], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][0], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][1], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][2], default_scaling_list_intra, 64);
    memcpy(sl->sl[3][3], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][4], default_scaling_list_inter, 64);
    memcpy(sl->sl[3][5], default_scaling_list_inter, 64);
}

static int scaling_list_data(HEVCContext *s, ScalingList *sl, HEVCSPS *sps)
{
    GetBitContext *gb = &s->HEVClc->gb;
    uint8_t scaling_list_pred_mode_flag;
    int32_t scaling_list_dc_coef[2][6];
    int size_id, matrix_id, pos;
    int i;

    for (size_id = 0; size_id < 4; size_id++)
        for (matrix_id = 0; matrix_id < 6; matrix_id += ((size_id == 3) ? 3 : 1)) {
            scaling_list_pred_mode_flag = get_bits1(gb);
            if (!scaling_list_pred_mode_flag) {
                unsigned int delta = get_ue_golomb_long(gb);
                /* Only need to handle non-zero delta. Zero means default,
                 * which should already be in the arrays. */
                if (delta) {
                    // Copy from previous array.
                    if (matrix_id < delta) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Invalid delta in scaling list data: %d.\n", delta);
                        return AVERROR_INVALIDDATA;
                    }

                    memcpy(sl->sl[size_id][matrix_id],
                           sl->sl[size_id][matrix_id - delta],
                           size_id > 0 ? 64 : 16);
                    if (size_id > 1)
                        sl->sl_dc[size_id - 2][matrix_id] = sl->sl_dc[size_id - 2][matrix_id - delta];
                }
            } else {
                int next_coef, coef_num;
                int32_t scaling_list_delta_coef;

                next_coef = 8;
                coef_num  = FFMIN(64, 1 << (4 + (size_id << 1)));
                if (size_id > 1) {
                    scaling_list_dc_coef[size_id - 2][matrix_id] = get_se_golomb(gb) + 8;
                    next_coef = scaling_list_dc_coef[size_id - 2][matrix_id];
                    sl->sl_dc[size_id - 2][matrix_id] = next_coef;
                }
                for (i = 0; i < coef_num; i++) {
                    if (size_id == 0)
                        pos = 4 * ff_hevc_diag_scan4x4_y[i] +
                                  ff_hevc_diag_scan4x4_x[i];
                    else
                        pos = 8 * ff_hevc_diag_scan8x8_y[i] +
                                  ff_hevc_diag_scan8x8_x[i];

                    scaling_list_delta_coef = get_se_golomb(gb);
                    next_coef = (next_coef + scaling_list_delta_coef + 256) % 256;
                    sl->sl[size_id][matrix_id][pos] = next_coef;
                }
            }
        }

    if (sps->chroma_format_idc == 3) {
        for (i = 0; i < 64; i++) {
            sl->sl[3][1][i] = sl->sl[2][1][i];
            sl->sl[3][2][i] = sl->sl[2][2][i];
            sl->sl[3][4][i] = sl->sl[2][4][i];
            sl->sl[3][5][i] = sl->sl[2][5][i];
        }
        sl->sl_dc[1][1] = sl->sl_dc[0][1];
        sl->sl_dc[1][2] = sl->sl_dc[0][2];
        sl->sl_dc[1][4] = sl->sl_dc[0][4];
        sl->sl_dc[1][5] = sl->sl_dc[0][5];
    }


    return 0;
}

int ff_hevc_decode_nal_sps(HEVCContext *s)
{
    const AVPixFmtDescriptor *desc;
    GetBitContext *gb = &s->HEVClc->gb;
    int ret    = 0;
    int sps_id = 0;
    int log2_diff_max_min_transform_block_size;
    int bit_depth_chroma, start, vui_present, sublayer_ordering_info;
    int i;
    print_cabac(" \n --- parse sps --- \n ", s->nuh_layer_id);
    HEVCSPS *sps;
    HEVCVPS *vps;
    AVBufferRef *sps_buf = av_buffer_allocz(sizeof(*sps));
    uint8_t v1_compatible = 1;

    if ( !sps_buf )
        return AVERROR(ENOMEM);
    sps = (HEVCSPS*)sps_buf->data;
    av_log(s->avctx, AV_LOG_DEBUG, "Decoding SPS\n");

    // Coded parameters

    sps->vps_id = get_bits(gb, 4);
    print_cabac("sps_video_parameter_set_id", sps->vps_id); 
    if (!s->vps_list[sps->vps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "VPS %d does not exist\n",
               sps->vps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    vps = ((HEVCVPS*)s->vps_list[sps->vps_id]->data);
    if (!s->nuh_layer_id) {
        sps->max_sub_layers = get_bits(gb, 3) + 1;
        print_cabac("sps_max_sub_layers_minus1", sps->max_sub_layers - 1);
        if (sps->max_sub_layers > MAX_SUB_LAYERS) {
            av_log(s->avctx, AV_LOG_ERROR, "sps_max_sub_layers out of range: %d\n",
                   sps->max_sub_layers);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
    } else {
        uint8_t sps_ext_or_max_sub_layers = 0;//get_bits(gb, 3) + 1;
        print_cabac("sps_ext_or_max_sub_layers_minus1", sps_ext_or_max_sub_layers - 1);
        if (sps_ext_or_max_sub_layers - 1 == 7) {
            sps->max_sub_layers = vps->vps_max_sub_layers;
        } else {
            sps->max_sub_layers = vps->vps_max_sub_layers;
            //FIXME sps->max_sub_layers = sps_ext_or_max_sub_layers;
            v1_compatible = 0;
        }
    }

    if (v1_compatible) {
        sps->m_bTemporalIdNestingFlag = get_bits1(gb); // temporal_id_nesting_flag
        print_cabac("sps_temporal_id_nesting_flag", sps->m_bTemporalIdNestingFlag);
        parse_ptl(s, &sps->ptl, sps->max_sub_layers);
    } else {
        if (sps->max_sub_layers > 1)
            sps->m_bTemporalIdNestingFlag = vps->vps_temporal_id_nesting_flag;
        else
            sps->m_bTemporalIdNestingFlag =1;
    }

    sps_id = get_ue_golomb_long(gb);

    print_cabac("sps_seq_parameter_set_id", sps_id);
    if (sps_id >= MAX_SPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "SPS id out of range: %d\n", sps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    if (s->nuh_layer_id > 0) {
        sps->update_rep_format_flag = get_bits1(gb);
        print_cabac("update_rep_format_flag", sps->update_rep_format_flag);
    } else {
        sps->update_rep_format_flag = 1;
    }
    if (v1_compatible) {
        sps->chroma_format_idc = get_ue_golomb_long(gb);
        print_cabac("chroma_format_idc", sps->chroma_format_idc);
        if (!(sps->chroma_format_idc == 0 || sps->chroma_format_idc == 1 || sps->chroma_format_idc == 2 || sps->chroma_format_idc == 3)) {
            avpriv_report_missing_feature(s->avctx, "chroma_format_idc != {0, 1, 2, 3}\n");
            ret = AVERROR_PATCHWELCOME;
            goto err;
        }

        if (sps->chroma_format_idc == 3)
            sps->separate_colour_plane_flag = get_bits1(gb);

       if (sps->separate_colour_plane_flag)
            sps->chroma_array_type = 0;
       else
            sps->chroma_array_type = sps->chroma_format_idc;

        sps->width  = get_ue_golomb_long(gb);
        sps->height = get_ue_golomb_long(gb);

        print_cabac("pic_width_in_luma_samples", sps->width);
        print_cabac("pic_height_in_luma_samples", sps->height);
        if ((ret = av_image_check_size(sps->width,
                sps->height, 0, s->avctx)) < 0)
            goto err;
    } else if (sps->update_rep_format_flag) {
        int update_rep_format = get_bits1(gb);
        print_cabac("update_rep_format", update_rep_format);
        sps->chroma_array_type = sps->chroma_format_idc = 0;
        if (update_rep_format) {
            sps->update_rep_format_index = get_bits(gb, 8);
            print_cabac("update_rep_format_index", sps->update_rep_format_index);
        }
    }
    int conformance_window_flag = get_bits1(gb);
    print_cabac("conformance_window_flag", conformance_window_flag);
    if (conformance_window_flag) { // pic_conformance_flag
        //TODO: * 2 is only valid for 420
        sps->pic_conf_win.left_offset   = get_ue_golomb_long(gb) * 2;
        sps->pic_conf_win.right_offset  = get_ue_golomb_long(gb) * 2;
        sps->pic_conf_win.top_offset    = get_ue_golomb_long(gb) * 2;
        sps->pic_conf_win.bottom_offset = get_ue_golomb_long(gb) * 2;

        print_cabac("conf_win_left_offset", sps->pic_conf_win.left_offset);
        print_cabac("conf_win_right_offset", sps->pic_conf_win.right_offset);
        print_cabac("conf_win_top_offset", sps->pic_conf_win.top_offset );
        print_cabac("conf_win_bottom_offset", sps->pic_conf_win.bottom_offset);

        if (s->avctx->flags2 & CODEC_FLAG2_IGNORE_CROP) {
            av_log(s->avctx, AV_LOG_DEBUG,
                   "discarding sps conformance window, "
                   "original values are l:%u r:%u t:%u b:%u\n",
                   sps->pic_conf_win.left_offset,
                   sps->pic_conf_win.right_offset,
                   sps->pic_conf_win.top_offset,
                   sps->pic_conf_win.bottom_offset);

            sps->pic_conf_win.left_offset   =
            sps->pic_conf_win.right_offset  =
            sps->pic_conf_win.top_offset    =
            sps->pic_conf_win.bottom_offset = 0;
        }
        sps->output_window = sps->pic_conf_win;
    }

    if (v1_compatible) {
        sps->bit_depth   = get_ue_golomb_long(gb) + 8;
        print_cabac("bit_depth_luma_minus8", sps->bit_depth - 8);
        bit_depth_chroma = get_ue_golomb_long(gb) + 8;
        print_cabac("bit_depth_chroma_minus8", bit_depth_chroma - 8);
        if (sps->chroma_format_idc && bit_depth_chroma != sps->bit_depth) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Luma bit depth (%d) is different from chroma bit depth (%d), "
                   "this is unsupported.\n",
                   sps->bit_depth, bit_depth_chroma);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
    }
    if (v1_compatible) {
        switch (sps->bit_depth) {
        case 8:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY8;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P;
            break;
        case 9:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P9;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P9;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P9;
            break;
        case 10:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P10;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P10;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P10;
            break;
        case 12:
            if (sps->chroma_format_idc == 0) sps->pix_fmt = AV_PIX_FMT_GRAY16;
            if (sps->chroma_format_idc == 1) sps->pix_fmt = AV_PIX_FMT_YUV420P12;
            if (sps->chroma_format_idc == 2) sps->pix_fmt = AV_PIX_FMT_YUV422P12;
            if (sps->chroma_format_idc == 3) sps->pix_fmt = AV_PIX_FMT_YUV444P12;
            break;
        default:
            av_log(s->avctx, AV_LOG_ERROR,
                   "4:0:0, 4:2:0, 4:2:2, 4:4:4 supports are currently specified for 8, 10 and 12bits.\n");
            return AVERROR_PATCHWELCOME;
        }
    } else {
        RepFormat Rep;
        if (sps->update_rep_format_flag)
            Rep = vps->vps_rep_format[sps->update_rep_format_index];
        else {
            if (vps->vps_num_rep_formats > 1)
                Rep = vps->vps_rep_format[vps->vps_rep_format_idx[s->nuh_layer_id]];
            else
                Rep = vps->vps_rep_format[0];
        }
        sps->width  = Rep.pic_width_vps_in_luma_samples;
        sps->height = Rep.pic_height_vps_in_luma_samples;
        sps->bit_depth   = Rep.bit_depth_vps_luma;
        sps->chroma_array_type = Rep.chroma_format_vps_idc;
        //sps->bit_depth_chroma = Rep.m_bitDepthVpsChroma;

        if(Rep.chroma_format_vps_idc) {
            switch (Rep.bit_depth_vps_luma) {
            case 8:  sps->pix_fmt = AV_PIX_FMT_YUV420P;   break;
            case 9:  sps->pix_fmt = AV_PIX_FMT_YUV420P9;  break;
            case 10: sps->pix_fmt = AV_PIX_FMT_YUV420P10; break;
            default:
                av_log(s->avctx, AV_LOG_ERROR, "-- Unsupported bit depth: %d\n",
                        sps->bit_depth);
                ret = AVERROR_PATCHWELCOME;
                goto err;
            }
        } else {
            av_log(s->avctx, AV_LOG_ERROR,
                    "non-4:2:0 support is currently unspecified.\n");
            return AVERROR_PATCHWELCOME;
        }
    }

    desc = av_pix_fmt_desc_get(sps->pix_fmt);
    if (!desc) {
        ret = AVERROR(EINVAL);
        goto err;
    }
    sps->hshift[0] = sps->vshift[0] = 0;
    sps->hshift[2] = sps->hshift[1] = desc->log2_chroma_w;
    sps->vshift[2] = sps->vshift[1] = desc->log2_chroma_h;
    sps->pixel_shift = sps->bit_depth > 8;
    sps->log2_max_poc_lsb = get_ue_golomb_long(gb) + 4;
    print_cabac("log2_max_pic_order_cnt_lsb_minus4", sps->log2_max_poc_lsb-4);
    if (sps->log2_max_poc_lsb > 16) {
        av_log(s->avctx, AV_LOG_ERROR, "log2_max_pic_order_cnt_lsb_minus4 out range: %d\n",
               sps->log2_max_poc_lsb - 4);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }

    if (v1_compatible) {
        sublayer_ordering_info = get_bits1(gb);
        print_cabac("sublayer_ordering_info", sublayer_ordering_info);
        start = sublayer_ordering_info ? 0 : sps->max_sub_layers - 1;
        for (i = start; i < sps->max_sub_layers; i++) {
            sps->temporal_layer[i].max_dec_pic_buffering = get_ue_golomb_long(gb) + 1;
            sps->temporal_layer[i].num_reorder_pics      = get_ue_golomb_long(gb);
            sps->temporal_layer[i].max_latency_increase  = get_ue_golomb_long(gb) - 1;

            print_cabac("sps_max_dec_pic_buffering_minus1", sps->temporal_layer[i].max_dec_pic_buffering - 1);
            print_cabac("sps_num_reorder_pics", sps->temporal_layer[i].num_reorder_pics);
            print_cabac("sps_max_latency_increase_plus1", sps->temporal_layer[i].max_latency_increase + 1);

            if (sps->temporal_layer[i].max_dec_pic_buffering > MAX_DPB_SIZE) {
                av_log(s->avctx, AV_LOG_ERROR, "sps_max_dec_pic_buffering_minus1 out of range: %d\n",
                       sps->temporal_layer[i].max_dec_pic_buffering - 1);
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            if (sps->temporal_layer[i].num_reorder_pics > sps->temporal_layer[i].max_dec_pic_buffering - 1) {
                av_log(s->avctx, AV_LOG_ERROR, "sps_max_num_reorder_pics out of range: %d\n",
                       sps->temporal_layer[i].num_reorder_pics);
                if (sps->temporal_layer[i].num_reorder_pics > MAX_DPB_SIZE - 1) {
                    ret = AVERROR_INVALIDDATA;
                    goto err;
                }
                sps->temporal_layer[i].max_dec_pic_buffering = sps->temporal_layer[i].num_reorder_pics + 1;
            }
        }
        if (!sublayer_ordering_info) {
            for (i = 0; i < start; i++) {
                sps->temporal_layer[i].max_dec_pic_buffering = sps->temporal_layer[start].max_dec_pic_buffering;
                sps->temporal_layer[i].num_reorder_pics      = sps->temporal_layer[start].num_reorder_pics;
                sps->temporal_layer[i].max_latency_increase  = sps->temporal_layer[start].max_latency_increase;
            }
        }
    } else {
        for (i = 0; i < sps->max_sub_layers; i++) {
            sps->temporal_layer[i].max_dec_pic_buffering = vps->max_vps_dec_pic_buffering_minus1[1][0] - 1;
            sps->temporal_layer[i].num_reorder_pics      = vps->max_vps_num_reorder_pics[1][0];
            sps->temporal_layer[i].max_latency_increase  = vps->max_vps_latency_increase_plus1[1][0] + 1;
        }
    }


    sps->log2_min_cb_size                    = get_ue_golomb_long(gb) + 3;
    sps->log2_diff_max_min_coding_block_size = get_ue_golomb_long(gb);
    sps->log2_min_tb_size                    = get_ue_golomb_long(gb) + 2;
    log2_diff_max_min_transform_block_size   = get_ue_golomb_long(gb);
    sps->log2_max_trafo_size                 = log2_diff_max_min_transform_block_size +
                                               sps->log2_min_tb_size;

    print_cabac("log2_min_coding_block_size_minus3", sps->log2_min_cb_size-3);
    print_cabac("log2_diff_max_min_coding_block_size", sps->log2_diff_max_min_coding_block_size);
    print_cabac("log2_min_transform_block_size_minus2", sps->log2_min_tb_size  -2);
    print_cabac("log2_diff_max_min_transform_block_size", log2_diff_max_min_transform_block_size);

    if (sps->log2_min_tb_size >= sps->log2_min_cb_size) {
        av_log(s->avctx, AV_LOG_ERROR, "Invalid value for log2_min_tb_size");
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    sps->max_transform_hierarchy_depth_inter = get_ue_golomb_long(gb);
    sps->max_transform_hierarchy_depth_intra = get_ue_golomb_long(gb);

    sps->scaling_list_enable_flag = get_bits1(gb);

    print_cabac("max_transform_hierarchy_depth_inter", sps->max_transform_hierarchy_depth_inter);
    print_cabac("max_transform_hierarchy_depth_intra", sps->max_transform_hierarchy_depth_intra);
    print_cabac("scaling_list_enabled_flag", sps->scaling_list_enable_flag);

    if (sps->scaling_list_enable_flag) {
        if (s->nuh_layer_id > 0) {
            sps->m_inferScalingListFlag =  get_bits1(gb);
            print_cabac("sps_infer_scaling_list_flag\n", sps->m_inferScalingListFlag);
        }
        if (sps->m_inferScalingListFlag) {
            sps->m_scalingListRefLayerId = get_ue_golomb_long(gb);
            print_cabac("sps_scaling_list_ref_layer_id\n", sps->m_scalingListRefLayerId);
            sps->scaling_list_enable_flag = 0;
        } else {
            set_default_scaling_list_data(&sps->scaling_list);
            sps->m_scalingListPresentFlag = get_bits1(gb);
            print_cabac("sps_scaling_list_data_present_flag", sps->m_scalingListPresentFlag);
            if (sps->m_scalingListPresentFlag) {
                ret = scaling_list_data(s, &sps->scaling_list, sps);
                if (ret < 0)
                    goto err;
            }
        }
    }
    sps->amp_enabled_flag = get_bits1(gb);
    sps->sao_enabled      = get_bits1(gb);

    print_cabac("amp_enabled_flag", sps->amp_enabled_flag);
    print_cabac("sample_adaptive_offset_enabled_flag", sps->sao_enabled);

    if (sps->sao_enabled)
        av_log(s->avctx, AV_LOG_DEBUG, "SAO enabled\n");


    sps->pcm_enabled_flag = get_bits1(gb);
    print_cabac("pcm_enabled_flag", sps->pcm_enabled_flag);
    if (sps->pcm_enabled_flag) {
        sps->pcm.bit_depth   = get_bits(gb, 4) + 1;
        sps->pcm.bit_depth_chroma = get_bits(gb, 4) + 1;
        sps->pcm.log2_min_pcm_cb_size = get_ue_golomb_long(gb) + 3;
        sps->pcm.log2_max_pcm_cb_size = sps->pcm.log2_min_pcm_cb_size +
                                        get_ue_golomb_long(gb);
        if (sps->pcm.bit_depth > sps->bit_depth) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "PCM bit depth (%d) is greater than normal bit depth (%d)\n",
                   sps->pcm.bit_depth, sps->bit_depth);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
        sps->pcm.loop_filter_disable_flag = get_bits1(gb);
        print_cabac("pcm_sample_bit_depth_luma_minus1", sps->pcm.bit_depth-1);
        print_cabac("pcm_sample_bit_depth_chroma_minus1", sps->pcm.bit_depth_chroma-1);
        print_cabac("log2_min_pcm_luma_coding_block_size_minus3", sps->pcm.log2_min_pcm_cb_size-3);
        print_cabac("log2_diff_max_min_pcm_luma_coding_block_size", sps->pcm.log2_max_pcm_cb_size-sps->pcm.log2_min_pcm_cb_size);
        print_cabac("pcm_loop_filter_disable_flag", sps->pcm.loop_filter_disable_flag);
    }

    sps->nb_st_rps = get_ue_golomb_long(gb);
    print_cabac("num_short_term_ref_pic_sets", sps->nb_st_rps);

    if (sps->nb_st_rps > MAX_SHORT_TERM_RPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "Too many short term RPS: %d.\n",
               sps->nb_st_rps);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    for (i = 0; i < sps->nb_st_rps; i++) {
        if ((ret = ff_hevc_decode_short_term_rps(s, &sps->st_rps[i],
                                                 sps, 0)) < 0)
            goto err;
    }

    sps->long_term_ref_pics_present_flag = get_bits1(gb);
    print_cabac("long_term_ref_pics_present_flag", sps->long_term_ref_pics_present_flag);
    if (sps->long_term_ref_pics_present_flag) {
        sps->num_long_term_ref_pics_sps = get_ue_golomb_long(gb);
        print_cabac("long_term_ref_pics_present_flag", sps->num_long_term_ref_pics_sps);
        for (i = 0; i < sps->num_long_term_ref_pics_sps; i++) {
            sps->lt_ref_pic_poc_lsb_sps[i]       = get_bits(gb, sps->log2_max_poc_lsb);
            sps->used_by_curr_pic_lt_sps_flag[i] = get_bits1(gb);
            print_cabac("lt_ref_pic_poc_lsb_sps", sps->lt_ref_pic_poc_lsb_sps[i]);
            print_cabac("used_by_curr_pic_lt_sps_flag", sps->used_by_curr_pic_lt_sps_flag[i]);
        }
    }

    sps->sps_temporal_mvp_enabled_flag          = get_bits1(gb);
    print_cabac("sps_temporal_mvp_enable_flag", sps->sps_temporal_mvp_enabled_flag);

    if(s->nuh_layer_id > 0)
        sps->set_mfm_enabled_flag = 1;
    else
        sps->set_mfm_enabled_flag = 0;

    sps->sps_strong_intra_smoothing_enable_flag = get_bits1(gb);
    sps->vui.sar = (AVRational){0, 1};
    print_cabac("sps_strong_intra_smoothing_enable_flag", sps->sps_strong_intra_smoothing_enable_flag);
    vui_present = get_bits1(gb);
    print_cabac("vui_parameters_present_flag", vui_present);
    if (vui_present)
        decode_vui(s, sps);

    if (get_bits1(gb)) { // sps_extension_flag
        int sps_extension_flag[2];
        int sps_extension_6bits;
        for (i = 0; i < 2; i++)
            sps_extension_flag[i] = get_bits1(gb);
        sps_extension_6bits	= get_bits(gb, 6);
        if (sps_extension_flag[0]) {
            int extended_precision_processing_flag;
            int high_precision_offsets_enabled_flag;
            int cabac_bypass_alignment_enabled_flag;

            sps->transform_skip_rotation_enabled_flag = get_bits1(gb);
            print_cabac("transform_skip_rotation_enabled_flag ", sps->transform_skip_rotation_enabled_flag);
            sps->transform_skip_context_enabled_flag  = get_bits1(gb);
            print_cabac("transform_skip_context_enabled_flag ", sps->transform_skip_context_enabled_flag);
            sps->implicit_rdpcm_enabled_flag = get_bits1(gb);
            print_cabac("implicit_rdpcm_enabled_flag ", sps->implicit_rdpcm_enabled_flag);

            sps->explicit_rdpcm_enabled_flag = get_bits1(gb);
            print_cabac("explicit_rdpcm_enabled_flag ", sps->explicit_rdpcm_enabled_flag);

            extended_precision_processing_flag = get_bits1(gb);
            if (extended_precision_processing_flag)
                av_log(s->avctx, AV_LOG_WARNING,
                   "extended_precision_processing_flag not yet implemented\n");

            print_cabac("extended_precision_processing_flag ", extended_precision_processing_flag);
            sps->intra_smoothing_disabled_flag       = get_bits1(gb);
            print_cabac("intra_smoothing_disabled_flag ", sps->intra_smoothing_disabled_flag);
            high_precision_offsets_enabled_flag  = get_bits1(gb);
            if (high_precision_offsets_enabled_flag)
                av_log(s->avctx, AV_LOG_WARNING,
                   "high_precision_offsets_enabled_flag not yet implemented\n");

            print_cabac("high_precision_offsets_enabled_flag ", high_precision_offsets_enabled_flag);
            sps->persistent_rice_adaptation_enabled_flag = get_bits1(gb);
            print_cabac("persistent_rice_adaptation_enabled_flag ", sps->persistent_rice_adaptation_enabled_flag);

            cabac_bypass_alignment_enabled_flag  = get_bits1(gb);
            print_cabac("cabac_bypass_alignment_enabled_flag ", cabac_bypass_alignment_enabled_flag);
            if (cabac_bypass_alignment_enabled_flag)
                av_log(s->avctx, AV_LOG_WARNING,
                   "cabac_bypass_alignment_enabled_flag not yet implemented\n");
        }
        if (sps_extension_flag[1]) {
            uint8_t inter_view_mv_vert_constraint_flag = get_bits1(gb);
            print_cabac("inter_view_mv_vert_constraint_flag",  inter_view_mv_vert_constraint_flag);
        }
    }
    if (s->apply_defdispwin) {
        sps->output_window.left_offset   += sps->vui.def_disp_win.left_offset;
        sps->output_window.right_offset  += sps->vui.def_disp_win.right_offset;
        sps->output_window.top_offset    += sps->vui.def_disp_win.top_offset;
        sps->output_window.bottom_offset += sps->vui.def_disp_win.bottom_offset;
    }
    if (sps->output_window.left_offset & (0x1F >> (sps->pixel_shift)) &&
        !(s->avctx->flags & CODEC_FLAG_UNALIGNED)) {
        sps->output_window.left_offset &= ~(0x1F >> (sps->pixel_shift));
        av_log(s->avctx, AV_LOG_WARNING, "Reducing left output window to %d "
               "chroma samples to preserve alignment.\n",
               sps->output_window.left_offset);
    }
    sps->output_width  = sps->width -
                         (sps->output_window.left_offset + sps->output_window.right_offset);
    sps->output_height = sps->height -
                         (sps->output_window.top_offset + sps->output_window.bottom_offset);
    if (sps->output_width <= 0 || sps->output_height <= 0) {
        av_log(s->avctx, AV_LOG_WARNING, "Invalid visible frame dimensions: %dx%d.\n",
               sps->output_width, sps->output_height);
        if (s->avctx->err_recognition & AV_EF_EXPLODE) {
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
        av_log(s->avctx, AV_LOG_WARNING,
               "Displaying the whole video surface.\n");
        sps->pic_conf_win.left_offset   =
        sps->pic_conf_win.right_offset  =
        sps->pic_conf_win.top_offset    =
        sps->pic_conf_win.bottom_offset = 0;
        sps->output_width               = sps->width;
        sps->output_height              = sps->height;
    }

    // Inferred parameters
    sps->log2_ctb_size = sps->log2_min_cb_size +
                         sps->log2_diff_max_min_coding_block_size;
    sps->log2_min_pu_size = sps->log2_min_cb_size - 1;

    sps->ctb_width  = (sps->width  + (1 << sps->log2_ctb_size) - 1) >> sps->log2_ctb_size;
    sps->ctb_height = (sps->height + (1 << sps->log2_ctb_size) - 1) >> sps->log2_ctb_size;
    sps->ctb_size   = sps->ctb_width * sps->ctb_height;

    sps->min_cb_width  = sps->width  >> sps->log2_min_cb_size;
    sps->min_cb_height = sps->height >> sps->log2_min_cb_size;
    sps->min_tb_width  = sps->width  >> sps->log2_min_tb_size;
    sps->min_tb_height = sps->height >> sps->log2_min_tb_size;
    sps->min_pu_width  = sps->width  >> sps->log2_min_pu_size;
    sps->min_pu_height = sps->height >> sps->log2_min_pu_size;
    sps->tb_mask       = (1 << (sps->log2_ctb_size - sps->log2_min_tb_size) ) - 1;

    sps->qp_bd_offset = 6 * (sps->bit_depth - 8);

    if (sps->width  & ((1 << sps->log2_min_cb_size) - 1) ||
        sps->height & ((1 << sps->log2_min_cb_size) - 1)) {
        av_log(s->avctx, AV_LOG_ERROR, "Invalid coded frame dimensions.\n");
        goto err;
    }

    if (sps->log2_ctb_size > MAX_LOG2_CTB_SIZE) {
        av_log(s->avctx, AV_LOG_ERROR, "CTB size out of range: 2^%d\n", sps->log2_ctb_size);
        goto err;
    }
    if (sps->max_transform_hierarchy_depth_inter > sps->log2_ctb_size - sps->log2_min_tb_size) {
        av_log(s->avctx, AV_LOG_ERROR, "max_transform_hierarchy_depth_inter out of range: %d\n",
               sps->max_transform_hierarchy_depth_inter);
        goto err;
    }
    if (sps->max_transform_hierarchy_depth_intra > sps->log2_ctb_size - sps->log2_min_tb_size) {
        av_log(s->avctx, AV_LOG_ERROR, "max_transform_hierarchy_depth_intra out of range: %d\n",
               sps->max_transform_hierarchy_depth_intra);
        goto err;
    }
    if (sps->log2_max_trafo_size > FFMIN(sps->log2_ctb_size, 5)) {
        av_log(s->avctx, AV_LOG_ERROR,
               "max transform block size out of range: %d\n",
               sps->log2_max_trafo_size);
        goto err;
    }

    if (s->avctx->debug & FF_DEBUG_BITSTREAM) {
        av_log(s->avctx, AV_LOG_DEBUG,
               "Parsed SPS: id %d; coded wxh: %dx%d; "
               "cropped wxh: %dx%d; pix_fmt: %s.\n",
               sps_id, sps->width, sps->height,
               sps->output_width, sps->output_height,
               av_get_pix_fmt_name(sps->pix_fmt));
    }

    /* check if this is a repeat of an already parsed SPS, then keep the
     * original one.
     * otherwise drop all PPSes that depend on it */
    if (s->sps_list[sps_id] &&
        !memcmp(s->sps_list[sps_id]->data, sps_buf->data, sps_buf->size)) {
        av_buffer_unref(&sps_buf);
    } else {
        av_buffer_unref(&s->sps_list[sps_id]);
        s->sps_list[sps_id] = sps_buf;
    }
    return 0;

err:
    av_buffer_unref(&sps_buf);
    return ret;
}

static void hevc_pps_free(void *opaque, uint8_t *data)
{
    HEVCPPS *pps = (HEVCPPS*)data;

    av_freep(&pps->column_width);
    av_freep(&pps->row_height);
    av_freep(&pps->col_idxX);
    av_freep(&pps->ctb_addr_rs_to_ts);
    av_freep(&pps->ctb_addr_ts_to_rs);
    av_freep(&pps->tile_pos_rs);
    av_freep(&pps->tile_id);
    av_freep(&pps->min_tb_addr_zs_tab);

    av_freep(&pps);
}

static int pps_range_extensions(HEVCContext *s, HEVCPPS *pps, HEVCSPS *sps) {
    GetBitContext *gb = &s->HEVClc->gb;
    int i;

    if (pps->transform_skip_enabled_flag) {
        pps->log2_max_transform_skip_block_size = get_ue_golomb_long(gb) + 2;
        print_cabac("log2_max_transform_skip_block_size", pps->log2_max_transform_skip_block_size);
        if (pps->log2_max_transform_skip_block_size > 2) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "log2_max_transform_skip_block_size_minus2 is partially implemented.\n");
        }
    }
    pps->cross_component_prediction_enabled_flag = get_bits1(gb);
    print_cabac("cross_component_prediction_enabled_flag", pps->cross_component_prediction_enabled_flag);
    pps->chroma_qp_offset_list_enabled_flag = get_bits1(gb);
    print_cabac("chroma_qp_offset_list_enabled_flag", pps->chroma_qp_offset_list_enabled_flag);
    if (pps->chroma_qp_offset_list_enabled_flag) {
        av_log(s->avctx, AV_LOG_ERROR,
               "chroma_qp_offset_list_enabled_flag is not yet implemented.\n");
    }
    if (pps->chroma_qp_offset_list_enabled_flag) {
        pps->diff_cu_chroma_qp_offset_depth = get_ue_golomb_long(gb);
        print_cabac("diff_cu_chroma_qp_offset_depth", pps->diff_cu_chroma_qp_offset_depth);
        if (pps->diff_cu_chroma_qp_offset_depth) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "diff_cu_chroma_qp_offset_depths is not yet implemented.\n");
        }
        pps->chroma_qp_offset_list_len_minus1 = get_ue_golomb_long(gb);
        print_cabac("chroma_qp_offset_list_len_minus1", pps->chroma_qp_offset_list_len_minus1);
        if (pps->chroma_qp_offset_list_len_minus1 && pps->chroma_qp_offset_list_len_minus1 >= 5) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "chroma_qp_offset_list_len_minus1 shall be in the range [0, 5].\n");
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i <= pps->chroma_qp_offset_list_len_minus1; i++) {
            pps->cb_qp_offset_list[i] = get_se_golomb_long(gb);
            print_cabac("cb_qp_offset_list", pps->cb_qp_offset_list[i]);
            if (pps->cb_qp_offset_list[i]) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "cb_qp_offset_list is not yet implemented.\n");
            }
            pps->cr_qp_offset_list[i] = get_se_golomb_long(gb);
            print_cabac("cr_qp_offset_list", pps->cr_qp_offset_list[i]);
            if (pps->cr_qp_offset_list[i]) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "cr_qp_offset_list is not yet implemented.\n");
            }
        }
    }
    pps->log2_sao_offset_scale_luma = get_ue_golomb_long(gb);
    print_cabac("log2_sao_offset_scale_luma", pps->log2_sao_offset_scale_luma);
    if (pps->log2_sao_offset_scale_luma && pps->log2_sao_offset_scale_luma > sps->bit_depth - 10) {
        av_log(s->avctx, AV_LOG_ERROR,
               "log2_sao_offset_scale_luma must be in range [0, %d]\n", sps->bit_depth - 10);
    }
    pps->log2_sao_offset_scale_chroma = get_ue_golomb_long(gb);
    print_cabac("log2_sao_offset_scale_chroma", pps->log2_sao_offset_scale_chroma);
    if (pps->log2_sao_offset_scale_luma && pps->log2_sao_offset_scale_luma > sps->bit_depth - 10) {
        av_log(s->avctx, AV_LOG_ERROR,
               "log2_sao_offset_scale_luma must be in range [0, %d]\n", sps->bit_depth - 10);
    }

    return(0);
}

static int pps_multilayer_extensions(HEVCContext *s, HEVCPPS *pps, HEVCSPS *sps) {
    GetBitContext *gb = &s->HEVClc->gb;
    int i;

    uint8_t poc_reset_info_present_flag = get_bits1(gb);
    print_cabac("poc_reset_info_present_flag", poc_reset_info_present_flag);
/*pps_infer_scaling_list_flag	u(1)
if( pps_infer_scaling_list_flag )
pps_scaling_list_ref_layer_id	u(6)
num_ref_loc_offsets	ue(v)
for( i = 0; i < num_ref_loc_offsets; i++) { [Ed. (JC): can insert a new sub-clause for all the offsets]
    ref_loc_offset_layer_id[ i ]	u(6)
    scaled_ref_layer_offset_present_flag[ i ]	u(1)
    if(scaled_ref_layer_offset_present_flag[ i ]) {
        scaled_ref_layer_left_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_top_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_right_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        scaled_ref_layer_bottom_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
    }
    ref_region_offset_present_flag[ i ]	u(1)
    if(ref_region_offset_present_flag[ i ]) {
        ref_region_left_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_top_offset[ ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_right_offset[ref_loc_offset_layer_id[ i ] ]	se(v)
        ref_region_bottom_offset[ref_loc_offset_layer_id[ i ] ]	se(v)
    }
    resample_phase_set_present_flag[ i ]	u(1)
    if(resample_phase_set_prsent_flag[ i ]) {
        phase_hor_luma[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_ver_luma[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_hor_chroma_plus8[ ref_loc_offset_layer_id[ i ] ]	ue(v)
        phase_ver_chroma_plus8[ ref_loc_offset_layer_id[ i ] ]	ue(v)
    }
}*/
    uint8_t colour_mapping_enabled_flag = get_bits1(gb);
    print_cabac("colour_mapping_enabled_flag", colour_mapping_enabled_flag);
    if (colour_mapping_enabled_flag)
        av_log(s->avctx, AV_LOG_ERROR,
               "colour_mapping_table() not yet implemented\n");


    return(0);
}

int ff_hevc_decode_nal_pps(HEVCContext *s)
{
    GetBitContext *gb = &s->HEVClc->gb;
    HEVCSPS      *sps = NULL;
    unsigned int *col_bd;
    unsigned int *row_bd;
    int pic_area_in_ctbs;
    int log2_diff_ctb_min_tb_size;
    int i, j, x, y, ctb_addr_rs, tile_id;
    int ret    = 0;
    int pps_id = 0;

    AVBufferRef *pps_buf;
    HEVCPPS *pps = av_mallocz(sizeof(*pps));
    print_cabac(" --- parse pps --- ", s->nuh_layer_id);
    if (!pps)
        return AVERROR(ENOMEM);

    pps_buf = av_buffer_create((uint8_t *)pps, sizeof(*pps),
                               hevc_pps_free, NULL, 0);
    if (!pps_buf) {
        av_freep(&pps);
        return AVERROR(ENOMEM);
    }

    av_log(s->avctx, AV_LOG_DEBUG, "Decoding PPS\n");

    // Default values
    pps->loop_filter_across_tiles_enabled_flag = 1;
    pps->num_tile_columns                      = 1;
    pps->num_tile_rows                         = 1;
    pps->uniform_spacing_flag                  = 1;
    pps->disable_dbf                           = 0;
    pps->beta_offset                           = 0;
    pps->tc_offset                             = 0;
    pps->log2_max_transform_skip_block_size    = 2;

    // Coded parameters
    pps_id = get_ue_golomb_long(gb);
    print_cabac("pps_pic_parameter_set_id", pps_id);
    if (pps_id >= MAX_PPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", pps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->sps_id = get_ue_golomb_long(gb);
    print_cabac("pps_seq_parameter_set_id", pps->sps_id);
    if (pps->sps_id >= MAX_SPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "SPS id out of range: %d\n", pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    if (!s->sps_list[pps->sps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "SPS %u does not exist.\n", pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    sps = (HEVCSPS *)s->sps_list[pps->sps_id]->data;

    pps->dependent_slice_segments_enabled_flag = get_bits1(gb);
    pps->output_flag_present_flag              = get_bits1(gb);
    pps->num_extra_slice_header_bits           = get_bits(gb, 3);

    pps->sign_data_hiding_flag = get_bits1(gb);

    pps->cabac_init_present_flag = get_bits1(gb);

    pps->num_ref_idx_l0_default_active = get_ue_golomb_long(gb) + 1;
    pps->num_ref_idx_l1_default_active = get_ue_golomb_long(gb) + 1;

    pps->pic_init_qp_minus26 = get_se_golomb(gb);

    pps->constrained_intra_pred_flag = get_bits1(gb);
    pps->transform_skip_enabled_flag = get_bits1(gb);

    pps->cu_qp_delta_enabled_flag = get_bits1(gb);
    print_cabac("dependent_slice_segments_enabled_flag", pps->dependent_slice_segments_enabled_flag);
    print_cabac("output_flag_present_flag", pps->output_flag_present_flag);
    print_cabac("num_extra_slice_header_bits", pps->num_extra_slice_header_bits);
    print_cabac("sign_data_hiding_flag", pps->sign_data_hiding_flag);
    print_cabac("cabac_init_present_flag", pps->cabac_init_present_flag);
    print_cabac("num_ref_idx_l0_default_active_minus1", pps->num_ref_idx_l0_default_active-1);
    print_cabac("num_ref_idx_l1_default_active_minus1", pps->num_ref_idx_l1_default_active-1);
    print_cabac("init_qp_minus26", pps->pic_init_qp_minus26);
    print_cabac("constrained_intra_pred_flag", pps->constrained_intra_pred_flag);
    print_cabac("transform_skip_enabled_flag", pps->transform_skip_enabled_flag);
    print_cabac("cu_qp_delta_enabled_flag", pps->cu_qp_delta_enabled_flag);

    pps->diff_cu_qp_delta_depth   = 0;
    if (pps->cu_qp_delta_enabled_flag) {
        pps->diff_cu_qp_delta_depth = get_ue_golomb_long(gb);
        print_cabac("diff_cu_qp_delta_depth", pps->diff_cu_qp_delta_depth);
    }

    pps->cb_qp_offset = get_se_golomb(gb);
    if (pps->cb_qp_offset < -12 || pps->cb_qp_offset > 12) {
        av_log(s->avctx, AV_LOG_ERROR, "pps_cb_qp_offset out of range: %d\n",
               pps->cb_qp_offset);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->cr_qp_offset = get_se_golomb(gb);
    if (pps->cr_qp_offset < -12 || pps->cr_qp_offset > 12) {
        av_log(s->avctx, AV_LOG_ERROR, "pps_cr_qp_offset out of range: %d\n",
               pps->cr_qp_offset);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }
    pps->pic_slice_level_chroma_qp_offsets_present_flag = get_bits1(gb);

    pps->weighted_pred_flag   = get_bits1(gb);
    pps->weighted_bipred_flag = get_bits1(gb);

    pps->transquant_bypass_enable_flag    = get_bits1(gb);
    pps->tiles_enabled_flag               = get_bits1(gb);
    pps->entropy_coding_sync_enabled_flag = get_bits1(gb);


    print_cabac("pps_cb_qp_offset", pps->cb_qp_offset);
    print_cabac("pps_cr_qp_offset", pps->cr_qp_offset);
    print_cabac("pps_slice_chroma_qp_offsets_present_flag", pps->pic_slice_level_chroma_qp_offsets_present_flag);
    print_cabac("weighted_pred_flag", pps->weighted_pred_flag);
    print_cabac("weighted_bipred_flag", pps->weighted_bipred_flag);
    print_cabac("transquant_bypass_enable_flag", pps->transquant_bypass_enable_flag);
    print_cabac("tiles_enabled_flag", pps->tiles_enabled_flag);
    print_cabac("entropy_coding_sync_enabled_flag", pps->entropy_coding_sync_enabled_flag);

    if (pps->entropy_coding_sync_enabled_flag)
        av_log(s->avctx, AV_LOG_DEBUG, "WPP enabled\n");


    if (pps->tiles_enabled_flag) {
        av_log(s->avctx, AV_LOG_DEBUG, "Tiles enabled\n");
        pps->num_tile_columns = get_ue_golomb_long(gb) + 1;
        pps->num_tile_rows    = get_ue_golomb_long(gb) + 1;

        print_cabac("num_tile_columns_minus1", pps->num_tile_columns - 1);
        print_cabac("num_tile_rows_minus1", pps->num_tile_rows - 1);

        if (pps->num_tile_columns == 0 ||
            pps->num_tile_columns >= sps->width) {
            av_log(s->avctx, AV_LOG_ERROR, "num_tile_columns_minus1 out of range: %d\n",
                   pps->num_tile_columns - 1);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }
        if (pps->num_tile_rows == 0 ||
            pps->num_tile_rows >= sps->height) {
            av_log(s->avctx, AV_LOG_ERROR, "num_tile_rows_minus1 out of range: %d\n",
                   pps->num_tile_rows - 1);
            ret = AVERROR_INVALIDDATA;
            goto err;
        }

        pps->column_width = av_malloc_array(pps->num_tile_columns, sizeof(*pps->column_width));
        pps->row_height   = av_malloc_array(pps->num_tile_rows,    sizeof(*pps->row_height));
        if (!pps->column_width || !pps->row_height) {
            ret = AVERROR(ENOMEM);
            goto err;
        }

        pps->uniform_spacing_flag = get_bits1(gb);
        print_cabac("uniform_spacing_flag", pps->uniform_spacing_flag);

        if (!pps->uniform_spacing_flag) {
            uint64_t sum = 0;
            for (i = 0; i < pps->num_tile_columns - 1; i++) {
                pps->column_width[i] = get_ue_golomb_long(gb) + 1;
                sum                 += pps->column_width[i];
                print_cabac("column_width_minus1", pps->column_width[i]-1);

            }
            if (sum >= sps->ctb_width) {
                av_log(s->avctx, AV_LOG_ERROR, "Invalid tile widths.\n");
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            pps->column_width[pps->num_tile_columns - 1] = sps->ctb_width - sum;

            sum = 0;
            for (i = 0; i < pps->num_tile_rows - 1; i++) {
                pps->row_height[i] = get_ue_golomb_long(gb) + 1;
                print_cabac("row_height_minus1", pps->row_height[i]-1);
                sum               += pps->row_height[i];
            }
            if (sum >= sps->ctb_height) {
                av_log(s->avctx, AV_LOG_ERROR, "Invalid tile heights.\n");
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            pps->row_height[pps->num_tile_rows - 1] = sps->ctb_height - sum;
        }
        pps->loop_filter_across_tiles_enabled_flag = get_bits1(gb);
        print_cabac("loop_filter_across_tiles_enabled_flag",  pps->loop_filter_across_tiles_enabled_flag);
    }

    pps->seq_loop_filter_across_slices_enabled_flag = get_bits1(gb);
    print_cabac("loop_filter_across_slices_enabled_flag", pps->seq_loop_filter_across_slices_enabled_flag);

    pps->deblocking_filter_control_present_flag = get_bits1(gb);
    print_cabac("deblocking_filter_control_present_flag", pps->deblocking_filter_control_present_flag);

    if (pps->deblocking_filter_control_present_flag) {
        pps->deblocking_filter_override_enabled_flag = get_bits1(gb);
        pps->disable_dbf                             = get_bits1(gb);

        print_cabac("deblocking_filter_override_enabled_flag", pps->deblocking_filter_override_enabled_flag);
        print_cabac("pps_disable_deblocking_filter_flag",  pps->disable_dbf); 

        if (!pps->disable_dbf) {
            pps->beta_offset = get_se_golomb(gb) * 2;
            pps->tc_offset = get_se_golomb(gb) * 2;
            print_cabac("pps_beta_offset_div2", pps->beta_offset);
            print_cabac("pps_tc_offset_div2", pps->tc_offset);
            if (pps->beta_offset/2 < -6 || pps->beta_offset/2 > 6) {
                av_log(s->avctx, AV_LOG_ERROR, "pps_beta_offset_div2 out of range: %d\n",
                       pps->beta_offset/2);
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
            if (pps->tc_offset/2 < -6 || pps->tc_offset/2 > 6) {
                av_log(s->avctx, AV_LOG_ERROR, "pps_tc_offset_div2 out of range: %d\n",
                       pps->tc_offset/2);
                ret = AVERROR_INVALIDDATA;
                goto err;
            }
        }
    }
    pps->m_inferScalingListFlag = 0;
    if (s->nuh_layer_id > 0) {
        pps->m_inferScalingListFlag = get_bits1(gb);
        print_cabac("pps_infer_scaling_list_flag", pps->m_inferScalingListFlag);
    }
    if (pps->m_inferScalingListFlag) {
        pps->m_scalingListRefLayerId = get_ue_golomb_long(gb);
        print_cabac("pps_scaling_list_ref_layer_id", pps->m_scalingListRefLayerId);
        pps->scaling_list_data_present_flag = 0;
    } else {
        pps->scaling_list_data_present_flag = get_bits1(gb);
        print_cabac("pps_scaling_list_data_present_flag", pps->scaling_list_data_present_flag);

        if (pps->scaling_list_data_present_flag) {
            set_default_scaling_list_data(&pps->scaling_list);
            ret = scaling_list_data(s, &pps->scaling_list, sps);
            if (ret < 0)
                goto err;
        }
    }

    pps->lists_modification_present_flag = get_bits1(gb);
    pps->log2_parallel_merge_level       = get_ue_golomb_long(gb) + 2;

    print_cabac("lists_modification_present_flag", pps->lists_modification_present_flag);
    print_cabac("log2_parallel_merge_level_minus2", pps->log2_parallel_merge_level-2 );

    if (pps->log2_parallel_merge_level > sps->log2_ctb_size) {
        av_log(s->avctx, AV_LOG_ERROR, "log2_parallel_merge_level_minus2 out of range: %d\n",
               pps->log2_parallel_merge_level - 2);
        ret = AVERROR_INVALIDDATA;
        goto err;
    }

    pps->slice_header_extension_present_flag = get_bits1(gb);

    if (get_bits1(gb)) { // pps_extension_present_flag
        int pps_range_extensions_flag     = get_bits1(gb);
        int pps_multilayer_extension_flag = get_bits1(gb);
        int pps_extension_6bits = get_bits(gb, 6);
        print_cabac("pps_range_extensions_flag", pps_range_extensions_flag);
        print_cabac("pps_multilayer_extension_flag", pps_multilayer_extension_flag);
        if (sps->ptl.general_ptl.profile_idc == FF_PROFILE_HEVC_REXT && pps_range_extensions_flag) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "PPS extension flag is partially implemented.\n");
            pps_range_extensions(s, pps, sps);
        }

        if (sps->ptl.general_ptl.profile_idc != FF_PROFILE_HEVC_REXT    &&
            sps->ptl.general_ptl.profile_idc != FF_PROFILE_HEVC_MAIN    &&
            sps->ptl.general_ptl.profile_idc != FF_PROFILE_HEVC_MAIN_10 &&
            pps_range_extensions_flag) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "hack with the spec.\n"
                   "use pps_range_extensions_flag instead of pps_multilayer_extension_flag\n."
                   "PPS multilayer extension flag is partially implemented.\n");
            pps_multilayer_extensions(s, pps, sps);
        }
	}

    // Inferred parameters
    col_bd   = av_malloc_array(pps->num_tile_columns + 1, sizeof(*col_bd));
    row_bd   = av_malloc_array(pps->num_tile_rows + 1,    sizeof(*row_bd));
    pps->col_idxX = av_malloc_array(sps->ctb_width,    sizeof(*pps->col_idxX));
    if (!col_bd || !row_bd || !pps->col_idxX) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    if (pps->uniform_spacing_flag) {
        if (!pps->column_width) {
            pps->column_width = av_malloc_array(pps->num_tile_columns, sizeof(*pps->column_width));
            pps->row_height   = av_malloc_array(pps->num_tile_rows,    sizeof(*pps->row_height));
        }
        if (!pps->column_width || !pps->row_height) {
            ret = AVERROR(ENOMEM);
            goto err;
        }

        for (i = 0; i < pps->num_tile_columns; i++) {
            pps->column_width[i] = ((i + 1) * sps->ctb_width) / pps->num_tile_columns -
                                   (i * sps->ctb_width) / pps->num_tile_columns;
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            pps->row_height[i] = ((i + 1) * sps->ctb_height) / pps->num_tile_rows -
                                 (i * sps->ctb_height) / pps->num_tile_rows;
        }
    }

    col_bd[0] = 0;
    for (i = 0; i < pps->num_tile_columns; i++)
        col_bd[i + 1] = col_bd[i] + pps->column_width[i];

    row_bd[0] = 0;
    for (i = 0; i < pps->num_tile_rows; i++)
        row_bd[i + 1] = row_bd[i] + pps->row_height[i];

    for (i = 0, j = 0; i < sps->ctb_width; i++) {
        if (i > col_bd[j])
            j++;
        pps->col_idxX[i] = j;
    }

    /**
     * 6.5
     */
    pic_area_in_ctbs     = sps->ctb_width    * sps->ctb_height;

    pps->ctb_addr_rs_to_ts = av_malloc_array(pic_area_in_ctbs,    sizeof(*pps->ctb_addr_rs_to_ts));
    pps->ctb_addr_ts_to_rs = av_malloc_array(pic_area_in_ctbs,    sizeof(*pps->ctb_addr_ts_to_rs));
    pps->tile_id           = av_malloc_array(pic_area_in_ctbs,    sizeof(*pps->tile_id));
    pps->min_tb_addr_zs_tab = av_malloc_array((sps->tb_mask+2) * (sps->tb_mask+2), sizeof(*pps->min_tb_addr_zs_tab));
    if (!pps->ctb_addr_rs_to_ts || !pps->ctb_addr_ts_to_rs ||
        !pps->tile_id || !pps->min_tb_addr_zs_tab) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    for (ctb_addr_rs = 0; ctb_addr_rs < pic_area_in_ctbs; ctb_addr_rs++) {
        int tb_x   = ctb_addr_rs % sps->ctb_width;
        int tb_y   = ctb_addr_rs / sps->ctb_width;
        int tile_x = 0;
        int tile_y = 0;
        int val    = 0;

        for (i = 0; i < pps->num_tile_columns; i++) {
            if (tb_x < col_bd[i + 1]) {
                tile_x = i;
                break;
            }
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            if (tb_y < row_bd[i + 1]) {
                tile_y = i;
                break;
            }
        }

        for (i = 0; i < tile_x; i++)
            val += pps->row_height[tile_y] * pps->column_width[i];
        for (i = 0; i < tile_y; i++)
            val += sps->ctb_width * pps->row_height[i];

        val += (tb_y - row_bd[tile_y]) * pps->column_width[tile_x] +
               tb_x - col_bd[tile_x];

        pps->ctb_addr_rs_to_ts[ctb_addr_rs] = val;
        pps->ctb_addr_ts_to_rs[val]         = ctb_addr_rs;
    }

    for (j = 0, tile_id = 0; j < pps->num_tile_rows; j++)
        for (i = 0; i < pps->num_tile_columns; i++, tile_id++)
            for (y = row_bd[j]; y < row_bd[j + 1]; y++)
                for (x = col_bd[i]; x < col_bd[i + 1]; x++)
                    pps->tile_id[pps->ctb_addr_rs_to_ts[y * sps->ctb_width + x]] = tile_id;

    pps->tile_pos_rs = av_malloc_array(tile_id, sizeof(*pps->tile_pos_rs));
    if (!pps->tile_pos_rs) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    for (j = 0; j < pps->num_tile_rows; j++)
        for (i = 0; i < pps->num_tile_columns; i++)
            pps->tile_pos_rs[j * pps->num_tile_columns + i] = row_bd[j] * sps->ctb_width + col_bd[i];

    log2_diff_ctb_min_tb_size = sps->log2_ctb_size - sps->log2_min_tb_size;
    pps->min_tb_addr_zs = &pps->min_tb_addr_zs_tab[1*(sps->tb_mask+2)+1];
    for (y = 0; y < sps->tb_mask+2; y++) {
        pps->min_tb_addr_zs_tab[y*(sps->tb_mask+2)] = -1;
        pps->min_tb_addr_zs_tab[y]    = -1;
    }
    for (y = 0; y < sps->tb_mask+1; y++) {
        for (x = 0; x < sps->tb_mask+1; x++) {
            int tb_x        = x >> log2_diff_ctb_min_tb_size;
            int tb_y        = y >> log2_diff_ctb_min_tb_size;
            int ctb_addr_rs = sps->ctb_width * tb_y + tb_x;
            int val         = pps->ctb_addr_rs_to_ts[ctb_addr_rs] <<
                              (log2_diff_ctb_min_tb_size * 2);
            for (i = 0; i < log2_diff_ctb_min_tb_size; i++) {
                int m = 1 << i;
                val += (m & x ? m * m : 0) + (m & y ? 2 * m * m : 0);
            }
            pps->min_tb_addr_zs[y * (sps->tb_mask+2) + x] = val;
        }
    }

    av_freep(&col_bd);
    av_freep(&row_bd);
    av_buffer_unref(&s->pps_list[pps_id]);
    s->pps_list[pps_id] = pps_buf;

    return 0;

err:
    av_buffer_unref(&pps_buf);
    return ret;
}
