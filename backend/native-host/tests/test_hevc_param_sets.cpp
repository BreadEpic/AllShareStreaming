/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/H264Vui.h"
#include "encode/HevcParamSets.h"
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

// What the Arc A380's D3D12 encoder was created with on 21/09/2026: 8..64
// coding blocks, 4..32 transforms, depth 2, AMP (which that driver requires),
// 1080 lines coded as 1088.
HevcStreamShape arcShape()
{
    HevcStreamShape shape;
    shape.width = 1920;
    shape.height = 1080;
    shape.codedWidth = 1920;
    shape.codedHeight = 1088;
    shape.asymmetricMotionPartitions = true;
    return shape;
}

struct SpsHead
{
    uint32_t chroma = 0, width = 0, height = 0, cropRight = 0, cropBottom = 0, lumaDepth = 0;
    bool cropped = false, overrun = false;
};

SpsHead parseSps(const std::vector<uint8_t>& nal)
{
    const std::vector<uint8_t> rbsp = h264vui_detail::unescape(nal.data() + 2, nal.size() - 2);
    h264vui_detail::BitReader r{rbsp};
    r.u(8);  // vps id, max_sub_layers_minus1, temporal_id_nesting
    r.u(32); // profile_tier_level: 96 bits with no sub-layers
    r.u(32);
    r.u(32);
    r.ue(); // sps_seq_parameter_set_id
    SpsHead h;
    h.chroma = r.ue();
    h.width = r.ue();
    h.height = r.ue();
    h.cropped = r.u(1) != 0;
    if (h.cropped) {
        r.ue();
        h.cropRight = r.ue();
        r.ue();
        h.cropBottom = r.ue();
    }
    h.lumaDepth = r.ue() + 8;
    h.overrun = r.overrun;
    return h;
}

} // namespace

void run_hevc_param_sets_tests()
{
    SECTION("HevcParamSets — the bytes a real decoder accepted over the Arc's slices");
    {
        // These three went ahead of 800 frames of the Arc's D3D12 encoder
        // (CBR, infinite GOP) and ffmpeg 9.0 decoded the lot without a
        // warning, P frames included — which is the only proof there is that
        // the PPS says what the driver's slice headers assume.
        const HevcStreamShape shape = arcShape();
        CHECK(hevcVps(shape) == fromHex("40010c01ffff016000000300b000000300000300992c09"));
        CHECK(hevcSps(shape) ==
              fromHex("420101016000000300b00000030000030099a003c0801107cb94b9246d226a"
                      "02020201"));
        CHECK(hevcPps(shape) == fromHex("4401c0f3e0cc90"));
    }

    SECTION("HevcParamSets — Annex-B, in the order a decoder needs");
    {
        const std::vector<uint8_t> all = hevcParameterSets(arcShape());
        const std::vector<uint8_t> startCode = {0, 0, 0, 1};
        size_t at = 0;
        for (uint8_t type : {uint8_t(32), uint8_t(33), uint8_t(34)}) {
            CHECK(at + 5 < all.size());
            CHECK(std::vector<uint8_t>(all.begin() + at, all.begin() + at + 4) == startCode);
            CHECK_EQ(int(all[at + 4] >> 1), int(type));
            // Next start code, or the end.
            size_t next = at + 4;
            while (next + 4 <= all.size() && !(all[next] == 0 && all[next + 1] == 0 &&
                                               all[next + 2] == 0 && all[next + 3] == 1))
                ++next;
            at = next + 4 <= all.size() ? next : all.size();
        }
        CHECK_EQ(at, all.size());
    }

    SECTION("HevcParamSets — the viewer's picture is cropped out of the coded one");
    {
        const SpsHead h = parseSps(hevcSps(arcShape()));
        CHECK(!h.overrun);
        CHECK_EQ(h.chroma, 1u);
        CHECK_EQ(h.width, 1920u);
        CHECK_EQ(h.height, 1088u);
        CHECK(h.cropped);
        CHECK_EQ(h.cropRight, 0u);
        CHECK_EQ(h.cropBottom, 4u); // chroma rows: 8 luma lines
        CHECK_EQ(h.lumaDepth, 8u);
    }

    SECTION("HevcParamSets — no crop window when the sizes agree, 10 bits when asked");
    {
        HevcStreamShape shape = arcShape();
        shape.width = shape.codedWidth = 2560;
        shape.height = shape.codedHeight = 1440;
        shape.tenBit = true;
        shape.hdr = true;
        const std::vector<uint8_t> sps = hevcSps(shape);
        const SpsHead h = parseSps(sps);
        CHECK(!h.overrun);
        CHECK(!h.cropped);
        CHECK_EQ(h.lumaDepth, 10u);
        // general_profile_idc: Main 10.
        CHECK_EQ(int(sps[3] & 0x1F), 2);
        // BT.2020, PQ, BT.2020 NCL travel as three consecutive bytes somewhere
        // in the VUI — byte-aligned or not, so look for them bit by bit.
        const std::vector<uint8_t> rbsp = h264vui_detail::unescape(sps.data() + 2, sps.size() - 2);
        bool found = false;
        for (size_t bit = 0; bit + 24 <= rbsp.size() * 8 && !found; ++bit) {
            h264vui_detail::BitReader r{rbsp, bit};
            found = r.u(8) == 9 && r.u(8) == 16 && r.u(8) == 9;
        }
        CHECK(found);
    }

    SECTION("HevcParamSets — no start code can appear inside a unit");
    {
        // The profile_tier_level alone holds 44 zero bits in a row: without
        // the escape, that is a start code in the middle of the VPS.
        HevcStreamShape shape = arcShape();
        for (const auto& unit : {hevcVps(shape), hevcSps(shape), hevcPps(shape)})
            for (size_t i = 0; i + 2 < unit.size(); ++i)
                CHECK(!(unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] <= 2));
    }
}
