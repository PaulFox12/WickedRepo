#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

// GG: the screen-space lens flare's ghosts and halo (Postprocess_ScreenLensFlare), from the blurred bright pass: each bright
// spot repeats along the line through the centre of the screen, mirrored, and as a ring round it, with a colour fringe, so
// each ghost takes its source's colour

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);

RWTEXTURE2D(output, float4, 0);

float3 SampleFringe(float2 uv, float2 fringe, float mip)
{
	return float3(
		input.SampleLevel(sampler_linear_clamp, uv + fringe, mip).r,
		input.SampleLevel(sampler_linear_clamp, uv, mip).g,
		input.SampleLevel(sampler_linear_clamp, uv - fringe, mip).b
	);
}

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;
	const float spacing = xPPParams0.x;
	const float haloRadius = xPPParams0.y;
	const float fringeWidth = xPPParams0.z;
	const uint ghosts = (uint)xPPParams0.w;
	const float mip = xPPParams1.x;

	const float2 flipped = 1 - uv;
	const float2 ghostVec = (0.5f - flipped) * spacing;
	const float ghostLength = length(ghostVec);
	const float2 dir = ghostLength > 0.0001f ? ghostVec / ghostLength : float2(0, 0);
	const float2 fringe = dir * fringeWidth;
	const float centreDist = length(float2(0.5f, 0.5f));

	float3 result = 0;
	for (uint i = 0; i < ghosts; ++i)
	{
		const float2 ghostUV = frac(flipped + ghostVec * i);
		float weight = 1.0f - length(0.5f - ghostUV) / centreDist;
		weight = pow(saturate(weight), 10.0f);
		result += SampleFringe(ghostUV, fringe, mip) * weight;
	}

	// the halo: a ring at the given radius round the centre
	const float2 haloUV = frac(flipped + dir * haloRadius);
	float haloWeight = 1.0f - length(0.5f - haloUV) / centreDist;
	haloWeight = pow(saturate(haloWeight), 5.0f);
	result += SampleFringe(haloUV, fringe, mip) * haloWeight;

	output[DTid.xy] = float4(result, 1);
}
