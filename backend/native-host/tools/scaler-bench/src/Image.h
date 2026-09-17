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

/// Pictures in and out: WIC for PNG, and the transfer functions the bench
/// needs on the CPU side (sRGB, PQ, half floats), written once so that the
/// reference, the uploads and the readbacks all agree on them.
namespace bench::image {

bool loadPng(const std::wstring& path, ImageRgba8& out, std::string& error);
bool savePng(const std::wstring& path, const ImageRgba8& img, std::string& error);

float srgbToLinear(float c);
float linearToSrgb(float c);

/// 8-bit sRGB → linear light, 0..1.
ImageF decodeSrgb(const ImageRgba8& img);
/// Linear light → sRGB-encoded floats (no quantisation).
ImageF srgbEncode(const ImageF& linear);
ImageF srgbDecode(const ImageF& encoded);

/// scRGB (linear, BT.709 primaries, 1.0 = 80 nits) → BT.2020 PQ code, 0..1.
/// The same four steps as the host's PsLumaHdr, so what the bench scores is
/// what the encoder would have seen.
ImageF pqEncode(const ImageF& scRgb);

/// The bench's HDR input, made from the SDR picture: linear light with SDR
/// white at 1.0, the brightest 2 % pushed up to 10.0 on a ramp, and four
/// synthetic patches (4, 8, 12 and a 1-pixel grid at 8 on black) along the
/// bottom edge for ringing and half-precision to bite on. Deterministic.
ImageF synthesizeHdr(const ImageF& linear, std::string& description);

/// `out(x, y) = in(x - dx, y - dy)`, edges clamped.
ImageF shifted(const ImageF& in, int dx, int dy);

uint16_t floatToHalf(float f);
float halfToFloat(uint16_t h);

/// Upload helpers: interleaved RGBA with alpha = 1.
std::vector<uint8_t> toRgba8(const ImageF& encoded);
std::vector<uint16_t> toRgba16Unorm(const ImageF& encoded);
std::vector<uint16_t> toRgba16Half(const ImageF& linear);

/// A window of an encoded picture as 8-bit, for the report's crops.
ImageRgba8 crop8(const ImageF& encoded, int x, int y, int w, int h);

} // namespace bench::image
