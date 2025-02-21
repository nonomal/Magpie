Texture2D tex : register(t0);
SamplerState sam : register(s0);

float4 main(float2 coord : TEXCOORD) : SV_Target {
	return tex.Sample(sam, coord);
}
