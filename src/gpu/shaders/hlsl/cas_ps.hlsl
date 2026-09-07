#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float3 Load(Texture2D<float4> tex, int2 c, int2 last)
{
    return tex.Load(int3(clamp(c, int2(0, 0), last), 0)).rgb;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h;
    tex.GetDimensions(w, h);
    const int2 last = int2(w, h) - 1;
    const int2 p = int2(position.xy);
    const float3 a = Load(tex, p + int2(-1, -1), last);
    const float3 b = Load(tex, p + int2( 0, -1), last);
    const float3 c = Load(tex, p + int2( 1, -1), last);
    const float3 d = Load(tex, p + int2(-1,  0), last);
    const float3 e = Load(tex, p,                last);
    const float3 f = Load(tex, p + int2( 1,  0), last);
    const float3 g = Load(tex, p + int2(-1,  1), last);
    const float3 hh = Load(tex, p + int2( 0,  1), last);
    const float3 i = Load(tex, p + int2( 1,  1), last);

    float3 mn = min(min(min(d, e), min(f, b)), hh);
    const float3 mn2 = min(min(min(mn, a), min(c, g)), i);
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), hh);
    const float3 mx2 = max(max(max(mx, a), max(c, g)), i);
    mx += mx2;
    const float3 rcpM = 1.0 / max(mx, 1e-5);
    float3 amp = saturate(min(mn, 2.0 - mx) * rcpM);
    amp = sqrt(amp);
    const float peak = -1.0 / lerp(8.0, 5.0, saturate(g_PushConstants.Extra.x));
    const float3 wgt = amp * peak;
    const float3 rcpWeight = 1.0 / (1.0 + 4.0 * wgt);
    const float3 outColor = saturate((b * wgt + d * wgt + f * wgt + hh * wgt + e) * rcpWeight);
    return float4(outColor, 1.0);
}
