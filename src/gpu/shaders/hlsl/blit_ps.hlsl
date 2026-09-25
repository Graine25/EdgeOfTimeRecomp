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

float4 LanczosWeights(float f)
{
    f = clamp(f, 1e-4, 1.0 - 1e-4);
    float sf, cf, sh, ch;
    sincos(3.14159265 * f, sf, cf);
    sincos(1.57079633 * f, sh, ch);
    const float a0 = f + 1.0, a1 = f, a2 = 1.0 - f, a3 = 2.0 - f;
    return float4(-sf * ch / (a0 * a0), sf * sh / (a1 * a1), sf * ch / (a2 * a2),
                  -sf * sh / (a3 * a3));
}

float4 LanczosUpsample(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
{
    const float2 p = uv * size - 0.5;
    const float2 f = frac(p);
    const float2 base = floor(p);
    const float4 wx = LanczosWeights(f.x), wy = LanczosWeights(f.y);
    const float2 w0 = float2(wx.x, wy.x);
    const float2 w1 = float2(wx.y, wy.y);
    const float2 w2 = float2(wx.z, wy.z);
    const float2 w3 = float2(wx.w, wy.w);
    const float2 w12 = w1 + w2;
    const float2 t0 = (base - 0.5) / size;
    const float2 t12 = (base + 0.5 + w2 / w12) / size;
    const float2 t3 = (base + 2.5) / size;
    float4 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t0.y), 0) * (w0.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t0.y), 0) * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t0.y), 0) * (w3.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t12.y), 0) * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t12.y), 0) * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(t0.x, t3.y), 0) * (w0.x * w3.y);
    acc += tex.SampleLevel(linear_sampler, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
    acc += tex.SampleLevel(linear_sampler, float2(t3.x, t3.y), 0) * (w3.x * w3.y);
    const float2 sum = w0 + w12 + w3;
    return acc / (sum.x * sum.y);
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

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float FsrRcpGuarded(float x) { return rcp(max(x, 1e-6)); }

void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob,
                float clp, float3 c)
{
    float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
    const float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    const float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU,
                bool biV, float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    const float dc = lD - lC, cb = lC - lB;
    float lenX = FsrRcpGuarded(max(abs(dc), abs(cb)));
    const float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += lenX * lenX * w;

    const float ec = lE - lC, ca = lC - lA;
    float lenY = FsrRcpGuarded(max(abs(ec), abs(ca)));
    const float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += lenY * lenY * w;
}

float3 FsrEasu(Texture2D<float4> tex, SamplerState smp, float2 ip, float2 src_size, float2 dst_size)
{
    const float2 texel = 1.0 / src_size;
    float2 pp = (ip + 0.5) * (src_size / dst_size) - 0.5;
    const float2 fp = floor(pp);
    pp -= fp;
    const float2 p0 = (fp + float2(1.0, -1.0)) * texel;
    const float2 p1 = p0 + float2(-1.0, 2.0) * texel;
    const float2 p2 = p0 + float2(1.0, 2.0) * texel;
    const float2 p3 = p0 + float2(0.0, 4.0) * texel;

    const float4 bczzR = tex.GatherRed(smp, p0), bczzG = tex.GatherGreen(smp, p0),
                 bczzB = tex.GatherBlue(smp, p0);
    const float4 ijfeR = tex.GatherRed(smp, p1), ijfeG = tex.GatherGreen(smp, p1),
                 ijfeB = tex.GatherBlue(smp, p1);
    const float4 klhgR = tex.GatherRed(smp, p2), klhgG = tex.GatherGreen(smp, p2),
                 klhgB = tex.GatherBlue(smp, p2);
    const float4 zzonR = tex.GatherRed(smp, p3), zzonG = tex.GatherGreen(smp, p3),
                 zzonB = tex.GatherBlue(smp, p3);

    const float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    const float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    const float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    const float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    const float bL = bczzL.x, cL = bczzL.y;
    const float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    const float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    const float oL = zzonL.z, nL = zzonL.w;

    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : rsqrt(max(dirR, 1e-12));
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    const float stretch = (dir.x * dir.x + dir.y * dir.y) * FsrRcpGuarded(max(abs(dir.x), abs(dir.y)));
    const float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    const float clp = FsrRcpGuarded(lob);

    const float3 f4 = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    const float3 g4 = float3(klhgR.w, klhgG.w, klhgB.w);
    const float3 j4 = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    const float3 k4 = float3(klhgR.x, klhgG.x, klhgB.x);
    const float3 min4 = min(min(f4, g4), min(j4, k4));
    const float3 max4 = max(max(f4, g4), max(j4, k4));

    float3 aC = 0.0;
    float aW = 0.0;
    FsrEasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
    FsrEasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
    FsrEasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
    FsrEasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j4);
    FsrEasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f4);
    FsrEasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
    FsrEasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k4);
    FsrEasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
    FsrEasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
    FsrEasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g4);
    FsrEasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
    FsrEasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));
    return min(max4, max(min4, aC * FsrRcpGuarded(aW)));
}

float3 FsrRcas(Texture2D<float4> tex, int2 sp, int2 last, float sharpness)
{
    const float3 b = tex.Load(int3(clamp(sp + int2(0, -1), int2(0, 0), last), 0)).rgb;
    const float3 d = tex.Load(int3(clamp(sp + int2(-1, 0), int2(0, 0), last), 0)).rgb;
    const float3 e = tex.Load(int3(clamp(sp, int2(0, 0), last), 0)).rgb;
    const float3 f = tex.Load(int3(clamp(sp + int2(1, 0), int2(0, 0), last), 0)).rgb;
    const float3 h = tex.Load(int3(clamp(sp + int2(0, 1), int2(0, 0), last), 0)).rgb;

    const float3 mn4 = min(min(b, d), min(f, h));
    const float3 mx4 = max(max(b, d), max(f, h));
    const float3 hit_min = mn4 / max(4.0 * mx4, 1e-6);
    const float3 hit_max = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-6);
    const float3 lobe3 = max(-hit_min, hit_max);
    const float lobe = max(-FSR_RCAS_LIMIT, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * sharpness;
    const float rcp_l = FsrRcpGuarded(4.0 * lobe + 1.0);
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l;
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
        s = LanczosUpsample(tex, g_SamplerDescriptorHeap[0u], uv, float2(w, h));
    }
    else if (mode == 3u)
    {
        s = CatmullRomUpsample(tex, g_SamplerDescriptorHeap[0u], uv, float2(w, h));
    }
    else if (mode == 4u)
    {
        s = float4(FsrEasu(tex, g_SamplerDescriptorHeap[0u], position.xy - 0.5, float2(w, h),
                           g_PushConstants.Extra.zw),
                   1.0);
    }
    else if (mode == 5u)
    {
        const int2 sp = int2(floor(uv * float2(w, h)));
        s = float4(FsrRcas(tex, sp, int2(w - 1u, h - 1u), g_PushConstants.Extra.y), 1.0);
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
