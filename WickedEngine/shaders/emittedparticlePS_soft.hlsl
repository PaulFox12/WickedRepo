#include "globals.hlsli"
#include "emittedparticleHF.hlsli"
#include "ShaderInterop_EmittedParticle.h"
#include "objectHF.hlsli"

TEXTURE2D(texture_color, float4, TEXSLOT_ONDEMAND0);
TEXTURE2D(texture_emissive, float4, TEXSLOT_ONDEMAND1);

[earlydepthstencil]
float4 main(VertextoPixel input) : SV_TARGET
{
    float4 color = texture_color.Sample(sampler_linear_clamp, input.tex.xy);

	[branch]
	if (xEmitterOptions & EMITTER_OPTION_BIT_FRAME_BLENDING_ENABLED)
	{
	    float4 color2 = texture_color.Sample(sampler_linear_clamp, input.tex.zw);
		color = lerp(color, color2, input.frameBlend);
	}

	float2 pixel = input.pos.xy;
	float2 ScreenCoord = pixel * g_xFrame_InternalResolution_rcp;
	// GG: drawn at half resolution, so the scene's depth (full resolution) is at twice the pixel: its gather the farthest of
	// the 2x2 pixels this one covers, which the half resolution composite matches
	if (xParticleLocalOptions & 32) ScreenCoord *= 2;
	float4 depthScene = texture_lineardepth.GatherRed(sampler_linear_clamp, ScreenCoord) * g_xCamera_ZFarP;
	float depthFragment = input.pos.w;
	float fade = saturate(1.0 / input.size*(max(max(depthScene.x, depthScene.y), max(depthScene.z, depthScene.w)) - depthFragment));

	float4 inputColor;
	inputColor.r = ((input.color >> 0)  & 0xFF) / 255.0f;
	inputColor.g = ((input.color >> 8)  & 0xFF) / 255.0f;
	inputColor.b = ((input.color >> 16) & 0xFF) / 255.0f;
	inputColor.a = ((input.color >> 24) & 0xFF) / 255.0f;

	float opacity = saturate(color.a * inputColor.a * fade);

	// GG: with an emissive map (on the same frames as the colour), only what it marks glows, by the emissive strength;
	// without one the whole particle is brightened by it as before
	float3 emissive = 0;
	[branch]
	if (xParticleEmissiveMap)
	{
		float3 emissiveColor = texture_emissive.Sample(sampler_linear_clamp, input.tex.xy).rgb;
		[branch]
		if (xEmitterOptions & EMITTER_OPTION_BIT_FRAME_BLENDING_ENABLED)
		{
			emissiveColor = lerp(emissiveColor, texture_emissive.Sample(sampler_linear_clamp, input.tex.zw).rgb, input.frameBlend);
		}
		color.rgb *= inputColor.rgb;
		emissive = emissiveColor * inputColor.rgb * xParticleEmissive;
	}
	else
	{
		color.rgb *= inputColor.rgb * (1 + xParticleEmissive);
	}
	color.a = opacity;

#ifdef EMITTEDPARTICLE_DISTORTION
	// just make normal maps blendable:
	color.rgb = color.rgb - 0.5f;
#endif // EMITTEDPARTICLE_DISTORTION

#ifdef EMITTEDPARTICLE_LIGHTING

	// GG: lit in the vertex shader at the sprite's corners and its centre, not by every pixel: the centre's light in the
	// middle, the corners' towards the edges, so a big sprite keeps the rounded shading the pixels gave it
	const float edge = saturate(length(input.unrotated_uv - 0.5) * 2);
	color.rgb *= lerp(input.litCentre, input.litCorner, edge);

#endif // EMITTEDPARTICLE_LIGHTING

#ifndef EMITTEDPARTICLE_DISTORTION
	color.rgb += emissive;
#endif // EMITTEDPARTICLE_DISTORTION

	// GG: the scene's fog, as objects have it, unless the emitter skips it (xParticleLocalOptions bit 3): an additive particle
	// dims instead of adding the fog's colour, a premultiplied one takes it by its opacity, and a distortion bends less
	[branch]
	if ((xParticleLocalOptions & 4) == 0)
	{
		float3 V = g_xCamera_CamPos - input.P;
		const float dist = length(V);
		V /= max(dist, 0.0001);
		float3 fogColor;
		const float fogAmount = GetFogColorAndAmount(dist, g_xCamera_CamPos, V, fogColor);
#ifdef EMITTEDPARTICLE_DISTORTION
		color.rgb *= 1 - fogAmount;
#else
		if (xParticleLocalOptions & 8)
		{
			color.rgb *= 1 - fogAmount;
		}
		else
		{
			color.rgb = lerp(color.rgb, (xParticleLocalOptions & 16) ? fogColor * color.a : fogColor, fogAmount);
		}
#endif // EMITTEDPARTICLE_DISTORTION
	}

	return max(color, 0);
}
