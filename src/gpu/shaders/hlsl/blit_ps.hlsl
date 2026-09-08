#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

float SelectChannel(float4 v, uint sel)
{
    return sel < 4u ? v[sel] : (sel == 5u ? 1.0 : 0.0);
}

float Lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-4)
        return 1.0;
    if (x >= 2.0)
        return 0.0;
    const float px = 3.14159265 * x;
    return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
}

float4 BoxDownsample(Texture2D<float4> tex, float2 uv, uint2 size, uint ratio)
{
    const uint taps = min(ratio, 8u);
    const float2 p = uv * float2(size);
    const int2 base = int2(floor(p - float(ratio) * 0.5 + 0.5));
    const int2 last = int2(size) - 1;
    float4 acc = 0.0;
    [loop] for (uint y = 0u; y < taps; ++y)
    {
        const int oy = (int)((y * ratio + ratio / 2u) / taps);
        [loop] for (uint x = 0u; x < taps; ++x)
        {
            const int ox = (int)((x * ratio + ratio / 2u) / taps);
            acc += tex.Load(int3(clamp(base + int2(ox, oy), int2(0, 0), last), 0));
        }
    }
    return acc / (float)(taps * taps);
}

float4 LanczosUpsample(Texture2D<float4> tex, float2 uv, uint2 size)
{
    const float2 p = uv * float2(size) - 0.5;
    const float2 f = frac(p);
    const int2 base = int2(floor(p));
    const int2 last = int2(size) - 1;
    float wx[4], wy[4];
    float sx = 0.0, sy = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        wx[i] = Lanczos2(f.x - float(i - 1));
        wy[i] = Lanczos2(f.y - float(i - 1));
        sx += wx[i];
        sy += wy[i];
    }
    float4 acc = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        [unroll] for (int k = 0; k < 4; ++k)
        {
            const int2 c = clamp(base + int2(k - 1, j - 1), int2(0, 0), last);
            acc += tex.Load(int3(c, 0)) * (wx[k] * wy[j]);
        }
    }
    return acc / (sx * sy);
}

float4 CatmullRomUpsample(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
{
    const float2 sample_pos = uv * size;
    const float2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
    const float2 f = sample_pos - tex_pos1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / w12;
    const float2 tex_pos0 = (tex_pos1 - 1.0) / size;
    const float2 tex_pos3 = (tex_pos1 + 2.0) / size;
    const float2 tex_pos12 = (tex_pos1 + offset12) / size;
    float4 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos0.y), 0) * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos0.x, tex_pos12.y), 0) * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos12.y), 0) * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos3.x, tex_pos12.y), 0) * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return acc / weight;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float2 uv = lerp(g_PushConstants.SourceRect.xy, g_PushConstants.SourceRect.zw, texCoord);
    uint w, h;
    tex.GetDimensions(w, h);
    float4 s;
    const uint mode = (uint)(g_PushConstants.Extra.x + 0.5);
    if (mode == 1u)
    {
        s = BoxDownsample(tex, uv, uint2(w, h), max((uint)(g_PushConstants.Extra.y + 0.5), 2u));
    }
    else if (mode == 2u)
    {
        s = LanczosUpsample(tex, uv, uint2(w, h));
    }
    else if (mode == 3u)
    {
        s = CatmullRomUpsample(tex, g_SamplerDescriptorHeap[0u], uv, float2(w, h));
    }
    else
    {
        float2 texels_per_pixel = float2(ddx(uv.x) * w, ddy(uv.y) * h);
        bool one_to_one = all(abs(texels_per_pixel - 1.0) < 0.01);
        s = tex.Sample(g_SamplerDescriptorHeap[one_to_one ? 1u : 0u], uv);
    }
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
