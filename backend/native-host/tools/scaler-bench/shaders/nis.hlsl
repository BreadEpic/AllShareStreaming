// mw-scaler-bench — NVIDIA Image Scaling 1.0.3 (NVScaler).
//
// Wrapper only: the algorithm is third_party/nis/NIS_Scaler.h, untouched, and
// the bindings are the ones of NVIDIA's own NIS_Main.hlsl.
//
// NIS is an UPSCALER: NVScalerUpdateConfig() in NIS_Config.h refuses any scale
// outside 1x..2x, and the shader's groupshared tile is sized for that range —
// at a reduction ratio the tile would overflow. So the bench asks the SDK's
// config function first and, when it says no, records the variant as
// unsupported rather than run a shader outside its contract.
//
// Defines: MW_FP16 → NIS_USE_HALF_PRECISION, MW_NIS_HDR → NIS_HDR_MODE (2 = PQ).

#define NIS_HLSL 1
#define NIS_SCALER 1
#if MW_FP16
#define NIS_USE_HALF_PRECISION 1
#endif
#ifdef MW_NIS_HDR
#define NIS_HDR_MODE MW_NIS_HDR
#endif
#ifndef NIS_BLOCK_WIDTH
#define NIS_BLOCK_WIDTH 32
#endif
#ifndef NIS_BLOCK_HEIGHT
#define NIS_BLOCK_HEIGHT 24
#endif
#ifndef NIS_THREAD_GROUP_SIZE
#define NIS_THREAD_GROUP_SIZE 256
#endif

cbuffer cb : register(b0)
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;

    float kContrastBoost;
    float kEps;
    float kSharpStartY;
    float kSharpScaleY;

    float kSharpStrengthMin;
    float kSharpStrengthScale;
    float kSharpLimitMin;
    float kSharpLimitScale;

    float kScaleX;
    float kScaleY;

    float kDstNormX;
    float kDstNormY;
    float kSrcNormX;
    float kSrcNormY;

    uint kInputViewportOriginX;
    uint kInputViewportOriginY;
    uint kInputViewportWidth;
    uint kInputViewportHeight;

    uint kOutputViewportOriginX;
    uint kOutputViewportOriginY;
    uint kOutputViewportWidth;
    uint kOutputViewportHeight;

    float reserved0;
    float reserved1;
};

SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);
Texture2D coef_scaler : register(t1);
Texture2D coef_usm : register(t2);

#include "third_party/nis/NIS_Scaler.h"

[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void CsMain(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    NVScaler(blockIdx.xy, threadIdx.x);
}
