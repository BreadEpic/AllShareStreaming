/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/H264Vui.h"
#include "native_test_framework.h"

#include <string>

using namespace mw::native::encode;

namespace {

std::vector<uint8_t> fromHex(const char* s)
{
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2)
        v.push_back(static_cast<uint8_t>(std::stoi(std::string(s, 2), nullptr, 16)));
    return v;
}

std::vector<uint8_t> rewrite(const std::vector<uint8_t>& sps)
{
    return h264WithNoReorderVui(sps.data(), sps.size());
}

} // namespace

void run_h264_vui_tests()
{
    SECTION("H264Vui — VideoToolbox's SPS gains a VUI that says nothing is reordered");
    {
        // Taken from bench-mac's encoder (21/09/2026): High, level 4.2,
        // 1662×1080, max_num_ref_frames 1, and no VUI at all. The expected
        // bytes were checked by a separate parser: everything up to the VUI
        // flag is unchanged, then bitstream_restriction with
        // max_num_reorder_frames 0 and max_dec_frame_buffering 1. Chrome's
        // hardware decoder held 4 frames of the original and none of this.
        const auto in = fromHex("2764002aac5680680227a950");
        const auto out = rewrite(in);
        CHECK(out == fromHex("2764002aac5680680227a9601b41008540"));
        // The NAL header (nal_ref_idc and type) is the input's.
        CHECK_EQ(out[0], in[0]);
    }

    SECTION("H264Vui — an existing VUI is kept, only its tail is replaced");
    {
        // A VUI with a colour description (BT.709) and no restriction: the
        // colour must survive, bit for bit, ahead of the new restriction.
        const auto in = fromHex("67640028ac2b401e0089f97016a0202020780d0a");
        const auto out = rewrite(in);
        CHECK(out == fromHex("67640028ac2b401e0089f97016a0202020da08042a"));
    }

    SECTION("H264Vui — a rewritten SPS rewrites to itself");
    {
        // What makes it safe to apply to an SPS that already says the right
        // thing — a future VideoToolbox that grows the VUI on its own.
        const auto once = rewrite(fromHex("2764002aac5680680227a950"));
        CHECK(rewrite(once) == once);
    }

    SECTION("H264Vui — what it cannot read, it leaves alone");
    {
        // Not an SPS (a PPS), too short, or cut before the VUI: empty, so the
        // caller sends the original rather than a guess.
        CHECK(rewrite(fromHex("28ee3cb0")).empty());
        CHECK(rewrite(fromHex("2764")).empty());
        CHECK(rewrite(fromHex("2764002aac56")).empty());
        CHECK(h264WithNoReorderVui(nullptr, 0).empty());
    }
}
