struct ResolveConstants {
  float exponentScale;
  float2 uvMin;
  float2 uvMax;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> g_Resolve : register(b0);

Texture2D<float4> g_Source : register(t0);
SamplerState g_Sampler : register(s1);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD)
    : SV_Target {
  const float2 uv =
      lerp(g_Resolve.uvMin, g_Resolve.uvMax, saturate(texCoord));
  return g_Source.Sample(g_Sampler, uv) * g_Resolve.exponentScale;
}
