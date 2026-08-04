Texture2D<float4> g_Source : register(t0);
SamplerState g_Sampler : register(s1);

float3 LinearToSrgb(float3 c) {
  c = max(c, 0.0);
  const float3 lo = c * 12.92;
  const float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
  return select(c <= 0.0031308, lo, hi);
}

float3 TonemapReinhard(float3 c) {
  const float kWhite = 13.0;
  const float3 num = c * (1.0 + c / (kWhite * kWhite));
  return num / (1.0 + c);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD)
    : SV_Target {
  float3 c = max(g_Source.Sample(g_Sampler, texCoord).rgb, 0.0);
  c = TonemapReinhard(c);
  return float4(LinearToSrgb(c), 1.0);
}
