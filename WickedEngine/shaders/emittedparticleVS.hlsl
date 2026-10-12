#include "globals.hlsli"
#include "ShaderInterop_EmittedParticle.h"
#include "emittedparticleHF.hlsli"

#ifdef EMITTEDPARTICLE_LIGHTING
// GG: a lit particle is lit here, at each corner and at its centre, rather than by every pixel it covers (the pixel shader
// shades with them); no decals or voxel light on a particle
#define DISABLE_DECALS
#define DISABLE_VOXELGI
#include "objectHF.hlsli"

// the light at a point of the sprite facing N (world space), as the pixel shader lit it: the lights of the screen tile the
// point falls in (nearest the screen's edge for one off it), their shadows, the ambient and the environment's
float3 ParticleLight(float3 P, float3 N)
{
	Lighting lighting;
	lighting.create(0, 0, GetAmbient(N), 0);

	Surface surface;
	surface.create(g_xMaterial, float4(1, 1, 1, 1), 0);
	surface.P = P;
	surface.N = N;
	surface.V = 0;
	const float4 clip = mul(g_xCamera_VP, float4(P, 1));
	const float2 ndc = clip.xy / max(clip.w, 0.0001);
	surface.pixel = clamp((ndc * float2(0.5, -0.5) + 0.5) * g_xFrame_InternalResolution, 0, g_xFrame_InternalResolution - 1);
	surface.sss = g_xMaterial.subsurfaceScattering;
	surface.sss_inv = g_xMaterial.subsurfaceScattering_inv;
	surface.update();

	float3 envAmbient = 0;
	TiledLighting(surface, lighting, envAmbient);
	return lighting.direct.diffuse + lighting.indirect.diffuse + envAmbient;
}

// the sprite's rounded normal at a place on it (0-1 across, unrotated), as the pixel shader made it
float3 ParticleNormal(float2 unrotated_uv)
{
	float3 N;
	N.x = -cos(PI * unrotated_uv.x);
	N.y = cos(PI * unrotated_uv.y);
	N.z = -sin(PI * length(unrotated_uv));
	return normalize(mul((float3x3)g_xCamera_InvV, N));
}
#endif // EMITTEDPARTICLE_LIGHTING

#define PIHALF 1.5707963265f
static const float3 BILLBOARD[] = {
	float3(-1, -1, 0),	// 0
	float3(1, -1, 0),	// 1
	float3(-1, 1, 0),	// 2
	float3(1, 1, 0),	// 4
};

STRUCTUREDBUFFER(particleBuffer, Particle, TEXSLOT_ONDEMAND21);
STRUCTUREDBUFFER(aliveList, uint, TEXSLOT_ONDEMAND22);

VertextoPixel main(uint vertexID : SV_VERTEXID, uint instanceID : SV_INSTANCEID)
{
	VertextoPixel Out;

	// load particle data:
	Particle particle = particleBuffer[aliveList[instanceID]];

	// calculate render properties from life:
	float lifeLerp = 1 - particle.life / particle.maxLife;
	float size = lerp(particle.sizeBeginEnd.x, particle.sizeBeginEnd.y, lifeLerp);
	// GG: fully opaque for the first xParticleFadeOutStart of its life, then fading out (0 fades over all of it)
	const float fadeLerp = saturate((lifeLerp - xParticleFadeOutStart) / (1 - xParticleFadeOutStart));
	float opacity = saturate(lerp(1, 0, fadeLerp) * xEmitterOpacity);
	float rotation = lifeLerp * particle.rotationalVelocity;

	// expand the point into a billboard in view space:
	float3 quadPos = BILLBOARD[vertexID];
	quadPos.x = particle.color_mirror & 0x10000000 ? -quadPos.x : quadPos.x;
	quadPos.y = particle.color_mirror & 0x20000000 ? -quadPos.y : quadPos.y;
	float2 uv = quadPos.xy * float2(0.5f, -0.5f) + 0.5f;
	float2 uv2 = uv;

	// Sprite sheet UV transform:
	//PE: Changed to always move flibook forward.
	const float spriteframe = xEmitterFrameRate == 0 ? 
		lerp(xEmitterFrameStart, xEmitterFrameCount, lifeLerp) : 
		((xEmitterFrameStart + (particle.maxLife - particle.life) * xEmitterFrameRate) % xEmitterFrameCount);
	const uint currentFrame = floor(spriteframe);
	const uint nextFrame = ceil(spriteframe);
	const float frameBlend = frac(spriteframe);
	uint2 offset = uint2(currentFrame % xEmitterFramesXY.x, currentFrame / xEmitterFramesXY.x);
	uv.xy += offset;
	uv.xy *= xEmitterTexMul;
	uint2 offset2 = uint2(nextFrame % xEmitterFramesXY.x, nextFrame / xEmitterFramesXY.x);
	uv2.xy += offset2;
	uv2.xy *= xEmitterTexMul;

	//PE: Rotate to fit direction if top up view.
    float3 velocity = mul((float3x3) g_xCamera_View, particle.velocity);
    
    if (particle.velocity.x != 0 || particle.velocity.z != 0)
    {
        rotation += (tan(normalize(velocity)) * xParticleStartRotation);
    }
	
	// rotate the billboard:
	float2x2 rot = float2x2(
		cos(rotation), -sin(rotation),
		sin(rotation), cos(rotation)
		);
	quadPos.xy = mul(quadPos.xy, rot);

	// scale the billboard:
	quadPos *= size;

	// scale the billboard along view space motion vector:
	// GG: a wrapping volume's (xParticleLocalOptions 64) along its motion relative to the emitter, which moves with the
	// camera, so rain streaks past a fast craft: as long as its own motion gives at rest, longer with the speed between
	// them (the streak's length squared their two speeds' product)
	float3 streak = velocity;
	[branch]
	if ((xParticleLocalOptions & 64) && xEmitterPrevValid > 0 && g_xFrame_DeltaTime > 0)
	{
		const float3 emitterVelocity = (mul(xEmitterWorld, float4(0, 0, 0, 1)).xyz - xEmitterPrevPos) / g_xFrame_DeltaTime;
		const float3 relative = particle.velocity - emitterVelocity;
		const float relativeSpeed = length(relative);
		streak = relativeSpeed > 0 ? mul((float3x3)g_xCamera_View, relative) * sqrt(length(particle.velocity) / relativeSpeed) : 0;
	}
	quadPos += dot(quadPos, streak) * streak * xParticleMotionBlurAmount;


	// copy to output:
	Out.pos = float4(particle.position, 1);
	Out.pos = mul(g_xCamera_View, Out.pos);
	Out.pos.xyz += quadPos.xyz;
	Out.P = mul(g_xCamera_InvV, float4(Out.pos.xyz, 1)).xyz;
	Out.pos = mul(g_xCamera_Proj, Out.pos);

	Out.tex = float4(uv, uv2);
	Out.size = size;

	
	//PE: Fade in particle.
	float normalizedTime = clamp(lifeLerp / xEmitterFadeinTime, 0, 1);
	opacity = saturate(lerp(0, opacity, normalizedTime));

	//PE: Endcolor
    uint red = particle.color_mirror   & 0x000000FF;
    uint green = (particle.color_mirror & 0x0000FF00) >> 8;
    uint blue = (particle.color_mirror & 0x00FF0000) >> 16;
	
    uint endcolor_red = 255;
    uint endcolor_green = 255;
    uint endcolor_blue = 255;

    red = lerp(red, xParticleEndColorRed, lifeLerp);
    green = lerp(green, xParticleEndColorGreen, lifeLerp);
    blue = lerp(blue, xParticleEndColorBlue, lifeLerp);

	//Out.color = (particle.color_mirror & 0x00FFFFFF) | (uint(opacity * 255.0f) << 24);
    Out.color = (red + (green << 8) + (blue << 16)) | (uint(opacity * 255.0f) << 24);

	Out.unrotated_uv = quadPos.xy * float2(1, -1) / size * 0.5f + 0.5f;
	Out.frameBlend = frameBlend;

#ifdef EMITTEDPARTICLE_LIGHTING
	Out.litCorner = ParticleLight(Out.P, ParticleNormal(Out.unrotated_uv));
	Out.litCentre = ParticleLight(particle.position, ParticleNormal(float2(0.5, 0.5)));
#endif // EMITTEDPARTICLE_LIGHTING

	return Out;
}
