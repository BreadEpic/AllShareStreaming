// mw-scaler-bench — what every candidate shader shares.
//
// The bench mirrors the host's conversion pass (ColorConvert.cpp): a
// full-screen triangle from SV_VertexID, no vertex buffer, the source bound
// as one SRV, the output as one render target whose viewport IS the scale.
// Every pixel-shader candidate is written as a `Scene(uv)`-style function
// returning the resampled colour for one output pixel, so what is measured is
// the filter and nothing else.
//
// Precision: MW_FP16 = 1 swaps the arithmetic type for min16float, which is
// what D3D11 offers for half precision (the driver may honour it or not; the
// bench reports D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT per GPU).

#ifndef MW_FP16
#define MW_FP16 0
#endif

#if MW_FP16
#define F1 min16float
#define F2 min16float2
#define F3 min16float3
#define F4 min16float4
#else
#define F1 float
#define F2 float2
#define F3 float3
#define F4 float4
#endif

Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);
SamplerState Point : register(s1);

cbuffer Params : register(b0)
{
    float2 SrcSize;     // source texture size, pixels
    float2 InvSrcSize;  // 1 / SrcSize
    float2 DstSize;     // output size, pixels
    float2 InvDstSize;  // 1 / DstSize
    float2 Scale;       // SrcSize / DstSize — > 1 when downscaling
    float2 InvScale;    // DstSize / SrcSize
};

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID)
{
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// ── Kernels ─────────────────────────────────────────────────────────────────
// All take the distance in SOURCE pixels already divided by the dilation, so
// a kernel of radius R spans R * dilation source pixels.

// Mitchell–Netravali family: B=C=1/3 is Mitchell, B=0 C=1/2 is Catmull-Rom.
float CubicWeight(float x, float B, float C)
{
    x = abs(x);
    float x2 = x * x;
    float x3 = x2 * x;
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x3 + (-18.0 + 12.0 * B + 6.0 * C) * x2 +
                (6.0 - 2.0 * B)) / 6.0;
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x3 + (6.0 * B + 30.0 * C) * x2 + (-12.0 * B - 48.0 * C) * x +
                (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

// Lanczos with `a` lobes.
float LanczosWeight(float x, float a)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= a) return 0.0;
    float px = 3.14159265 * x;
    return a * sin(px) * sin(px / a) / (px * px);
}
