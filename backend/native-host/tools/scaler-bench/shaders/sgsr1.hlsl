// mw-scaler-bench — Qualcomm Snapdragon Game Super Resolution 1.
//
// Wrapper only: the algorithm is third_party/sgsr/sgsr1_mobile.h, untouched;
// the vendor's own sgsr1_shader_mobile.hlsl is kept next to it for the audit,
// this file merely swaps its vertex stage for the bench's SV_VertexID triangle
// and its `half` for the precision under test.
//
// SGSR1 is a 12-tap Lanczos-2-like UPSCALER with edge-adaptive sharpening and
// no low-pass: at a reduction ratio it can only alias. Run as asked, reported
// as measured. Perceptual input only, per the vendor.
//
// Defines: MW_FP16 — `half` becomes min16float (fxc's `half` is float).

#include "common.hlsli"

#if MW_FP16
#define half min16float
#define half2 min16float2
#define half3 min16float3
#define half4 min16float4
#endif

#define SGSR_MOBILE
#define SGSR_H 1

// ViewportInfo = (1/srcW, 1/srcH, srcW, srcH), the vendor's convention.
#define ViewportInfo float4(InvSrcSize, SrcSize)

half4 SGSRRH(float2 p) { return half4(Source.GatherRed(Linear, p)); }
half4 SGSRGH(float2 p) { return half4(Source.GatherGreen(Linear, p)); }
half4 SGSRBH(float2 p) { return half4(Source.GatherBlue(Linear, p)); }
half4 SGSRAH(float2 p) { return half4(Source.GatherAlpha(Linear, p)); }
half4 SGSRRGBH(float2 p) { return half4(Source.SampleLevel(Linear, p, 0)); }

half4 SGSRH(float2 p, uint channel)
{
    if (channel == 0) return SGSRRH(p);
    if (channel == 1) return SGSRGH(p);
    if (channel == 2) return SGSRBH(p);
    return SGSRAH(p);
}

#include "third_party/sgsr/sgsr1_mobile.h"

float4 PsMain(VsOut i) : SV_TARGET
{
    half4 c = half4(0, 0, 0, 1);
    SgsrYuvH(c, i.uv, ViewportInfo);
    return float4(c.rgb, 1.0);
}
