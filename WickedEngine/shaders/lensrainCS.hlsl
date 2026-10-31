#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"
#include "lensrainHF.hlsli"

// GG: rain on the lens, after the tone map (Postprocess_LensRain): the drops of lensrainHF.hlsli over the image, each
// showing the scene through it; the fog, if any, blurs the image between them

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);

RWTEXTURE2D(output, unorm float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;
	const float aspect = xPPResolution.x * xPPResolution_rcp.y;

	LensRainParams rain;
	rain.amount = xPPParams0.x;
	rain.size = xPPParams0.y;
	rain.flow = xPPParams0.zw;
	rain.slide = xPPParams1.x;
	rain.refraction = xPPParams1.y;
	rain.fog = xPPParams1.z;
	rain.time = xPPParams1.w;

	float wiped;
	const LensRainDrop drop = LensRain_Drops(float2(uv.x * aspect, uv.y), rain, wiped);

	float4 color = input[DTid.xy];
	if (rain.fog > 0)
	{
		float3 blur = color.rgb;
		const float radius = 0.004 + 0.012 * rain.fog;
		[unroll]
		for (int i = 0; i < 8; ++i)
		{
			const float a = i * 0.785398 + 0.392699;
			blur += input.SampleLevel(sampler_linear_clamp, uv + float2(cos(a) / aspect, sin(a)) * radius, 0).rgb;
		}
		color.rgb = lerp(color.rgb, blur / 9.0, rain.fog * (1 - wiped));
	}
	if (drop.mask > 0)
	{
		float3 through = input.SampleLevel(sampler_linear_clamp, uv + float2(drop.offset.x / aspect, drop.offset.y), 0).rgb;
		through = through * (1 - 0.6 * drop.rim) + drop.spec * 0.5;
		color.rgb = lerp(color.rgb, through, drop.mask);
	}

	output[DTid.xy] = saturate(color);
}
