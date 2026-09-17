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

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/// The vocabulary shared by every part of the bench.
namespace bench {

struct Size
{
    int w = 0;
    int h = 0;
};

/// SDR: an 8-bit sRGB desktop. HDR: an FP16 scRGB one, as DXGI hands it over.
enum class Range
{
    Sdr,
    Hdr
};

/// In which space the filter's arithmetic happens. Perceptual is the encoded
/// signal (sRGB, or PQ for HDR) — what the host samples today. Linear is
/// light. Driver: not ours to know (the video processor).
enum class Space
{
    Perceptual,
    Linear,
    Driver
};

enum class Precision
{
    Fp32,
    Fp16,
    Driver
};

/// How a candidate is executed.
enum class Kind
{
    /// Full-screen triangle, pixel shader, viewport = output.
    Pixel,
    /// Same, after GenerateMips on the source; trilinear sampler.
    Mip,
    /// Compute dispatch writing a UAV (FSR, NIS).
    Compute,
    /// ID3D11VideoProcessor::VideoProcessorBlt.
    VideoProcessor
};

const char* toString(Range r);
const char* toString(Space s);
const char* toString(Precision p);
const char* toString(Kind k);

/// One source → output pair.
struct Case
{
    std::string id;
    std::string label;
    Size src;
    Size dst;
    /// Where the source picture came from, empty when no picture of that
    /// size was available (the case is then skipped and reported as such).
    std::string sourcePath;
};

/// A planar-free float picture, RGB interleaved, whatever the encoding.
struct ImageF
{
    int w = 0;
    int h = 0;
    std::vector<float> px;

    ImageF() = default;
    ImageF(int width, int height)
        : w(width)
        , h(height)
        , px(static_cast<size_t>(width) * height * 3, 0.0f)
    {}

    float* at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
    const float* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 3]; }
};

struct ImageRgba8
{
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;
};

struct TimeStats
{
    /// Mean of the central 80 % — the figure the report leads with.
    double trimmedUs = 0;
    double medianUs = 0;
    double p95Us = 0;
    double minUs = 0;
    int n = 0;
    /// Batches thrown away because the GPU clock changed under them.
    int discarded = 0;
    /// "gpu-timestamp" for the shader passes; "wall-clock" for the video
    /// processor, whose Blt runs on a queue the 3D timestamps do not see
    /// (0 µs on NVIDIA and Intel, 17/09/2026): there the figure is the CPU
    /// time from the Blt call to the GPU's completion event, which includes
    /// the submission — a different, larger, kind of number.
    std::string method = "gpu-timestamp";
};

struct Quality
{
    /// Against the CPU reference (Lanczos-3, linear light), in the encoded domain.
    double psnrY = 0;
    double psnrRgb = 0;
    double ssimY = 0;
    /// The scroll test. The source moves by one pixel; the candidate's
    /// frame-to-frame difference D is projected onto the reference's R:
    /// D = gain · R + E. `motionGain` is that gain (1 = the motion of the
    /// ideal filter, below = blur), `flicker` is |E|² / |R|² — the part of
    /// the change that the picture's motion does not explain, i.e. aliasing
    /// and shimmer, what the encoder pays for and the eye sees on scrolling
    /// text (0 = none). `temporalPsnr` is the PSNR between D and R.
    double flicker = 0;
    double motionGain = 0;
    double temporalPsnr = 0;
    /// Energy above 0.8 × output Nyquist over total AC energy: sharpness or
    /// aliasing, the phase figure tells which.
    double hfRatio = 0;
};

struct GpuInfo
{
    int index = 0;
    std::string name;
    uint64_t luid = 0;
    unsigned vendorId = 0;
    unsigned deviceId = 0;
    std::string driver;
    std::string featureLevel;
    bool fp16 = false;
    bool displayGpu = false;
    uint64_t vramMb = 0;
};

struct Result
{
    int gpu = 0;
    std::string caseId;
    Range range = Range::Sdr;
    std::string variant;
    /// "ok", "unsupported" (with reason), "failed" (with reason).
    std::string status;
    std::string reason;
    bool hasTime = false;
    TimeStats time;
    bool hasQuality = false;
    Quality quality;
    /// Relative paths of the crop images, one per region, empty when none.
    std::vector<std::string> crops;
    /// Free-form notes the pass wants shown (e.g. which colour space the
    /// driver accepted).
    std::string notes;
};

struct CropRegion
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string tag;
};

/// What is known about one (case, range) input, for the report's header.
struct SourceInfo
{
    std::string caseId;
    Range range = Range::Sdr;
    std::string description;
    std::vector<CropRegion> regions;
    std::vector<std::string> referenceCrops;
    std::vector<std::string> sourceCrops;
    double referenceHf = 0;
};

} // namespace bench
