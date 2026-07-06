Texture2D<float4> g_Source : register(t0);
SamplerState g_Sampler : register(s1);

static const float kInvGamma = 1.0 / 2.2;

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD)
    : SV_Target {
  float3 c = g_Source.Sample(g_Sampler, texCoord).rgb;
  return float4(pow(max(c, 0.0), kInvGamma), 1.0);
}
