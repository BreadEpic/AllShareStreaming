/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "H264Vui.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

// The parameter sets and slice headers of an H.264 or HEVC stream, written by
// us — for a driver that wants them from the application.
//
// ── Why ─────────────────────────────────────────────────────────────────────
//
// Mesa's VA-API encoders stopped writing their own headers somewhere after
// 23.2: from 25.x (radeonsi, and every driver behind the same frontend) the
// VPS/SPS/PPS come out ONLY when the application hands them in as packed
// headers, and the slice header's frame_num (H.264) or reference picture set
// (HEVC) are read from the application's packed slice header too — without
// one they are zero. Mesa then re-writes every one of them itself from what it
// parsed, so what is written here is the description, and the driver's own
// writer produces the bytes that leave. That is why these must say exactly
// what the parameter buffers say: they are not decoration on top of them,
// they replace them.
//
// Measured on a Radeon 610M, Mesa 25.2.8 (16/09/2026): keyframes of the
// picture alone, no parameter set, no client could start. Same binary on a
// 780M under Mesa 23.2: all four sets, written by the driver.
//
// ── What they say ───────────────────────────────────────────────────────────
//
// The encoder's choices, nothing more: I and P only, one reference used per
// picture (the others kept, so a repair after a loss can reach back to them),
// POC = decode order, no reordering (the B8 lesson: bitstream_restriction on
// H.264), BT.709 limited range — the converter's output — declared in the VUI.
//
// Every function returns one NAL unit in Annex-B form: a four-byte start code,
// the NAL header, the RBSP with emulation prevention and its trailing bits.
// Pure bytes, no driver: testable anywhere.

namespace mw::native::encode::paramsets {

namespace detail {

using h264vui_detail::BitWriter;

inline void se(BitWriter& w, int32_t v)
{
    w.ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v));
}

/// rbsp_trailing_bits(): a one, then zeros to the byte.
inline void trailing(BitWriter& w)
{
    w.u(1, 1);
    while (w.used != 8)
        w.u(1, 0);
}

inline std::vector<uint8_t> nal(std::initializer_list<uint8_t> header, BitWriter& w)
{
    trailing(w);
    std::vector<uint8_t> out = {0, 0, 0, 1};
    out.insert(out.end(), header.begin(), header.end());
    h264vui_detail::escapeInto(w.bytes, out);
    return out;
}

/// BT.709, limited range: what GlConvert and every other converter here write.
inline void bt709Limited(BitWriter& w)
{
    w.u(1, 1); // video_signal_type_present_flag
    w.u(3, 5); // video_format: unspecified
    w.u(1, 0); // video_full_range_flag
    w.u(1, 1); // colour_description_present_flag
    w.u(8, 1); // colour_primaries: BT.709
    w.u(8, 1); // transfer_characteristics: BT.709
    w.u(8, 1); // matrix_coefficients: BT.709
}

} // namespace detail

// ── H.264 ───────────────────────────────────────────────────────────────────

struct H264Sequence
{
    int profileIdc = 100; ///< 66 Constrained Baseline, 77 Main, 100 High
    int levelIdc = 51;
    uint32_t widthMbs = 0;
    uint32_t heightMbs = 0;
    /// The crop, in the frame_crop_*_offset unit: two luma samples in 4:2:0.
    uint32_t cropRight = 0;
    uint32_t cropBottom = 0;
    uint32_t maxRefFrames = 1;
    /// log2_max_frame_num: frame_num is this many bits wide.
    int log2MaxFrameNum = 16;
    int fps = 60;
};

inline std::vector<uint8_t> h264Sps(const H264Sequence& s)
{
    detail::BitWriter w;
    w.u(8, static_cast<uint32_t>(s.profileIdc));
    // Constrained Baseline is Baseline with constraint_set0 and 1: the only
    // Baseline a browser decodes.
    w.u(8, s.profileIdc == 66 ? 0xC0 : 0x00); // constraint flags + reserved_zero_2bits
    w.u(8, static_cast<uint32_t>(s.levelIdc));
    w.ue(0); // seq_parameter_set_id
    if (s.profileIdc == 100) {
        w.ue(1);   // chroma_format_idc: 4:2:0
        w.ue(0);   // bit_depth_luma_minus8
        w.ue(0);   // bit_depth_chroma_minus8
        w.u(1, 0); // qpprime_y_zero_transform_bypass_flag
        w.u(1, 0); // seq_scaling_matrix_present_flag
    }
    w.ue(static_cast<uint32_t>(s.log2MaxFrameNum - 4));
    // POC type 2: output order IS decode order — no B-frames, nothing to count.
    w.ue(2);
    w.ue(s.maxRefFrames);
    w.u(1, 0); // gaps_in_frame_num_value_allowed_flag
    w.ue(s.widthMbs - 1);
    w.ue(s.heightMbs - 1);
    w.u(1, 1); // frame_mbs_only_flag
    w.u(1, 1); // direct_8x8_inference_flag
    const bool crop = s.cropRight || s.cropBottom;
    w.u(1, crop ? 1 : 0);
    if (crop) {
        w.ue(0);
        w.ue(s.cropRight);
        w.ue(0);
        w.ue(s.cropBottom);
    }

    w.u(1, 1); // vui_parameters_present_flag
    w.u(1, 0); // aspect_ratio_info_present_flag
    w.u(1, 0); // overscan_info_present_flag
    detail::bt709Limited(w);
    w.u(1, 0);  // chroma_loc_info_present_flag
    w.u(1, 1);  // timing_info_present_flag — the tick counts fields, hence ×2
    w.u(32, 1); // num_units_in_tick
    w.u(32, static_cast<uint32_t>(s.fps) * 2);
    w.u(1, 0); // fixed_frame_rate_flag
    w.u(1, 0); // nal_hrd_parameters_present_flag
    w.u(1, 0); // vcl_hrd_parameters_present_flag
    w.u(1, 0); // pic_struct_present_flag
    // bitstream_restriction — the B8 lesson: without it a hardware decoder
    // holds a DPB's worth of frames on a stream that reorders none.
    w.u(1, 1);
    w.u(1, 1); // motion_vectors_over_pic_boundaries_flag
    w.ue(2);   // max_bytes_per_pic_denom (the default)
    w.ue(1);   // max_bits_per_mb_denom (the default)
    w.ue(15);  // log2_max_mv_length_horizontal
    w.ue(15);  // log2_max_mv_length_vertical
    w.ue(0);   // max_num_reorder_frames
    w.ue(s.maxRefFrames);
    return detail::nal({0x67}, w); // nal_ref_idc 3, type 7
}

inline std::vector<uint8_t> h264Pps(const H264Sequence& s)
{
    detail::BitWriter w;
    w.ue(0);                            // pic_parameter_set_id
    w.ue(0);                            // seq_parameter_set_id
    w.u(1, s.profileIdc == 66 ? 0 : 1); // entropy_coding_mode_flag: CABAC above Baseline
    w.u(1, 0);                          // bottom_field_pic_order_in_frame_present_flag
    w.ue(0);                            // num_slice_groups_minus1
    w.ue(0);          // num_ref_idx_l0_default_active_minus1: one reference per picture
    w.ue(0);          // num_ref_idx_l1_default_active_minus1
    w.u(1, 0);        // weighted_pred_flag
    w.u(2, 0);        // weighted_bipred_idc
    detail::se(w, 0); // pic_init_qp_minus26 — the parameter buffer's 26
    detail::se(w, 0); // pic_init_qs_minus26
    detail::se(w, 0); // chroma_qp_index_offset
    w.u(1, 1);        // deblocking_filter_control_present_flag
    w.u(1, 0);        // constrained_intra_pred_flag
    w.u(1, 0);        // redundant_pic_cnt_present_flag
    if (s.profileIdc == 100) {
        w.u(1, 1);        // transform_8x8_mode_flag
        w.u(1, 0);        // pic_scaling_matrix_present_flag
        detail::se(w, 0); // second_chroma_qp_index_offset
    }
    return detail::nal({0x68}, w);
}

struct H264Slice
{
    bool idr = false;
    uint32_t frameNum = 0;
    uint32_t idrPicId = 0;
    /// The frame_num of the picture this one predicts from. Ignored on an IDR.
    uint32_t referenceFrameNum = 0;
};

/// The slice header, up to and including the deblocking fields.
///
/// A P picture predicts from the newest short-term picture by default. When it
/// must reach further back — the frames after the reference were lost — the
/// header reorders the list so the chosen one comes first
/// (ref_pic_list_modification, subtracting from the current picture number).
inline std::vector<uint8_t> h264SliceHeader(const H264Sequence& s, const H264Slice& slice)
{
    const uint32_t frameNumMask = (1u << s.log2MaxFrameNum) - 1u;
    detail::BitWriter w;
    w.ue(0);                 // first_mb_in_slice
    w.ue(slice.idr ? 7 : 5); // slice_type, every slice of the picture alike
    w.ue(0);                 // pic_parameter_set_id
    w.u(s.log2MaxFrameNum, slice.frameNum & frameNumMask);
    if (slice.idr) w.ue(slice.idrPicId & 0xFFFF);
    // POC type 2: no pic_order_cnt_lsb.
    if (!slice.idr) {
        w.u(1, 0); // num_ref_idx_active_override_flag
        const uint32_t distance = (slice.frameNum - slice.referenceFrameNum) & frameNumMask;
        const bool reorder = distance != 1;
        w.u(1, reorder ? 1 : 0); // ref_pic_list_modification_flag_l0
        if (reorder) {
            w.ue(0);            // modification_of_pic_nums_idc: subtract
            w.ue(distance - 1); // abs_diff_pic_num_minus1
            w.ue(3);            // end of the list
        }
    }
    // dec_ref_pic_marking: every picture is a reference.
    if (slice.idr) {
        w.u(1, 0); // no_output_of_prior_pics_flag
        w.u(1, 0); // long_term_reference_flag
    } else {
        w.u(1, 0); // adaptive_ref_pic_marking_mode_flag: the sliding window
    }
    if (!slice.idr && s.profileIdc != 66) w.ue(0); // cabac_init_idc
    detail::se(w, 0);                              // slice_qp_delta
    w.ue(0);                                       // disable_deblocking_filter_idc
    detail::se(w, 0);                              // slice_alpha_c0_offset_div2
    detail::se(w, 0);                              // slice_beta_offset_div2
    return detail::nal({static_cast<uint8_t>(slice.idr ? 0x65 : 0x41)}, w);
}

// ── HEVC ────────────────────────────────────────────────────────────────────

struct HevcSequence
{
    /// The picture as shown.
    uint32_t width = 0;
    uint32_t height = 0;
    /// The picture as coded — a whole number of the blocks the encoder works
    /// in. The difference goes in the conformance window, in chroma samples.
    uint32_t codedWidth = 0;
    uint32_t codedHeight = 0;
    int levelIdc = 153; ///< 30 × the level number: 153 = 5.1
    /// sps_max_dec_pic_buffering_minus1: the pictures kept for reference.
    uint32_t maxReferences = 1;
    int log2MaxPocLsb = 16;
    int fps = 60;
};

namespace detail {

inline void profileTierLevel(BitWriter& w, int levelIdc)
{
    w.u(2, 0);           // general_profile_space
    w.u(1, 0);           // general_tier_flag: Main tier
    w.u(5, 1);           // general_profile_idc: Main
    w.u(32, 0x60000000); // compatible with Main (1) and Main 10 (2)
    w.u(1, 1);           // general_progressive_source_flag
    w.u(1, 0);           // general_interlaced_source_flag
    w.u(1, 0);           // general_non_packed_constraint_flag
    w.u(1, 1);           // general_frame_only_constraint_flag
    w.u(32, 0);          // general_reserved_zero_43bits + general_inbld_flag…
    w.u(12, 0);          // …44 bits in all
    w.u(8, static_cast<uint32_t>(levelIdc));
}

} // namespace detail

inline std::vector<uint8_t> hevcVps(const HevcSequence& s)
{
    detail::BitWriter w;
    w.u(4, 0);       // vps_video_parameter_set_id
    w.u(1, 1);       // vps_base_layer_internal_flag
    w.u(1, 1);       // vps_base_layer_available_flag
    w.u(6, 0);       // vps_max_layers_minus1
    w.u(3, 0);       // vps_max_sub_layers_minus1
    w.u(1, 1);       // vps_temporal_id_nesting_flag
    w.u(16, 0xFFFF); // vps_reserved_0xffff_16bits
    detail::profileTierLevel(w, s.levelIdc);
    w.u(1, 1); // vps_sub_layer_ordering_info_present_flag
    w.ue(s.maxReferences);
    w.ue(0);   // vps_max_num_reorder_pics
    w.ue(0);   // vps_max_latency_increase_plus1
    w.u(6, 0); // vps_max_layer_id
    w.ue(0);   // vps_num_layer_sets_minus1
    w.u(1, 0); // vps_timing_info_present_flag
    w.u(1, 0); // vps_extension_flag
    return detail::nal({0x40, 0x01}, w);
}

inline std::vector<uint8_t> hevcSps(const HevcSequence& s)
{
    detail::BitWriter w;
    w.u(4, 0); // sps_video_parameter_set_id
    w.u(3, 0); // sps_max_sub_layers_minus1
    w.u(1, 1); // sps_temporal_id_nesting_flag
    detail::profileTierLevel(w, s.levelIdc);
    w.ue(0); // sps_seq_parameter_set_id
    w.ue(1); // chroma_format_idc: 4:2:0
    w.ue(s.codedWidth);
    w.ue(s.codedHeight);
    const uint32_t right = (s.codedWidth - s.width) / 2;
    const uint32_t bottom = (s.codedHeight - s.height) / 2;
    const bool window = right || bottom;
    w.u(1, window ? 1 : 0); // conformance_window_flag
    if (window) {
        w.ue(0);
        w.ue(right);
        w.ue(0);
        w.ue(bottom);
    }
    w.ue(0); // bit_depth_luma_minus8
    w.ue(0); // bit_depth_chroma_minus8
    w.ue(static_cast<uint32_t>(s.log2MaxPocLsb - 4));
    w.u(1, 1);             // sps_sub_layer_ordering_info_present_flag
    w.ue(s.maxReferences); // sps_max_dec_pic_buffering_minus1
    w.ue(0);               // sps_max_num_reorder_pics — the whole point
    w.ue(0);               // sps_max_latency_increase_plus1
    // The block geometry of the sequence parameter buffer: CTB 64, min CB 8.
    w.ue(0);   // log2_min_luma_coding_block_size_minus3
    w.ue(3);   // log2_diff_max_min_luma_coding_block_size
    w.ue(0);   // log2_min_luma_transform_block_size_minus2
    w.ue(3);   // log2_diff_max_min_luma_transform_block_size
    w.ue(2);   // max_transform_hierarchy_depth_inter
    w.ue(2);   // max_transform_hierarchy_depth_intra
    w.u(1, 0); // scaling_list_enabled_flag
    w.u(1, 1); // amp_enabled_flag
    w.u(1, 1); // sample_adaptive_offset_enabled_flag
    w.u(1, 0); // pcm_enabled_flag
    w.ue(0);   // num_short_term_ref_pic_sets: each slice carries its own
    w.u(1, 0); // long_term_ref_pics_present_flag
    w.u(1, 0); // sps_temporal_mvp_enabled_flag
    w.u(1, 1); // strong_intra_smoothing_enabled_flag

    w.u(1, 1); // vui_parameters_present_flag
    w.u(1, 0); // aspect_ratio_info_present_flag
    w.u(1, 0); // overscan_info_present_flag
    detail::bt709Limited(w);
    w.u(1, 0);  // chroma_loc_info_present_flag
    w.u(1, 0);  // neutral_chroma_indication_flag
    w.u(1, 0);  // field_seq_flag
    w.u(1, 0);  // frame_field_info_present_flag
    w.u(1, 0);  // default_display_window_flag
    w.u(1, 1);  // vui_timing_info_present_flag — a tick is a frame here
    w.u(32, 1); // vui_num_units_in_tick
    w.u(32, static_cast<uint32_t>(s.fps));
    w.u(1, 0); // vui_poc_proportional_to_timing_flag
    w.u(1, 0); // vui_hrd_parameters_present_flag
    w.u(1, 1); // bitstream_restriction_flag
    w.u(1, 0); // tiles_fixed_structure_flag
    w.u(1, 1); // motion_vectors_over_pic_boundaries_flag
    w.u(1, 1); // restricted_ref_pic_lists_flag
    w.ue(0);   // min_spatial_segmentation_idc
    w.ue(2);   // max_bytes_per_pic_denom
    w.ue(1);   // max_bits_per_min_cu_denom
    w.ue(15);  // log2_max_mv_length_horizontal
    w.ue(15);  // log2_max_mv_length_vertical
    w.u(1, 0); // sps_extension_present_flag
    return detail::nal({0x42, 0x01}, w);
}

inline std::vector<uint8_t> hevcPps(const HevcSequence&)
{
    detail::BitWriter w;
    w.ue(0);          // pps_pic_parameter_set_id
    w.ue(0);          // pps_seq_parameter_set_id
    w.u(1, 0);        // dependent_slice_segments_enabled_flag
    w.u(1, 0);        // output_flag_present_flag
    w.u(3, 0);        // num_extra_slice_header_bits
    w.u(1, 0);        // sign_data_hiding_enabled_flag
    w.u(1, 0);        // cabac_init_present_flag
    w.ue(0);          // num_ref_idx_l0_default_active_minus1: one reference per picture
    w.ue(0);          // num_ref_idx_l1_default_active_minus1
    detail::se(w, 0); // init_qp_minus26
    w.u(1, 0);        // constrained_intra_pred_flag
    w.u(1, 0);        // transform_skip_enabled_flag
    w.u(1, 1);        // cu_qp_delta_enabled_flag — what the picture buffer asks for
    w.ue(0);          // diff_cu_qp_delta_depth
    detail::se(w, 0); // pps_cb_qp_offset
    detail::se(w, 0); // pps_cr_qp_offset
    w.u(1, 0);        // pps_slice_chroma_qp_offsets_present_flag
    w.u(1, 0);        // weighted_pred_flag
    w.u(1, 0);        // weighted_bipred_flag
    w.u(1, 0);        // transquant_bypass_enabled_flag
    w.u(1, 0);        // tiles_enabled_flag
    w.u(1, 0);        // entropy_coding_sync_enabled_flag
    w.u(1, 0);        // pps_loop_filter_across_slices_enabled_flag: one slice
    w.u(1, 0);        // deblocking_filter_control_present_flag: the default filter
    w.u(1, 0);        // pps_scaling_list_data_present_flag
    w.u(1, 0);        // lists_modification_present_flag
    w.ue(0);          // log2_parallel_merge_level_minus2
    w.u(1, 0);        // slice_segment_header_extension_present_flag
    w.u(1, 0);        // pps_extension_present_flag
    return detail::nal({0x44, 0x01}, w);
}

struct HevcSlice
{
    bool idr = false;
    /// The picture order count of this picture: its number since the IDR.
    uint32_t poc = 0;
    /// Every picture still held for reference, by POC, the one used included.
    std::vector<uint32_t> kept;
    /// The one this picture predicts from — must be among `kept`.
    uint32_t referencePoc = 0;
};

/// The slice segment header of a picture's only slice.
///
/// HEVC keeps in the decoder only the pictures the reference picture set
/// lists: one left out is gone for good. So every picture still held is
/// listed — the one predicted from marked "used", the others kept for a repair
/// that may need to reach back to them after a loss.
inline std::vector<uint8_t> hevcSliceHeader(const HevcSequence& s, const HevcSlice& slice)
{
    detail::BitWriter w;
    w.u(1, 1);                // first_slice_segment_in_pic_flag
    if (slice.idr) w.u(1, 0); // no_output_of_prior_pics_flag
    w.ue(0);                  // slice_pic_parameter_set_id
    w.ue(slice.idr ? 2 : 1);  // slice_type: I or P
    if (!slice.idr) {
        w.u(s.log2MaxPocLsb, slice.poc & ((1u << s.log2MaxPocLsb) - 1u));
        w.u(1, 0); // short_term_ref_pic_set_sps_flag: the set follows

        std::vector<uint32_t> before;
        for (uint32_t poc : slice.kept)
            if (poc < slice.poc) before.push_back(poc);
        std::sort(before.begin(), before.end(), [](uint32_t a, uint32_t b) { return a > b; });

        // st_ref_pic_set(num_short_term_ref_pic_sets = 0): no prediction from
        // another set, since there is none.
        w.ue(static_cast<uint32_t>(before.size())); // num_negative_pics
        w.ue(0);                                    // num_positive_pics
        uint32_t previous = slice.poc;
        for (uint32_t poc : before) {
            w.ue(previous - poc - 1);                  // delta_poc_s0_minus1
            w.u(1, poc == slice.referencePoc ? 1 : 0); // used_by_curr_pic_s0_flag
            previous = poc;
        }
    }
    w.u(1, 1); // slice_sao_luma_flag
    w.u(1, 1); // slice_sao_chroma_flag
    if (!slice.idr) {
        w.u(1, 0); // num_ref_idx_active_override_flag
        w.ue(0);   // five_minus_max_num_merge_cand
    }
    detail::se(w, 0);                        // slice_qp_delta
    const uint8_t type = slice.idr ? 19 : 1; // IDR_W_RADL, TRAIL_R
    return detail::nal({static_cast<uint8_t>(type << 1), 0x01}, w);
}

} // namespace mw::native::encode::paramsets
