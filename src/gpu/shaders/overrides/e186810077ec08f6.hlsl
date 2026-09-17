#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#ifdef REEOT_RECOMP
    #define SPEC_CONSTANT_SINT_TEXCOORD     (1 << 2)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#ifdef REEOT_RECOMP
#define g_Booleans(i)              vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320 + (i)*4)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 352)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 356)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 364)
#define g_SwappedNormals           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 368)
#define g_SwappedBinormals         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 372)
#define g_SwappedTangents          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 376)
#define g_SwappedBlendWeights      vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 380)
#define g_SwappedPositions         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 384)
#define g_SintTexcoords            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 388)
#define g_BiasedTextures           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 392)
#define g_PackedDec3               vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 396)
#define g_LoopConstants(i)         vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 400 + (i)*16)
#define g_PosScale                 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 912)
#define g_PosOffset                vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 928)
#else
#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)
#endif

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#ifdef REEOT_RECOMP
#define DEFINE_SHARED_CONSTANTS() \
    uint4 g_BooleansArr[2] : packoffset(c20); \
    uint g_SwappedTexcoords : packoffset(c22.x); \
    float2 g_HalfPixelOffset : packoffset(c22.y); \
    float g_AlphaThreshold : packoffset(c22.w); \
    uint g_SwappedNormals : packoffset(c23.x); \
    uint g_SwappedBinormals : packoffset(c23.y); \
    uint g_SwappedTangents : packoffset(c23.z); \
    uint g_SwappedBlendWeights : packoffset(c23.w); \
    uint g_SwappedPositions : packoffset(c24.x); \
    uint g_SintTexcoords : packoffset(c24.y); \
    uint g_BiasedTextures : packoffset(c24.z); \
    uint g_PackedDec3 : packoffset(c24.w); \
    uint4 g_LoopConstantsArr[32] : packoffset(c25); \
    float4 g_PosScale : packoffset(c57); \
    float4 g_PosOffset : packoffset(c58);

#define g_Booleans(i) (g_BooleansArr[(i) / 4][(i) % 4])
#define g_LoopConstants(i) g_LoopConstantsArr[i]
#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);
#endif

uint g_SpecConstants();

#endif

#ifdef REEOT_RECOMP
#define BOOL_BIT(n) ((g_Booleans((n) / 32u) & (1u << ((n) & 31u))) != 0)
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
Texture1D<float4> g_Texture1DDescriptorHeap[] : register(t0, space4);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

#ifdef REEOT_RECOMP
float pwlGammaToLinear1(float gamma)
{
    gamma = saturate(gamma);
    float scale, offset;
    if (gamma >= 96.0 / 255.0)
    {
        if (gamma >= 192.0 / 255.0) { scale = 8.0 / 1024.0; offset = -1024.0; }
        else                        { scale = 4.0 / 1024.0; offset = -256.0; }
    }
    else
    {
        if (gamma >= 64.0 / 255.0)  { scale = 2.0 / 1024.0; offset = -64.0; }
        else                        { scale = 1.0 / 1024.0; offset = 0.0; }
    }
    float lin = gamma * ((255.0 * 1024.0) * scale) + offset;
    lin += trunc(lin * scale);
    return lin * (1.0 / 1023.0);
}

float linearToPWLGamma1(float lin)
{
    lin = saturate(lin);
    float scale, offset;
    if (lin >= 128.0 / 1023.0)
    {
        if (lin >= 512.0 / 1023.0) { scale = 1023.0 / 8.0; offset = 128.0 / 255.0; }
        else                       { scale = 1023.0 / 4.0; offset = 64.0 / 255.0; }
    }
    else
    {
        if (lin >= 64.0 / 1023.0)  { scale = 1023.0 / 2.0; offset = 32.0 / 255.0; }
        else                       { scale = 1023.0;       offset = 0.0; }
    }
    return trunc(lin * scale) * (1.0 / 255.0) + offset;
}

float3 linearToPWLGamma(float3 lin)
{
    return float3(linearToPWLGamma1(lin.r), linearToPWLGamma1(lin.g),
                  linearToPWLGamma1(lin.b));
}

float4 applyFetchSign(float4 value, uint biasedMask, uint alphaMask, uint slotIndex)
{
    if (biasedMask & (1u << slotIndex))
        value.rgb = value.rgb * 2.0 - 1.0;
    if (alphaMask & (1u << (16u + slotIndex)))
        value.a = value.a * 2.0 - 1.0;
    if (biasedMask & (1u << (16u + slotIndex)))
        value.rgb = float3(pwlGammaToLinear1(value.r), pwlGammaToLinear1(value.g),
                           pwlGammaToLinear1(value.b));
    return value;
}
#endif

float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    return g_Texture1DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

#ifdef REEOT_RECOMP
float4 tfetchR11G11B10(uint dec3Mask, float4 value, uint slotCode)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        uint v = asuint(value.x);
        if (dec3Mask & (1u << slotCode))
        {
            int3 s = int3(v << 22, v << 12, v << 2) >> 22;
            return float4(max(float3(s) / 511.0, -1.0), 1.0);
        }
        return float4(
            (v & 0x00000400 ? -1.0 : 0.0) + ((v & 0x3FF) / 1024.0),
            (v & 0x00200000 ? -1.0 : 0.0) + (((v >> 11) & 0x3FF) / 1024.0),
            (v & 0x80000000 ? -1.0 : 0.0) + (((v >> 22) & 0x1FF) / 512.0),
            1.0);
    }
    return value;
}

float4 swapFloats(uint swappedMask, float4 value, uint semanticIndex)
{
    return (swappedMask & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 sintTexcoord(uint mask, float4 value, uint semanticIndex)
{
    if ((mask & (1u << semanticIndex)) != 0)
    {
        int4 si = (int4(asuint(value)) << 16) >> 16;
        return float4(si);
    }
    return value;
}
#else
float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}
#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif

#ifdef __spirv__

#define HiDepth_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4)
#define HiDepth_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 68)
#define HiDepth_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 132)
#define HiDepth_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 196)
#define HiDepth_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define LoColor_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define LoColor_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 64)
#define LoColor_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 128)
#define LoColor_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192)
#define LoColor_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define LoDepth_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 8)
#define LoDepth_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 72)
#define LoDepth_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 136)
#define LoDepth_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 200)
#define LoDepth_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 264)
#define s3_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 12)
#define s3_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 76)
#define s3_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 140)
#define s3_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 204)
#define s3_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 268)
#define s4_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 16)
#define s4_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 80)
#define s4_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 144)
#define s4_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 208)
#define s4_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 272)
#define s5_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 20)
#define s5_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 84)
#define s5_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 148)
#define s5_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 212)
#define s5_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276)
#define s6_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 24)
#define s6_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 88)
#define s6_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 152)
#define s6_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 216)
#define s6_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 280)
#define s7_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 28)
#define s7_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 92)
#define s7_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 156)
#define s7_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 220)
#define s7_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 284)
#define s8_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 32)
#define s8_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 96)
#define s8_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 160)
#define s8_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 224)
#define s8_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 288)
#define s9_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 36)
#define s9_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 100)
#define s9_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 164)
#define s9_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 228)
#define s9_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 292)
#define s10_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 40)
#define s10_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 104)
#define s10_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 168)
#define s10_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 232)
#define s10_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296)
#define s11_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 44)
#define s11_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 108)
#define s11_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 172)
#define s11_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 236)
#define s11_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 300)
#define s12_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 48)
#define s12_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 112)
#define s12_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 176)
#define s12_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 240)
#define s12_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 304)
#define s13_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 52)
#define s13_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 116)
#define s13_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 180)
#define s13_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 244)
#define s13_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 308)
#define s14_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 56)
#define s14_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 120)
#define s14_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 184)
#define s14_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 248)
#define s14_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 312)
#define s15_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 60)
#define s15_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 124)
#define s15_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 188)
#define s15_Texture1DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 252)
#define s15_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 316)

#else

cbuffer PixelShaderConstants : register(b1, space4)
{
};

cbuffer SharedConstants : register(b2, space4)
{
	uint HiDepth_Texture2DDescriptorIndex : packoffset(c0.y);
	uint HiDepth_Texture3DDescriptorIndex : packoffset(c4.y);
	uint HiDepth_TextureCubeDescriptorIndex : packoffset(c8.y);
	uint HiDepth_Texture1DDescriptorIndex : packoffset(c12.y);
	uint HiDepth_SamplerDescriptorIndex : packoffset(c16.y);
	uint LoColor_Texture2DDescriptorIndex : packoffset(c0.x);
	uint LoColor_Texture3DDescriptorIndex : packoffset(c4.x);
	uint LoColor_TextureCubeDescriptorIndex : packoffset(c8.x);
	uint LoColor_Texture1DDescriptorIndex : packoffset(c12.x);
	uint LoColor_SamplerDescriptorIndex : packoffset(c16.x);
	uint LoDepth_Texture2DDescriptorIndex : packoffset(c0.z);
	uint LoDepth_Texture3DDescriptorIndex : packoffset(c4.z);
	uint LoDepth_TextureCubeDescriptorIndex : packoffset(c8.z);
	uint LoDepth_Texture1DDescriptorIndex : packoffset(c12.z);
	uint LoDepth_SamplerDescriptorIndex : packoffset(c16.z);
	uint s3_Texture2DDescriptorIndex : packoffset(c0.w);
	uint s3_Texture3DDescriptorIndex : packoffset(c4.w);
	uint s3_TextureCubeDescriptorIndex : packoffset(c8.w);
	uint s3_Texture1DDescriptorIndex : packoffset(c12.w);
	uint s3_SamplerDescriptorIndex : packoffset(c16.w);
	uint s4_Texture2DDescriptorIndex : packoffset(c1.x);
	uint s4_Texture3DDescriptorIndex : packoffset(c5.x);
	uint s4_TextureCubeDescriptorIndex : packoffset(c9.x);
	uint s4_Texture1DDescriptorIndex : packoffset(c13.x);
	uint s4_SamplerDescriptorIndex : packoffset(c17.x);
	uint s5_Texture2DDescriptorIndex : packoffset(c1.y);
	uint s5_Texture3DDescriptorIndex : packoffset(c5.y);
	uint s5_TextureCubeDescriptorIndex : packoffset(c9.y);
	uint s5_Texture1DDescriptorIndex : packoffset(c13.y);
	uint s5_SamplerDescriptorIndex : packoffset(c17.y);
	uint s6_Texture2DDescriptorIndex : packoffset(c1.z);
	uint s6_Texture3DDescriptorIndex : packoffset(c5.z);
	uint s6_TextureCubeDescriptorIndex : packoffset(c9.z);
	uint s6_Texture1DDescriptorIndex : packoffset(c13.z);
	uint s6_SamplerDescriptorIndex : packoffset(c17.z);
	uint s7_Texture2DDescriptorIndex : packoffset(c1.w);
	uint s7_Texture3DDescriptorIndex : packoffset(c5.w);
	uint s7_TextureCubeDescriptorIndex : packoffset(c9.w);
	uint s7_Texture1DDescriptorIndex : packoffset(c13.w);
	uint s7_SamplerDescriptorIndex : packoffset(c17.w);
	uint s8_Texture2DDescriptorIndex : packoffset(c2.x);
	uint s8_Texture3DDescriptorIndex : packoffset(c6.x);
	uint s8_TextureCubeDescriptorIndex : packoffset(c10.x);
	uint s8_Texture1DDescriptorIndex : packoffset(c14.x);
	uint s8_SamplerDescriptorIndex : packoffset(c18.x);
	uint s9_Texture2DDescriptorIndex : packoffset(c2.y);
	uint s9_Texture3DDescriptorIndex : packoffset(c6.y);
	uint s9_TextureCubeDescriptorIndex : packoffset(c10.y);
	uint s9_Texture1DDescriptorIndex : packoffset(c14.y);
	uint s9_SamplerDescriptorIndex : packoffset(c18.y);
	uint s10_Texture2DDescriptorIndex : packoffset(c2.z);
	uint s10_Texture3DDescriptorIndex : packoffset(c6.z);
	uint s10_TextureCubeDescriptorIndex : packoffset(c10.z);
	uint s10_Texture1DDescriptorIndex : packoffset(c14.z);
	uint s10_SamplerDescriptorIndex : packoffset(c18.z);
	uint s11_Texture2DDescriptorIndex : packoffset(c2.w);
	uint s11_Texture3DDescriptorIndex : packoffset(c6.w);
	uint s11_TextureCubeDescriptorIndex : packoffset(c10.w);
	uint s11_Texture1DDescriptorIndex : packoffset(c14.w);
	uint s11_SamplerDescriptorIndex : packoffset(c18.w);
	uint s12_Texture2DDescriptorIndex : packoffset(c3.x);
	uint s12_Texture3DDescriptorIndex : packoffset(c7.x);
	uint s12_TextureCubeDescriptorIndex : packoffset(c11.x);
	uint s12_Texture1DDescriptorIndex : packoffset(c15.x);
	uint s12_SamplerDescriptorIndex : packoffset(c19.x);
	uint s13_Texture2DDescriptorIndex : packoffset(c3.y);
	uint s13_Texture3DDescriptorIndex : packoffset(c7.y);
	uint s13_TextureCubeDescriptorIndex : packoffset(c11.y);
	uint s13_Texture1DDescriptorIndex : packoffset(c15.y);
	uint s13_SamplerDescriptorIndex : packoffset(c19.y);
	uint s14_Texture2DDescriptorIndex : packoffset(c3.z);
	uint s14_Texture3DDescriptorIndex : packoffset(c7.z);
	uint s14_TextureCubeDescriptorIndex : packoffset(c11.z);
	uint s14_Texture1DDescriptorIndex : packoffset(c15.z);
	uint s14_SamplerDescriptorIndex : packoffset(c19.z);
	uint s15_Texture2DDescriptorIndex : packoffset(c3.w);
	uint s15_Texture3DDescriptorIndex : packoffset(c7.w);
	uint s15_TextureCubeDescriptorIndex : packoffset(c11.w);
	uint s15_Texture1DDescriptorIndex : packoffset(c15.w);
	uint s15_SamplerDescriptorIndex : packoffset(c19.w);
	DEFINE_SHARED_CONSTANTS();
};

#endif

float4 loDepthSign(float4 g)
{
	float4 a = applyFetchSign(float4(g.x, g.y, g.z, 1.0), g_BiasedTextures, g_SintTexcoords, 2);
	float4 b = applyFetchSign(float4(g.w, 0.0, 0.0, 1.0), g_BiasedTextures, g_SintTexcoords, 2);
	return float4(a.x, a.y, a.z, b.x);
}

#ifndef __spirv__
[shader("pixel")]
#endif
void shaderMain(
	in float4 iPos : SV_Position,
	[[vk::location(0)]] in float4 iTexCoord0 : TEXCOORD0,
#ifdef __spirv__
	in bool iFace : SV_IsFrontFace
#else
	in uint iFace : SV_IsFrontFace
#endif
,
	out float4 oC0 : SV_Target0)
{
	float4 c252 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c253 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c254 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c255 = asfloat(uint4(0x38D1B717, 0x3F800000, 0x0, 0x0));

	float4 r0 = iTexCoord0;
	float4 r1 = 0.0;
	float4 r2 = 0.0;
	float4 r3 = 0.0;
	float4 r4 = 0.0;
	float4 r5 = 0.0;
	float4 r6 = 0.0;
	float4 r7 = 0.0;
	float4 r8 = 0.0;
	float4 r9 = 0.0;
	float4 r10 = 0.0;
	float4 r11 = 0.0;
	float4 r12 = 0.0;
	float4 r13 = 0.0;
	float4 r14 = 0.0;
	float4 r15 = 0.0;
	float4 r16 = 0.0;
	float4 r17 = 0.0;
	float4 r18 = 0.0;
	float4 r19 = 0.0;
	float4 r20 = 0.0;
	float4 r21 = 0.0;
	float4 r22 = 0.0;
	float4 r23 = 0.0;
	float4 r24 = 0.0;
	float4 r25 = 0.0;
	float4 r26 = 0.0;
	float4 r27 = 0.0;
	float4 r28 = 0.0;
	float4 r29 = 0.0;
	float4 r30 = 0.0;
	float4 r31 = 0.0;
	int a0 = 0;
	int aL = 0;
	bool p0 = false;
	float ps = 0.0;
	CubeMapData cubeMapData = (CubeMapData)0;

	r2.xyzw = applyFetchSign(tfetch2D(LoColor_Texture2DDescriptorIndex, LoColor_SamplerDescriptorIndex, r0.xy, float2(0.5, 0.5)), g_BiasedTextures, g_SintTexcoords, 0).xyzw;
	r3.xyzw = applyFetchSign(tfetch2D(LoColor_Texture2DDescriptorIndex, LoColor_SamplerDescriptorIndex, r0.xy, float2(-0.5, -0.5)), g_BiasedTextures, g_SintTexcoords, 0).xyzw;
	r4.xyzw = applyFetchSign(tfetch2D(LoColor_Texture2DDescriptorIndex, LoColor_SamplerDescriptorIndex, r0.xy, float2(0.5, -0.5)), g_BiasedTextures, g_SintTexcoords, 0).xyzw;
	r1.xyzw = applyFetchSign(tfetch2D(LoColor_Texture2DDescriptorIndex, LoColor_SamplerDescriptorIndex, r0.xy, float2(-0.5, 0.5)), g_BiasedTextures, g_SintTexcoords, 0).xyzw;
	r6.xy = getWeights2D(LoDepth_Texture2DDescriptorIndex, LoDepth_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xy;
	r0.z = applyFetchSign(tfetch2D(HiDepth_Texture2DDescriptorIndex, HiDepth_SamplerDescriptorIndex, r0.xy, float2(0, 0)), g_BiasedTextures, g_SintTexcoords, 1).x;
	{
		Texture2D<float4> loDepthTex = g_Texture2DDescriptorHeap[LoDepth_Texture2DDescriptorIndex];
		float4 g = loDepthSign(loDepthTex.GatherRed(g_SamplerDescriptorHeap[LoDepth_SamplerDescriptorIndex], r0.xy));
		r5.x = g.y;
		r5.y = g.x;
		r5.z = g.z;
		r5.w = g.w;
	}
	r0.xyzw = -r5.xyzw + r0.zzzz;
	r0.xyzw = abs(r0.xwyz) + c255.xxxx;
	ps = clamp(rcp(r0.x), FLT_MIN, FLT_MAX);
	r5.x = ps;
	ps = clamp(rcp(r0.w), FLT_MIN, FLT_MAX);
	r5.y = ps;
	r6.zw = -r6.yx + c255.yy;
	ps = clamp(rcp(r0.y), FLT_MIN, FLT_MAX);
	r5.z = ps;
	r6.xyzw = r6.xzww * r6.yxzy;
	ps = clamp(rcp(r0.z), FLT_MIN, FLT_MAX);
	r5.w = ps;
	r0.xyzw = r6.wxyz * r5.wxyz;
	r5.x = dot(r6.xwzy, r5.xwzy);
	r1.xyzw = r0.xxxx * r1.xzyw;
	ps = clamp(rcp(r5.x), FLT_MIN, FLT_MAX);
	r0.x = ps;
	r1.xyzw = r0.zzzz * r4.xzyw + r1.xyzw;
	r1.xyzw = r0.wwww * r3.xzyw + r1.xyzw;
	r1.xyzw = r0.yyyy * r2.xzyw + r1.xyzw;
	oC0.xyz = r1.xzy * r0.xxx;
	oC0.w = -r1.w * r0.x + c255.y;
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)	{		clip(oC0.w - g_AlphaThreshold);
	}	[branch] if (g_PackedDec3 & 0x80000000u) oC0.rgb = linearToPWLGamma(oC0.rgb);
	[branch] if (g_PackedDec3 & 0x40000000u) oC0 = saturate(oC0);
	return;
}