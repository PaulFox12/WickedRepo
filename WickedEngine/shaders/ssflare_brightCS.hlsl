#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

// GG: the screen-space lens flare's bright pass (Postprocess_ScreenLensFlare): what is over its own threshold, at a quarter
// of the resolution, each sample capped; the sky can be left out, as the sun has its own flare

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);

RWTEXTURE2D(output, float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = DTid.xy;
	const float threshold = xPPParams0.x;
	const float cap = xPPParams0.y;
	const bool noSky = xPPParams0.z > 0;

	float3 color = 0;
	[unroll]
	for (uint i = 0; i < 4; ++i)
	{
		const float2 tapUV = (uv + float2((i & 1) ? 0.75f : 0.25f, (i & 2) ? 0.75f : 0.25f)) * xPPResolution_rcp;
		if (noSky && texture_depth.SampleLevel(sampler_point_clamp, tapUV, 0) == 0)
			continue;
		color += min(input.SampleLevel(sampler_linear_clamp, tapUV, 0).rgb, cap);
	}
	color /= 4.0f;

	color = max(color - threshold, 0);

	output[DTid.xy] = float4(color, 1);
}
