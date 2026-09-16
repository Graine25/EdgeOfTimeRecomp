Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState      g_SamplerDescriptorHeap[]   : register(s0, space3);

struct TaaConstants
{
    row_major float4x4 Reproject;
    float4 Jitter;
    float4 Params;
    uint4  Indices;
};

#ifdef __spirv__
struct PushConstants
{
    uint64_t Constants;
    uint64_t Unused1;
    uint64_t Unused2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define TAA_LOAD(type, off) vk::RawBufferLoad<type>(g_PushConstants.Constants + (off))
#define TAA_REPROJECT_ROW(r) TAA_LOAD(float4, (r) * 16)
#define TAA_JITTER  TAA_LOAD(float4, 64)
#define TAA_PARAMS  TAA_LOAD(float4, 80)
#define TAA_INDICES TAA_LOAD(uint4, 96)
#else
ConstantBuffer<TaaConstants> g_Taa : register(b0, space4);
#define TAA_REPROJECT_ROW(r) g_Taa.Reproject[r]
#define TAA_JITTER  g_Taa.Jitter
#define TAA_PARAMS  g_Taa.Params
#define TAA_INDICES g_Taa.Indices
#endif

static const uint kSamplerLinearClamp = 0u;

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float3 SampleHistory(Texture2D<float4> tex, SamplerState linear_sampler, float2 uv, float2 size)
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
    float3 acc = 0.0;
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos0.y), 0).rgb * (w12.x * w0.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos0.x, tex_pos12.y), 0).rgb * (w0.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos12.y), 0).rgb * (w12.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos3.x, tex_pos12.y), 0).rgb * (w3.x * w12.y);
    acc += tex.SampleLevel(linear_sampler, float2(tex_pos12.x, tex_pos3.y), 0).rgb * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(acc / weight, 0.0);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    const uint4 indices = TAA_INDICES;
    const float4 params = TAA_PARAMS;
    const float4 jitter = TAA_JITTER;
    Texture2D<float4> scene = g_Texture2DDescriptorHeap[indices.x];
    Texture2D<float4> history = g_Texture2DDescriptorHeap[indices.y];
    Texture2D<float4> depth = g_Texture2DDescriptorHeap[indices.z];

    const int2 size = int2(params.zw);
    const int2 px = int2(position.xy);
    const float4 current = scene.Load(int3(px, 0));

    if (params.y < 0.5 || (indices.w & 1u) != 0u)
        return current;

    float3 lo = current.rgb, hi = current.rgb;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            const int2 c = clamp(px + int2(x, y), int2(0, 0), size - 1);
            const float3 n = scene.Load(int3(c, 0)).rgb;
            lo = min(lo, n);
            hi = max(hi, n);
        }
    }

    const float d = depth.Load(int3(px, 0)).x;
    const float2 uvUnjittered = texCoord - jitter.xy;
    const float4 clip = float4(uvUnjittered.x * 2.0 - 1.0, 1.0 - uvUnjittered.y * 2.0, 1.0 - d, 1.0);
    float4 prev;
    prev.x = dot(clip, float4(TAA_REPROJECT_ROW(0).x, TAA_REPROJECT_ROW(1).x, TAA_REPROJECT_ROW(2).x, TAA_REPROJECT_ROW(3).x));
    prev.y = dot(clip, float4(TAA_REPROJECT_ROW(0).y, TAA_REPROJECT_ROW(1).y, TAA_REPROJECT_ROW(2).y, TAA_REPROJECT_ROW(3).y));
    prev.z = dot(clip, float4(TAA_REPROJECT_ROW(0).z, TAA_REPROJECT_ROW(1).z, TAA_REPROJECT_ROW(2).z, TAA_REPROJECT_ROW(3).z));
    prev.w = dot(clip, float4(TAA_REPROJECT_ROW(0).w, TAA_REPROJECT_ROW(1).w, TAA_REPROJECT_ROW(2).w, TAA_REPROJECT_ROW(3).w));
    if (prev.w <= 1e-6)
        return current;
    const float2 prevNdc = prev.xy / prev.w;
    const float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
        return current;

    float3 hist = SampleHistory(history, g_SamplerDescriptorHeap[kSamplerLinearClamp], prevUv, float2(size));
    hist = clamp(hist, lo, hi);

    const float feedback = params.x;
    const float wc = (1.0 - feedback) / (1.0 + Luma(current.rgb));
    const float wh = feedback / (1.0 + Luma(hist));
    const float3 blended = (current.rgb * wc + hist * wh) / max(wc + wh, 1e-5);
    return float4(blended, current.a);
}
