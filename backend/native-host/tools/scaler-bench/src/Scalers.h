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

#include <string>
#include <vector>

namespace bench {

/// One candidate: a filter, a colour space to run it in, a precision.
struct Variant
{
    std::string id;
    /// The filter, independent of space and precision — what the report
    /// groups by.
    std::string family;
    std::string label;
    /// Shown in the report: what this candidate is and what to expect.
    std::string note;

    Kind kind = Kind::Pixel;
    Space space = Space::Perceptual;
    Precision precision = Precision::Fp32;
    bool sdr = true;
    bool hdr = true;

    /// Shader file under the shaders directory, and its entry point.
    std::string file;
    std::string entry;
    bool fp16 = false;

    // filters.hlsl parameters.
    int kernel = 0; // 0 cubic, 1 Lanczos
    int radius = 2;
    double cubicB = 0.0;
    double cubicC = 0.5;
    bool dilated = false;

    // fsr1.hlsl: run RCAS after EASU.
    bool rcas = false;

    // Video processor: D3D11_VIDEO_USAGE, and stream auto-processing.
    int vpUsage = 0;
    bool vpAuto = false;

    /// Whether crops are saved for this candidate (fp16 twins add nothing
    /// visible; the report keeps the disk for the fp32 ones).
    bool crops = true;
};

/// Every candidate, in the order the report lists them.
std::vector<Variant> allVariants();

} // namespace bench
