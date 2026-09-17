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

/// The picture every candidate is scored against.
namespace bench::reference {

/// Separable Lanczos-3 in double precision, the kernel dilated by the
/// reduction ratio so that it is a true low-pass at any ratio, applied to
/// LINEAR light. That is the textbook answer to "what should a downscale
/// produce"; the candidates are measured by their distance from it. It
/// favours no candidate: none of them is a dilated Lanczos-3 in linear light
/// on the GPU (the closest, `lanczos3-dilated-linear`, is single precision
/// and single-pass, and the scores show how close it gets).
ImageF downscale(const ImageF& linear, int dstW, int dstH);

} // namespace bench::reference
