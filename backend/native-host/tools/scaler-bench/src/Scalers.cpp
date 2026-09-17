/*
 * MoonlightWeb — native capture & encoding engine: scaler bench.
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

#include "Scalers.h"

#include <d3d11.h>

namespace bench {

namespace {

struct Family
{
    const char* id;
    const char* label;
    const char* note;
    Kind kind;
    const char* entry;
    int kernel;
    int radius;
    double B;
    double C;
    bool dilated;
    /// Whether an fp16 twin makes sense (there is arithmetic to halve).
    bool halfable;
};

// The classic filters, every one of them from filters.hlsl.
const Family kClassic[] = {
    {"bilinear", "Bilinear",
     "One hardware fetch, no mipmaps — the host today (ColorConvert.cpp). A 2-texel tent "
     "whatever the ratio: no low-pass, so it aliases below 1:1.",
     Kind::Pixel, "PsBilinear", 0, 2, 0, 0, false, false},
    {"mip", "Bilinear + mipmaps",
     "GenerateMips then a trilinear fetch: the cheapest hardware pre-filter there is (box "
     "2x2 per level, blended between levels). The GenerateMips is in the time.",
     Kind::Mip, "PsBilinear", 0, 2, 0, 0, false, false},
    {"catmullrom9", "Catmull-Rom (9 fetches)",
     "4x4 cubic B=0 C=1/2 in 9 bilinear fetches. Fixed radius: a reconstruction filter, "
     "sharp, slight ringing; not a low-pass.",
     Kind::Pixel, "PsCubic9", 0, 2, 0.0, 0.5, false, true},
    {"mitchell9", "Mitchell (9 fetches)",
     "4x4 cubic B=C=1/3 in 9 bilinear fetches. Fixed radius, softer than Catmull-Rom, "
     "less ringing.",
     Kind::Pixel, "PsCubic9", 0, 2, 1.0 / 3.0, 1.0 / 3.0, false, true},
    {"catmullrom-dilated", "Catmull-Rom (dilated)",
     "The cubic stretched to the ratio and every tap loaded: a true low-pass at any ratio.",
     Kind::Pixel, "PsDirect", 0, 2, 0.0, 0.5, true, true},
    {"mitchell-dilated", "Mitchell (dilated)", "Mitchell stretched to the ratio, every tap loaded.",
     Kind::Pixel, "PsDirect", 0, 2, 1.0 / 3.0, 1.0 / 3.0, true, true},
    {"lanczos2", "Lanczos-2 (fixed)",
     "4x4 Lanczos, every tap loaded, fixed radius: reconstruction only.", Kind::Pixel, "PsDirect",
     1, 2, 0, 0, false, true},
    {"lanczos2-dilated", "Lanczos-2 (dilated)", "Lanczos-2 stretched to the ratio.", Kind::Pixel,
     "PsDirect", 1, 2, 0, 0, true, true},
    {"lanczos3", "Lanczos-3 (fixed)", "6x6 Lanczos, fixed radius: the sharpest, the most ringing.",
     Kind::Pixel, "PsDirect", 1, 3, 0, 0, false, true},
    {"lanczos3-dilated", "Lanczos-3 (dilated)",
     "Lanczos-3 stretched to the ratio — the GPU cousin of the CPU reference, in one pass "
     "and single precision.",
     Kind::Pixel, "PsDirect", 1, 3, 0, 0, true, true},
};

Variant classic(const Family& f, Space space, bool fp16)
{
    Variant v;
    v.family = f.id;
    v.id = std::string(f.id) + (space == Space::Linear ? "-linear" : "-perceptual") +
           (fp16 ? "-fp16" : "");
    v.label = f.label;
    v.note = f.note;
    v.kind = f.kind;
    v.space = space;
    v.precision = fp16 ? Precision::Fp16 : Precision::Fp32;
    v.file = "filters.hlsl";
    v.entry = f.entry;
    v.fp16 = fp16;
    v.kernel = f.kernel;
    v.radius = f.radius;
    v.cubicB = f.B;
    v.cubicC = f.C;
    v.dilated = f.dilated;
    v.crops = !fp16;
    return v;
}

} // namespace

std::vector<Variant> allVariants()
{
    std::vector<Variant> out;
    for (const Family& f : kClassic) {
        for (Space space : {Space::Perceptual, Space::Linear}) {
            out.push_back(classic(f, space, false));
            if (f.halfable) out.push_back(classic(f, space, true));
        }
    }

    // The vendor upscalers, perceptual input only, as their headers require.
    for (bool fp16 : {false, true}) {
        Variant easu;
        easu.family = "fsr1-easu";
        easu.id = std::string("fsr1-easu") + (fp16 ? "-fp16" : "");
        easu.label = "FSR 1.0 EASU";
        easu.note = "AMD FidelityFX Super Resolution 1.0, EASU pass alone. An UPSCALER by "
                    "contract (\"1x to 4x area\"): run at a reduction ratio as asked; the "
                    "figures say what it does there.";
        easu.kind = Kind::Compute;
        easu.space = Space::Perceptual;
        easu.precision = fp16 ? Precision::Fp16 : Precision::Fp32;
        easu.file = "fsr1.hlsl";
        easu.entry = "CsMain";
        easu.fp16 = fp16;
        easu.crops = !fp16;
        out.push_back(easu);

        Variant rcas = easu;
        rcas.family = "fsr1-easu-rcas";
        rcas.id = std::string("fsr1-easu-rcas") + (fp16 ? "-fp16" : "");
        rcas.label = "FSR 1.0 EASU + RCAS";
        rcas.note = "EASU then RCAS (sharpness 0.2), two dispatches, both in the time — the "
                    "way FSR1 ships.";
        rcas.rcas = true;
        out.push_back(rcas);

        Variant nis;
        nis.family = "nis";
        nis.id = std::string("nis") + (fp16 ? "-fp16" : "");
        nis.label = "NVIDIA Image Scaling";
        nis.note = "NIS 1.0.3 NVScaler. Its own config function accepts scales of 1x..2x "
                   "(upscale) only and its shared-memory tile is sized for that; a reduction "
                   "ratio is refused by the SDK and reported as unsupported, not run.";
        nis.kind = Kind::Compute;
        nis.space = Space::Perceptual;
        nis.precision = fp16 ? Precision::Fp16 : Precision::Fp32;
        nis.file = "nis.hlsl";
        nis.entry = "CsMain";
        nis.fp16 = fp16;
        nis.crops = !fp16;
        out.push_back(nis);

        Variant sgsr;
        sgsr.family = "sgsr1";
        sgsr.id = std::string("sgsr1") + (fp16 ? "-fp16" : "");
        sgsr.label = "Snapdragon GSR 1";
        sgsr.note = "Qualcomm SGSR 1: 12-tap Lanczos-2-like with edge-adaptive sharpening, "
                    "one pass. An upscaler with no low-pass; run at a reduction as asked.";
        sgsr.kind = Kind::Pixel;
        sgsr.space = Space::Perceptual;
        sgsr.precision = fp16 ? Precision::Fp16 : Precision::Fp32;
        sgsr.file = "sgsr1.hlsl";
        sgsr.entry = "PsMain";
        sgsr.fp16 = fp16;
        sgsr.crops = !fp16;
        out.push_back(sgsr);
    }

    // The driver's own scaler.
    struct Vp
    {
        const char* id;
        const char* label;
        const char* note;
        int usage;
        bool autoProcessing;
    };
    const Vp vps[] = {
        {"vp-normal", "VideoProcessor (playback)",
         "ID3D11VideoProcessor::VideoProcessorBlt, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL, "
         "auto-processing off. The driver picks the filter; nothing here can say which.",
         D3D11_VIDEO_USAGE_PLAYBACK_NORMAL, false},
        {"vp-quality", "VideoProcessor (optimal quality)",
         "Same, D3D11_VIDEO_USAGE_OPTIMAL_QUALITY.", D3D11_VIDEO_USAGE_OPTIMAL_QUALITY, false},
        {"vp-auto", "VideoProcessor (auto-processing)",
         "PLAYBACK_NORMAL with the stream's auto-processing left ON: whatever the driver's "
         "\"enhancements\" are (super-resolution included where the vendor ships one).",
         D3D11_VIDEO_USAGE_PLAYBACK_NORMAL, true},
    };
    for (const Vp& vp : vps) {
        Variant v;
        v.family = vp.id;
        v.id = vp.id;
        v.label = vp.label;
        v.note = vp.note;
        v.kind = Kind::VideoProcessor;
        v.space = Space::Driver;
        v.precision = Precision::Driver;
        v.vpUsage = vp.usage;
        v.vpAuto = vp.autoProcessing;
        out.push_back(v);
    }
    return out;
}

} // namespace bench
