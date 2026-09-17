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

#include "Reference.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace bench::reference {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kLobes = 3;

double lanczos(double x)
{
    x = std::abs(x);
    if (x < 1e-9) return 1.0;
    if (x >= kLobes) return 0.0;
    const double px = kPi * x;
    return kLobes * std::sin(px) * std::sin(px / kLobes) / (px * px);
}

/// The taps of one output row/column: which source indices and what weights.
struct Taps
{
    int first = 0;
    std::vector<double> weights;
};

std::vector<Taps> buildTaps(int srcN, int dstN)
{
    const double scale = static_cast<double>(srcN) / dstN;
    const double dilate = std::max(1.0, scale);
    const double reach = kLobes * dilate;
    std::vector<Taps> taps(dstN);
    for (int d = 0; d < dstN; ++d) {
        // Output pixel centre in source coordinates; source centres on integers.
        const double centre = (d + 0.5) * scale - 0.5;
        const int first = static_cast<int>(std::ceil(centre - reach));
        const int last = static_cast<int>(std::floor(centre + reach));
        Taps& t = taps[d];
        t.first = first;
        t.weights.resize(last - first + 1);
        double sum = 0.0;
        for (int i = first; i <= last; ++i) {
            const double w = lanczos((i - centre) / dilate);
            t.weights[i - first] = w;
            sum += w;
        }
        for (double& w : t.weights)
            w /= sum;
    }
    return taps;
}

void parallelRows(int rows, const std::function<void(int, int)>& fn)
{
    const int threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    const int chunk = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int y0 = t * chunk;
        const int y1 = std::min(rows, y0 + chunk);
        if (y0 >= y1) break;
        pool.emplace_back(fn, y0, y1);
    }
    for (auto& th : pool)
        th.join();
}

} // namespace

ImageF downscale(const ImageF& linear, int dstW, int dstH)
{
    // Horizontal pass into a srcH × dstW intermediate, kept in double.
    const std::vector<Taps> tx = buildTaps(linear.w, dstW);
    std::vector<double> mid(static_cast<size_t>(linear.h) * dstW * 3, 0.0);
    parallelRows(linear.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* row = linear.at(0, y);
            for (int x = 0; x < dstW; ++x) {
                const Taps& t = tx[x];
                double acc[3] = {0, 0, 0};
                for (size_t k = 0; k < t.weights.size(); ++k) {
                    const int sx = std::clamp(t.first + static_cast<int>(k), 0, linear.w - 1);
                    const double w = t.weights[k];
                    acc[0] += w * row[sx * 3 + 0];
                    acc[1] += w * row[sx * 3 + 1];
                    acc[2] += w * row[sx * 3 + 2];
                }
                double* o = &mid[(static_cast<size_t>(y) * dstW + x) * 3];
                o[0] = acc[0];
                o[1] = acc[1];
                o[2] = acc[2];
            }
        }
    });

    // Vertical pass.
    const std::vector<Taps> ty = buildTaps(linear.h, dstH);
    ImageF out(dstW, dstH);
    parallelRows(dstH, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Taps& t = ty[y];
            for (int x = 0; x < dstW; ++x) {
                double acc[3] = {0, 0, 0};
                for (size_t k = 0; k < t.weights.size(); ++k) {
                    const int sy = std::clamp(t.first + static_cast<int>(k), 0, linear.h - 1);
                    const double w = t.weights[k];
                    const double* m = &mid[(static_cast<size_t>(sy) * dstW + x) * 3];
                    acc[0] += w * m[0];
                    acc[1] += w * m[1];
                    acc[2] += w * m[2];
                }
                float* o = out.at(x, y);
                // Lanczos undershoots below zero on hard edges; light does not.
                o[0] = static_cast<float>(std::max(0.0, acc[0]));
                o[1] = static_cast<float>(std::max(0.0, acc[1]));
                o[2] = static_cast<float>(std::max(0.0, acc[2]));
            }
        }
    });
    return out;
}

} // namespace bench::reference
