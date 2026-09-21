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

#include "encode/HevcParamSets.h"

#include "encode/H264Vui.h"

namespace mw::native::encode {

namespace {

using h264vui_detail::BitWriter;

constexpr uint8_t kNalVps = 32;
constexpr uint8_t kNalSps = 33;
constexpr uint8_t kNalPps = 34;

// profile_tier_level( 1, 0 ): the general part only, there are no sub-layers.
void profileTierLevel(BitWriter& w, const HevcStreamShape& shape)
{
    const uint32_t profile = shape.tenBit ? 2u : 1u; // Main 10 : Main
    w.u(2, 0);                                       // general_profile_space
    w.u(1, shape.highTier ? 1u : 0u);
    w.u(5, profile);
    // A Main stream is also a conforming Main 10 stream, and says so.
    for (uint32_t j = 0; j < 32; ++j)
        w.u(1, (j == profile || (j == 2 && profile == 1)) ? 1u : 0u);
    w.u(1, 1);  // general_progressive_source_flag
    w.u(1, 0);  // general_interlaced_source_flag
    w.u(1, 1);  // general_non_packed_constraint_flag
    w.u(1, 1);  // general_frame_only_constraint_flag
    w.u(32, 0); // general_reserved_zero_43bits, then general_inbld_flag: 44 zeros
    w.u(12, 0);
    w.u(8, static_cast<uint32_t>(shape.levelIdc));
}

// One (max_dec_pic_buffering, num_reorder, latency_increase) triple — no
// reordering, which is what spares the viewer's decoder a DPB of delay.
void orderingInfo(BitWriter& w, const HevcStreamShape& shape)
{
    w.ue(static_cast<uint32_t>(shape.decodedPictureBuffer - 1));
    w.ue(0); // max_num_reorder_pics
    w.ue(0); // max_latency_increase_plus1: no limit expressed
}

void videoUsability(BitWriter& w, const HevcStreamShape& shape)
{
    w.u(1, 0);                    // aspect_ratio_info_present_flag
    w.u(1, 0);                    // overscan_info_present_flag
    w.u(1, 1);                    // video_signal_type_present_flag
    w.u(3, 5);                    // video_format: unspecified
    w.u(1, 0);                    // video_full_range_flag: limited, as the conversion shader writes
    w.u(1, 1);                    // colour_description_present_flag
    w.u(8, shape.hdr ? 9u : 1u);  // colour_primaries: BT.2020 : BT.709
    w.u(8, shape.hdr ? 16u : 1u); // transfer_characteristics: SMPTE 2084 (PQ) : BT.709
    w.u(8, shape.hdr ? 9u : 1u);  // matrix_coeffs: BT.2020 NCL : BT.709
    w.u(1, 0);                    // chroma_loc_info_present_flag
    w.u(1, 0);                    // neutral_chroma_indication_flag
    w.u(1, 0);                    // field_seq_flag
    w.u(1, 0);                    // frame_field_info_present_flag
    w.u(1, 0);                    // default_display_window_flag
    w.u(1, 0);                    // vui_timing_info_present_flag
    w.u(1, 0);                    // bitstream_restriction_flag
}

std::vector<uint8_t> nal(uint8_t type, BitWriter& rbsp)
{
    rbsp.u(1, 1); // rbsp_stop_one_bit; the writer pads the byte with zeros
    std::vector<uint8_t> out;
    out.reserve(rbsp.bytes.size() + 8);
    // forbidden_zero_bit, the type, then nuh_layer_id 0 and nuh_temporal_id_plus1 1.
    out.push_back(static_cast<uint8_t>(type << 1));
    out.push_back(1);
    h264vui_detail::escapeInto(rbsp.bytes, out);
    return out;
}

} // namespace

std::vector<uint8_t> hevcVps(const HevcStreamShape& shape)
{
    BitWriter w;
    w.u(4, 0);       // vps_video_parameter_set_id
    w.u(1, 1);       // vps_base_layer_internal_flag
    w.u(1, 1);       // vps_base_layer_available_flag
    w.u(6, 0);       // vps_max_layers_minus1
    w.u(3, 0);       // vps_max_sub_layers_minus1
    w.u(1, 1);       // vps_temporal_id_nesting_flag
    w.u(16, 0xFFFF); // vps_reserved_0xffff_16bits
    profileTierLevel(w, shape);
    w.u(1, 0); // vps_sub_layer_ordering_info_present_flag
    orderingInfo(w, shape);
    w.u(6, 0); // vps_max_layer_id
    w.ue(0);   // vps_num_layer_sets_minus1
    w.u(1, 0); // vps_timing_info_present_flag
    w.u(1, 0); // vps_extension_flag
    return nal(kNalVps, w);
}

std::vector<uint8_t> hevcSps(const HevcStreamShape& shape)
{
    BitWriter w;
    w.u(4, 0); // sps_video_parameter_set_id
    w.u(3, 0); // sps_max_sub_layers_minus1
    w.u(1, 1); // sps_temporal_id_nesting_flag
    profileTierLevel(w, shape);
    w.ue(0); // sps_seq_parameter_set_id
    w.ue(1); // chroma_format_idc: 4:2:0
    w.ue(static_cast<uint32_t>(shape.codedWidth));
    w.ue(static_cast<uint32_t>(shape.codedHeight));
    // The window is counted in chroma samples: half the luma ones in 4:2:0.
    const int cropRight = (shape.codedWidth - shape.width) / 2;
    const int cropBottom = (shape.codedHeight - shape.height) / 2;
    const bool cropped = cropRight > 0 || cropBottom > 0;
    w.u(1, cropped ? 1u : 0u);
    if (cropped) {
        w.ue(0);
        w.ue(static_cast<uint32_t>(cropRight));
        w.ue(0);
        w.ue(static_cast<uint32_t>(cropBottom));
    }
    w.ue(shape.tenBit ? 2u : 0u); // bit_depth_luma_minus8
    w.ue(shape.tenBit ? 2u : 0u); // bit_depth_chroma_minus8
    w.ue(static_cast<uint32_t>(shape.log2MaxPicOrderCntLsb - 4));
    w.u(1, 0); // sps_sub_layer_ordering_info_present_flag
    orderingInfo(w, shape);
    w.ue(static_cast<uint32_t>(shape.log2MinCodingBlock - 3));
    w.ue(static_cast<uint32_t>(shape.log2MaxCodingBlock - shape.log2MinCodingBlock));
    w.ue(static_cast<uint32_t>(shape.log2MinTransformBlock - 2));
    w.ue(static_cast<uint32_t>(shape.log2MaxTransformBlock - shape.log2MinTransformBlock));
    w.ue(static_cast<uint32_t>(shape.transformDepthInter));
    w.ue(static_cast<uint32_t>(shape.transformDepthIntra));
    w.u(1, 0); // scaling_list_enabled_flag
    w.u(1, shape.asymmetricMotionPartitions ? 1u : 0u);
    w.u(1, shape.sampleAdaptiveOffset ? 1u : 0u);
    w.u(1, 0); // pcm_enabled_flag
    w.ue(0);   // num_short_term_ref_pic_sets: every slice header carries its own
    w.u(1, shape.longTermReferences ? 1u : 0u);
    if (shape.longTermReferences) w.ue(0); // num_long_term_ref_pics_sps: slice headers again
    w.u(1, 0);                             // sps_temporal_mvp_enabled_flag
    w.u(1, 0);                             // strong_intra_smoothing_enabled_flag
    w.u(1, 1);                             // vui_parameters_present_flag
    videoUsability(w, shape);
    w.u(1, 0); // sps_extension_present_flag
    return nal(kNalSps, w);
}

std::vector<uint8_t> hevcPps(const HevcStreamShape& shape)
{
    BitWriter w;
    w.ue(0);   // pps_pic_parameter_set_id
    w.ue(0);   // pps_seq_parameter_set_id
    w.u(1, 0); // dependent_slice_segments_enabled_flag
    w.u(1, 0); // output_flag_present_flag
    w.u(3, 0); // num_extra_slice_header_bits
    w.u(1, 0); // sign_data_hiding_enabled_flag
    w.u(1, 1); // cabac_init_present_flag
    w.ue(static_cast<uint32_t>(shape.defaultActiveReferences - 1)); // l0
    w.ue(0);                                                        // l1: no B pictures
    w.se(0);                                                        // init_qp_minus26
    w.u(1, shape.constrainedIntraPrediction ? 1u : 0u);
    w.u(1, shape.transformSkip ? 1u : 0u);
    w.u(1, 1); // cu_qp_delta_enabled_flag: rate control moves the QP inside a picture
    w.ue(0);   // diff_cu_qp_delta_depth
    w.se(0);   // pps_cb_qp_offset
    w.se(0);   // pps_cr_qp_offset
    w.u(1, 1); // pps_slice_chroma_qp_offsets_present_flag
    w.u(1, 0); // weighted_pred_flag
    w.u(1, 0); // weighted_bipred_flag
    w.u(1, 0); // transquant_bypass_enabled_flag
    w.u(1, 0); // tiles_enabled_flag
    w.u(1, 0); // entropy_coding_sync_enabled_flag
    w.u(1, shape.loopFilterAcrossSlices ? 1u : 0u);
    w.u(1, 1);                                  // deblocking_filter_control_present_flag
    w.u(1, 0);                                  //   deblocking_filter_override_enabled_flag
    w.u(1, 0);                                  //   pps_deblocking_filter_disabled_flag
    w.se(0);                                    //   pps_beta_offset_div2
    w.se(0);                                    //   pps_tc_offset_div2
    w.u(1, 0);                                  // pps_scaling_list_data_present_flag
    w.u(1, shape.longTermReferences ? 1u : 0u); // lists_modification_present_flag
    w.ue(0);                                    // log2_parallel_merge_level_minus2
    w.u(1, 0);                                  // slice_segment_header_extension_present_flag
    w.u(1, 0);                                  // pps_extension_present_flag
    return nal(kNalPps, w);
}

std::vector<uint8_t> hevcParameterSets(const HevcStreamShape& shape)
{
    std::vector<uint8_t> out;
    for (const auto& unit : {hevcVps(shape), hevcSps(shape), hevcPps(shape)}) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

} // namespace mw::native::encode
