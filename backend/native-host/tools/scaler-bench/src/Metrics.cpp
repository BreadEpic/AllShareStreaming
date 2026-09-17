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

#include "Metrics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>

namespace bench::metrics {

namespace {

constexpr double kPsnrCap = 99.0;

std::vector<float> lumaPlane(const ImageF& a, const float* w)
{
    const size_t n = static_cast<size_t>(a.w) * a.h;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i) {
        const float* p = &a.px[i * 3];
        y[i] = w[0] * p[0] + w[1] * p[1] + w[2] * p[2];
    }
    return y;
}

/// Separable convolution of a plane with a symmetric 1-D kernel, edges
/// clamped, rows in parallel.
std::vector<float> blur(const std::vector<float>& in, int w, int h, const std::vector<float>& k)
{
    const int r = static_cast<int>(k.size() / 2);
    std::vector<float> tmp(in.size());
    std::vector<float> out(in.size());
    const int threads = std::max(1u, std::thread::hardware_concurrency());
    auto rows = [&](const std::function<void(int)>& fn) {
        std::vector<std::thread> pool;
        const int chunk = (h + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            const int y0 = t * chunk;
            const int y1 = std::min(h, y0 + chunk);
            if (y0 >= y1) break;
            pool.emplace_back([&fn, y0, y1] {
                for (int y = y0; y < y1; ++y)
                    fn(y);
            });
        }
        for (auto& th : pool)
            th.join();
    };
    rows([&](int y) {
        const float* row = &in[static_cast<size_t>(y) * w];
        float* o = &tmp[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i)
                acc += k[i + r] * row[std::clamp(x + i, 0, w - 1)];
            o[x] = acc;
        }
    });
    rows([&](int y) {
        float* o = &out[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i)
                acc += k[i + r] * tmp[static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w + x];
            o[x] = acc;
        }
    });
    return out;
}

std::vector<float> gaussian(int radius, float sigma)
{
    std::vector<float> k(2 * radius + 1);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        k[i + radius] = std::exp(-0.5f * (i * i) / (sigma * sigma));
        sum += k[i + radius];
    }
    for (float& v : k)
        v /= sum;
    return k;
}

} // namespace

const float* lumaWeights(Range range)
{
    static const float k709[3] = {0.2126f, 0.7152f, 0.0722f};
    static const float k2020[3] = {0.2627f, 0.6780f, 0.0593f};
    return range == Range::Hdr ? k2020 : k709;
}

double psnr(const ImageF& a, const ImageF& b, bool lumaOnly, const float* luma, int x0, int y0,
            int x1, int y1)
{
    double se = 0.0;
    size_t n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float* p = a.at(x, y);
            const float* q = b.at(x, y);
            if (lumaOnly) {
                const double ya = luma[0] * p[0] + luma[1] * p[1] + luma[2] * p[2];
                const double yb = luma[0] * q[0] + luma[1] * q[1] + luma[2] * q[2];
                se += (ya - yb) * (ya - yb);
                n++;
            } else {
                for (int c = 0; c < 3; ++c)
                    se += static_cast<double>(p[c] - q[c]) * (p[c] - q[c]);
                n += 3;
            }
        }
    }
    if (n == 0) return 0.0;
    const double mse = se / n;
    if (mse < 1e-20) return kPsnrCap;
    return std::min(kPsnrCap, 10.0 * std::log10(1.0 / mse));
}

double ssimLuma(const ImageF& a, const ImageF& b, const float* luma)
{
    const int w = a.w;
    const int h = a.h;
    const std::vector<float> ya = lumaPlane(a, luma);
    const std::vector<float> yb = lumaPlane(b, luma);
    const std::vector<float> k = gaussian(5, 1.5f);

    std::vector<float> aa(ya.size());
    std::vector<float> bb(ya.size());
    std::vector<float> ab(ya.size());
    for (size_t i = 0; i < ya.size(); ++i) {
        aa[i] = ya[i] * ya[i];
        bb[i] = yb[i] * yb[i];
        ab[i] = ya[i] * yb[i];
    }
    const std::vector<float> mua = blur(ya, w, h, k);
    const std::vector<float> mub = blur(yb, w, h, k);
    const std::vector<float> saa = blur(aa, w, h, k);
    const std::vector<float> sbb = blur(bb, w, h, k);
    const std::vector<float> sab = blur(ab, w, h, k);

    constexpr double c1 = 0.01 * 0.01;
    constexpr double c2 = 0.03 * 0.03;
    double sum = 0.0;
    for (size_t i = 0; i < ya.size(); ++i) {
        const double ma = mua[i];
        const double mb = mub[i];
        const double va = saa[i] - ma * ma;
        const double vb = sbb[i] - mb * mb;
        const double cov = sab[i] - ma * mb;
        sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
    }
    return sum / static_cast<double>(ya.size());
}

double hfRatio(const ImageF& a, const float* luma)
{
    const std::vector<float> y = lumaPlane(a, luma);
    // A Gaussian of σ 0.8 passes little above 0.8 × Nyquist; what it removes
    // is the top of the band.
    const std::vector<float> low = blur(y, a.w, a.h, gaussian(3, 0.8f));
    double mean = 0.0;
    for (float v : y)
        mean += v;
    mean /= static_cast<double>(y.size());
    double ac = 0.0;
    double hf = 0.0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double d = y[i] - mean;
        ac += d * d;
        const double e = y[i] - low[i];
        hf += e * e;
    }
    return ac > 0 ? hf / ac : 0.0;
}

void temporal(const ImageF& out, const ImageF& outShifted, const ImageF& ref,
              const ImageF& refShifted, const float* luma, double& flicker, double& motionGain,
              double& temporalPsnr)
{
    constexpr int kMargin = 8;
    double dotDR = 0.0;
    double energyD = 0.0;
    double energyR = 0.0;
    double se = 0.0;
    size_t n = 0;
    auto lumaOf = [&](const float* p) { return luma[0] * p[0] + luma[1] * p[1] + luma[2] * p[2]; };
    for (int y = kMargin; y < out.h - kMargin; ++y) {
        for (int x = kMargin; x < out.w - kMargin; ++x) {
            const double d = lumaOf(outShifted.at(x, y)) - lumaOf(out.at(x, y));
            const double r = lumaOf(refShifted.at(x, y)) - lumaOf(ref.at(x, y));
            dotDR += d * r;
            energyD += d * d;
            energyR += r * r;
            se += (d - r) * (d - r);
            n++;
        }
    }
    motionGain = energyR > 0 ? dotDR / energyR : 0.0;
    // |E|² = |D|² − gain² |R|², the residual of the least-squares fit.
    const double residual = std::max(0.0, energyD - motionGain * motionGain * energyR);
    flicker = energyR > 0 ? residual / energyR : 0.0;
    const double mse = n ? se / n : 0.0;
    temporalPsnr = mse < 1e-20 ? kPsnrCap : std::min(kPsnrCap, 10.0 * std::log10(1.0 / mse));
}

std::vector<CropRegion> pickCropRegions(const ImageF& reference, int cw, int ch, const float* luma)
{
    const int w = reference.w;
    const int h = reference.h;
    const std::vector<float> y = lumaPlane(reference, luma);
    const std::vector<float> low = blur(y, w, h, gaussian(3, 0.8f));

    // Tile the picture; score each tile by gradient energy (edges) and by
    // high-pass energy (fine texture).
    const int cols = std::max(1, w / cw);
    const int rows = std::max(1, h / ch);
    struct Tile
    {
        int x, y;
        double edges, texture;
    };
    std::vector<Tile> tiles;
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            Tile t{tx * cw, ty * ch, 0.0, 0.0};
            for (int yy = t.y + 1; yy < std::min(h, t.y + ch) - 1; ++yy) {
                for (int xx = t.x + 1; xx < std::min(w, t.x + cw) - 1; ++xx) {
                    const size_t i = static_cast<size_t>(yy) * w + xx;
                    const double gx = y[i + 1] - y[i - 1];
                    const double gy = y[i + w] - y[i - w];
                    t.edges += gx * gx + gy * gy;
                    const double e = y[i] - low[i];
                    t.texture += e * e;
                }
            }
            tiles.push_back(t);
        }
    }
    std::vector<CropRegion> out;
    auto take = [&](const std::string& tag, const std::function<double(const Tile&)>& score) {
        const Tile* best = nullptr;
        for (const Tile& t : tiles) {
            bool used = false;
            for (const CropRegion& r : out)
                used = used || (r.x == t.x && r.y == t.y);
            if (used) continue;
            if (!best || score(t) > score(*best)) best = &t;
        }
        if (best) out.push_back({best->x, best->y, cw, ch, tag});
    };
    take("edges", [](const Tile& t) { return t.edges; });
    take("texture", [](const Tile& t) { return t.texture; });
    const int cx = (w / 2 - cw / 2) / cw * cw;
    const int cy = (h / 2 - ch / 2) / ch * ch;
    take("centre", [&](const Tile& t) {
        const double dx = t.x - cx;
        const double dy = t.y - cy;
        return -(dx * dx + dy * dy);
    });
    return out;
}

} // namespace bench::metrics
