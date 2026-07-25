struct ResolveConstants {
  float exponentScale;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> g_Resolve : register(b0);

Texture2D<float4> g_Source : register(t0);
SamplerState g_Sampler : register(s1);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD)
    : SV_Target {
  return g_Source.Sample(g_Sampler, texCoord) * g_Resolve.exponentScale;
}
