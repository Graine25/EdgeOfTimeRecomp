#pragma once

struct PushConstants
{
#ifdef __spirv__
    [[vk::offset(24)]]
#endif
    uint   ResourceDescriptorIndex;
    uint   ResourceDescriptorIndex2;
    float  Param0;
    float  Param1;
    float4 SourceRect;
    float4 ColorAdjust;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants : register(b3, space4);
