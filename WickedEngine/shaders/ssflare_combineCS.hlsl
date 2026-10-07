#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

// GG: the screen-space lens flare added to the image (Postprocess_ScreenLensFlare), times its intensity

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);
TEXTURE2D(texture_flare, float3, TEXSLOT_ONDEMAND1);

RWTEXTURE2D(output, float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;
	const float intensity = xPPParams0.x;

	float3 flare = texture_flare.SampleLevel(sampler_linear_clamp, uv, 0) * intensity;

	output[DTid.xy] = input[DTid.xy] + float4(flare, 0);
}
