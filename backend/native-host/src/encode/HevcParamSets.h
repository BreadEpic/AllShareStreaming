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

#include <cstdint>
#include <vector>

// The HEVC parameter sets of a stream whose slices somebody else writes.
//
// ── Why ─────────────────────────────────────────────────────────────────────
//
// NVENC, AMF and oneVPL hand over a finished elementary stream. The D3D12 Video
// Encode API does not: the driver writes slice NAL units only, and the VPS, SPS
// and PPS they refer to are the application's to write. Nothing checks that the
// two agree — a PPS that says cabac_init_present_flag = 0 over slices coded
// with it set decodes as noise, with no error from anybody.
//
// So this is not a general HEVC writer. It says what a D3D12 driver's slices
// assume, which the API fixes for everything it does not let the caller choose:
// no tiles, no weighted prediction, no scaling lists, no temporal MVP, no
// short-term reference sets in the SPS (each slice header carries its own),
// cabac_init, per-slice chroma QP offsets, CU-level QP deltas and deblocking
// control always present. What the caller did choose — block sizes, AMP, SAO,
// long-term references — comes in through HevcStreamShape, from the very
// configuration the encoder was created with.

namespace mw::native::encode {

struct HevcStreamShape
{
    int width = 0; // the picture the viewer sees
    int height = 0;
    int codedWidth = 0; // what the encoder codes: the above, rounded up to its alignment
    int codedHeight = 0;
    bool tenBit = false;
    bool hdr = false; // BT.2020 + PQ in the VUI; BT.709 otherwise. Limited range either way.

    int levelIdc = 153; // general_level_idc, 30 × the level: 5.1 = 153
    bool highTier = false;

    int log2MinCodingBlock = 3;
    int log2MaxCodingBlock = 6;
    int log2MinTransformBlock = 2;
    int log2MaxTransformBlock = 5;
    int transformDepthInter = 2;
    int transformDepthIntra = 2;

    bool asymmetricMotionPartitions = false;
    bool sampleAdaptiveOffset = false;
    bool longTermReferences = false; // also turns on lists_modification_present_flag
    bool transformSkip = false;
    bool constrainedIntraPrediction = false;
    bool loopFilterAcrossSlices = true;

    int log2MaxPicOrderCntLsb = 8;
    int decodedPictureBuffer = 2; // pictures: the references held, plus the current one
    int defaultActiveReferences = 1;
};

/// Each returns one NAL unit, header included, emulation prevention applied, no
/// start code.
std::vector<uint8_t> hevcVps(const HevcStreamShape& shape);
std::vector<uint8_t> hevcSps(const HevcStreamShape& shape);
std::vector<uint8_t> hevcPps(const HevcStreamShape& shape);

/// The three of them as Annex-B, in the order a decoder needs them — what goes
/// ahead of every keyframe.
std::vector<uint8_t> hevcParameterSets(const HevcStreamShape& shape);

} // namespace mw::native::encode
