#pragma once
#include "copy_common.hlsli"

#define FXAA_PC 1
#define FXAA_HLSL_5 1
#define FXAA_GREEN_AS_LUMA 1
#include "FXAA3_11.h"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    FxaaTex t;
    t.smpl = g_SamplerDescriptorHeap[0];
    t.tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h;
    t.tex.GetDimensions(w, h);
    const float2 rcpFrame = 1.0 / float2(w, h);
    const float4 unused = float4(0.0, 0.0, 0.0, 0.0);
    float4 c = FxaaPixelShader(texCoord, unused, t, t, t, rcpFrame, unused, unused, unused,
                               g_PushConstants.Extra.x, g_PushConstants.Extra.y,
                               g_PushConstants.Extra.z, 8.0, 0.125, 0.05,
                               float4(1.0, -1.0, 0.25, -0.25));
    return float4(c.rgb, 1.0);
}
