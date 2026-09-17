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

#include "Bench.h"

#include <vector>

/// How a candidate's output is scored. Everything here works on ENCODED
/// pictures (sRGB values for SDR, PQ codes for HDR): that is the perceptually
/// even domain, and also the one the encoder is handed.
namespace bench::metrics {

/// Luma weights for the encoded domain: BT.709 on sRGB, BT.2020 on PQ.
const float* lumaWeights(Range range);

/// PSNR in dB over [x0, x1) × [y0, y1). 99 caps a perfect match.
double psnr(const ImageF& a, const ImageF& b, bool lumaOnly, const float* luma, int x0, int y0,
            int x1, int y1);

/// Mean SSIM on luma, 11-tap Gaussian window (σ 1.5), the usual constants.
double ssimLuma(const ImageF& a, const ImageF& b, const float* luma);

/// Energy of the luma above 0.8 × Nyquist over the total AC energy, by a
/// separable high-pass. Reference-free.
double hfRatio(const ImageF& a, const float* luma);

/// The scroll test. `out` / `outShifted` are the candidate's outputs for the
/// source and the source moved by one pixel; `ref` / `refShifted` the
/// reference's. The candidate's luma difference D is projected onto the
/// reference's R: D = gain · R + E. Fills `motionGain` (gain), `flicker`
/// (|E|² / |R|²) and `temporalPsnr` (PSNR between D and R). A margin keeps
/// the clamped edge out of it.
void temporal(const ImageF& out, const ImageF& outShifted, const ImageF& ref,
              const ImageF& refShifted, const float* luma, double& flicker, double& motionGain,
              double& temporalPsnr);

/// Three windows of the reference worth looking at: the busiest edges (text,
/// UI lines), the busiest fine texture, and the centre. Deterministic.
std::vector<CropRegion> pickCropRegions(const ImageF& reference, int cw, int ch, const float* luma);

} // namespace bench::metrics
