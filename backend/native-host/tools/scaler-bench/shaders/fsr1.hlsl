// mw-scaler-bench — AMD FidelityFX Super Resolution 1.0 (EASU, then RCAS).
//
// Wrapper only: the algorithm is third_party/ffx/ffx_fsr1.h, untouched. The
// dispatch pattern (64 threads, ARmp8x8 swizzle, four pixels a thread) is the
// one from AMD's own sample.
//
// EASU is documented for "1x to 4x area" — an UPSCALER. The bench runs it at
// the host's reduction ratios as asked, and reports what comes out; nothing
// is patched to make it behave.
//
// Input is expected perceptual (sRGB-encoded, or PQ for HDR) per the header's
// own requirements; the bench never feeds it linear light.
//
// Defines: MW_FP16 (packed half maths, FSR_EASU_H / FSR_RCAS_H), MW_RCAS
// (compile the RCAS pass instead of EASU).

#define A_GPU 1
#define A_HLSL 1
#if MW_FP16
#define A_HALF 1
#endif

#include "third_party/ffx/ffx_a.h"

cbuffer Consts : register(b0)
{
    AU4 Const0;
    AU4 Const1;
    AU4 Const2;
    AU4 Const3;
};

Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);
RWTexture2D<float4> Output : register(u0);

#if MW_RCAS
#if MW_FP16
#define FSR_RCAS_H 1
AH4 FsrRcasLoadH(ASW2 p) { return AH4(Source.Load(ASU3(ASU2(p), 0))); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return Source.Load(ASU3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#else
#if MW_FP16
#define FSR_EASU_H 1
AH4 FsrEasuRH(AF2 p) { return AH4(Source.GatherRed(Linear, p, ASU2(0, 0))); }
AH4 FsrEasuGH(AF2 p) { return AH4(Source.GatherGreen(Linear, p, ASU2(0, 0))); }
AH4 FsrEasuBH(AF2 p) { return AH4(Source.GatherBlue(Linear, p, ASU2(0, 0))); }
#else
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return Source.GatherRed(Linear, p, ASU2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return Source.GatherGreen(Linear, p, ASU2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return Source.GatherBlue(Linear, p, ASU2(0, 0)); }
#endif
#endif

#include "third_party/ffx/ffx_fsr1.h"

void Filter(AU2 pos)
{
#if MW_RCAS
#if MW_FP16
    AH3 c;
    FsrRcasH(c.r, c.g, c.b, pos, Const0);
    Output[pos] = AF4(AH4(c, AH1(1.0)));
#else
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, Const0);
    Output[pos] = AF4(c, 1.0);
#endif
#else
#if MW_FP16
    AH3 c;
    FsrEasuH(c, pos, Const0, Const1, Const2, Const3);
    Output[pos] = AF4(AH4(c, AH1(1.0)));
#else
    AF3 c;
    FsrEasuF(c, pos, Const0, Const1, Const2, Const3);
    Output[pos] = AF4(c, 1.0);
#endif
#endif
}

[numthreads(64, 1, 1)]
void CsMain(uint3 localId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    AU2 gxy = ARmp8x8(localId.x) + AU2(groupId.x << 4u, groupId.y << 4u);
    Filter(gxy);
    gxy.x += 8u;
    Filter(gxy);
    gxy.y += 8u;
    Filter(gxy);
    gxy.x -= 8u;
    Filter(gxy);
}
