// mw-scaler-bench — the classic resampling filters, three ways.
//
//   PsBilinear  one hardware fetch. This is what the host does today
//               (ColorConvert.cpp, D3D11_FILTER_MIN_MAG_MIP_LINEAR, no mips).
//               With a mip chain bound and a trilinear sampler it becomes the
//               "bilinear + mipmaps" candidate without changing a line.
//
//   PsCubic9    a 4x4 cubic (Catmull-Rom or Mitchell, per MW_B / MW_C) done
//               in 9 bilinear fetches (Jimenez): the two positive middle
//               weights of each axis merge into one fetch placed between the
//               texels. Fixed radius — the kernel spans 4 source pixels no
//               matter the ratio, so it is a reconstruction filter, not a
//               low-pass. Cheap, and exactly what most engines ship.
//
//   PsDirect    the honest version: the kernel (cubic or Lanczos, per
//               MW_KERNEL) dilated by the reduction ratio (MW_DILATE), every
//               tap loaded and weighted. MW_TAPS taps per axis, unrolled.
//               This is the only candidate here that is a true anti-aliasing
//               low-pass at a non-integer ratio.
//
// Defines set by the bench per (variant, case):
//   MW_FP16     0/1 — arithmetic in min16float
//   MW_B, MW_C  cubic parameters (0, 0.5 = Catmull-Rom; 1/3, 1/3 = Mitchell)
//   MW_KERNEL   0 = cubic, 1 = Lanczos
//   MW_RADIUS   kernel radius in (dilated) pixels: 2 for cubic/Lanczos-2, 3 for Lanczos-3
//   MW_DILATE   max(1, source / output) — how far the kernel is stretched
//   MW_TAPS     ceil(2 * MW_RADIUS * MW_DILATE) + 1

#include "common.hlsli"

#ifndef MW_B
#define MW_B 0.0
#endif
#ifndef MW_C
#define MW_C 0.5
#endif
#ifndef MW_KERNEL
#define MW_KERNEL 0
#endif
#ifndef MW_RADIUS
#define MW_RADIUS 2
#endif
#ifndef MW_DILATE
#define MW_DILATE 1.0
#endif
#ifndef MW_TAPS
#define MW_TAPS 5
#endif

float Kernel(float x)
{
#if MW_KERNEL == 1
    return LanczosWeight(x, float(MW_RADIUS));
#else
    return CubicWeight(x, MW_B, MW_C);
#endif
}

// ── One fetch ───────────────────────────────────────────────────────────────

float4 PsBilinear(VsOut i) : SV_TARGET
{
    return float4(F3(Source.Sample(Linear, i.uv).rgb), 1.0);
}

// ── Cubic in 9 bilinear fetches ─────────────────────────────────────────────

float4 PsCubic9(VsOut i) : SV_TARGET
{
    float2 samplePos = i.uv * SrcSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    // Weights for the texels at -1, 0, +1, +2 around texPos1.
    float2 w0 = float2(Kernel(1.0 + f.x), Kernel(1.0 + f.y));
    float2 w1 = float2(Kernel(f.x), Kernel(f.y));
    float2 w2 = float2(Kernel(1.0 - f.x), Kernel(1.0 - f.y));
    float2 w3 = float2(Kernel(2.0 - f.x), Kernel(2.0 - f.y));

    // The two centre weights are positive for every cubic in the family, so
    // one bilinear fetch placed at w2/(w1+w2) reproduces both.
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5);

    float2 texPos0 = (texPos1 - 1.0) * InvSrcSize;
    float2 texPos3 = (texPos1 + 2.0) * InvSrcSize;
    float2 texPos12 = (texPos1 + offset12) * InvSrcSize;

    F3 result = F3(0.0, 0.0, 0.0);
    result += F3(Source.SampleLevel(Linear, float2(texPos0.x, texPos0.y), 0).rgb) * F1(w0.x * w0.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos12.x, texPos0.y), 0).rgb) * F1(w12.x * w0.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos3.x, texPos0.y), 0).rgb) * F1(w3.x * w0.y);

    result += F3(Source.SampleLevel(Linear, float2(texPos0.x, texPos12.y), 0).rgb) * F1(w0.x * w12.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos12.x, texPos12.y), 0).rgb) * F1(w12.x * w12.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos3.x, texPos12.y), 0).rgb) * F1(w3.x * w12.y);

    result += F3(Source.SampleLevel(Linear, float2(texPos0.x, texPos3.y), 0).rgb) * F1(w0.x * w3.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos12.x, texPos3.y), 0).rgb) * F1(w12.x * w3.y);
    result += F3(Source.SampleLevel(Linear, float2(texPos3.x, texPos3.y), 0).rgb) * F1(w3.x * w3.y);

    // The weights of each axis sum to 1 for this family, so no normalisation.
    return float4(max(result, F3(0.0, 0.0, 0.0)), 1.0);
}

// ── Direct taps, kernel dilated to the ratio ────────────────────────────────

float4 PsDirect(VsOut i) : SV_TARGET
{
    // Continuous source coordinate; texel centres sit on integers.
    float2 center = i.uv * SrcSize - 0.5;
    const float dilate = MW_DILATE;
    const float reach = float(MW_RADIUS) * dilate;
    int2 first = int2(ceil(center - reach));
    int2 last = int2(SrcSize) - 1;

    float wx[MW_TAPS];
    float wy[MW_TAPS];
    float sumX = 0.0;
    float sumY = 0.0;
    [unroll]
    for (int t = 0; t < MW_TAPS; ++t) {
        float dx = (float(first.x + t) - center.x) / dilate;
        float dy = (float(first.y + t) - center.y) / dilate;
        wx[t] = Kernel(dx);
        wy[t] = Kernel(dy);
        sumX += wx[t];
        sumY += wy[t];
    }

    F3 acc = F3(0.0, 0.0, 0.0);
    [unroll]
    for (int y = 0; y < MW_TAPS; ++y) {
        int sy = clamp(first.y + y, 0, last.y);
        F3 row = F3(0.0, 0.0, 0.0);
        [unroll]
        for (int x = 0; x < MW_TAPS; ++x) {
            int sx = clamp(first.x + x, 0, last.x);
            row += F3(Source.Load(int3(sx, sy, 0)).rgb) * F1(wx[x]);
        }
        acc += row * F1(wy[y]);
    }
    acc /= F1(sumX * sumY);
    return float4(max(acc, F3(0.0, 0.0, 0.0)), 1.0);
}
