#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);
TEXTURE2D(texture_bloom, float3, TEXSLOT_ONDEMAND1);

RWTEXTURE2D(output, float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;

	float3 bloom = texture_bloom.SampleLevel(sampler_linear_clamp, uv, 1.5f);
	bloom += texture_bloom.SampleLevel(sampler_linear_clamp, uv, 3.5f);
	bloom += texture_bloom.SampleLevel(sampler_linear_clamp, uv, 4.0f); // GG: was 4.5, clamped to the last of 5 mips
	bloom /= 3.0f;

	// GG: wider halos (SetBloom's radius): the two mips past them added in, 0 as before
	const float bloomRadius = xPPParams0.y;
	[branch]
	if (bloomRadius > 0)
	{
		bloom += bloomRadius * 0.5f * (texture_bloom.SampleLevel(sampler_linear_clamp, uv, 5.0f) + texture_bloom.SampleLevel(sampler_linear_clamp, uv, 6.0f));
	}

	float strength = xPPParams0.x;
	bloom *= strength;

	output[DTid.xy] = input[DTid.xy] + float4(bloom, 0);
}
