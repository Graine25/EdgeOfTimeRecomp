#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

float SelectChannel(float4 v, uint sel)
{
    return sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    uint w, h;
    tex.GetDimensions(w, h);
    float2 texels_per_pixel = float2(ddx(uv.x) * w, ddy(uv.y) * h);
    bool one_to_one = all(abs(texels_per_pixel - 1.0) < 0.01);
    float4 s = tex.Sample(g_SamplerDescriptorHeap[one_to_one ? 1u : 0u], uv);
    if (g_PushConstants.Param1 < 0.5 && (g_PushConstants.ResourceDescriptorIndex2 & 0x80000000u))
    {
        uint p = g_PushConstants.ResourceDescriptorIndex2;
        float4 src = s;
        s = float4(SelectChannel(src, p & 7u), SelectChannel(src, (p >> 3) & 7u),
                   SelectChannel(src, (p >> 6) & 7u), SelectChannel(src, (p >> 9) & 7u));
    }
    float3 rgb = s.rgb * g_PushConstants.Param0;
    if (g_PushConstants.ColorAdjust.w > 0.0)
    {
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(float3(luma, luma, luma), rgb, g_PushConstants.ColorAdjust.z);
        rgb = (rgb - 0.5) * g_PushConstants.ColorAdjust.y + 0.5 + g_PushConstants.ColorAdjust.x;
        rgb = pow(saturate(rgb), 1.0 / g_PushConstants.ColorAdjust.w);
    }
    if (g_PushConstants.Param1 > 1.5)
    {
        Texture2D<float4> lut = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex2];
        float3 u = saturate(rgb) * (1023.0 / 1024.0) + (0.5 / 1024.0);
        rgb = float3(lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.r, 0.5), 0).r,
                     lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.g, 0.5), 0).g,
                     lut.SampleLevel(g_SamplerDescriptorHeap[0], float2(u.b, 0.5), 0).b);
    }
    float a = g_PushConstants.Param1 > 0.5 ? 1.0 : s.a;
    return float4(rgb, a);
}
