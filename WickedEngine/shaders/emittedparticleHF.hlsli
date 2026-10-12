#ifndef WI_EMITTEDPARTICLE_HF
#define WI_EMITTEDPARTICLE_HF

struct VertextoPixel
{
	float4 pos						: SV_POSITION;
	float4 tex						: TEXCOORD0;
	nointerpolation float size		: TEXCOORD1;
	nointerpolation uint color		: TEXCOORD2;
	float3 P : WORLDPOSITION;
	nointerpolation float frameBlend : FRAMEBLEND;
	float2 unrotated_uv : UNROTATED_UV;
#ifdef EMITTEDPARTICLE_LIGHTING
	// GG: lit in the vertex shader, at this corner and at the sprite's centre
	float3 litCorner : LITCORNER;
	nointerpolation float3 litCentre : LITCENTRE;
#endif // EMITTEDPARTICLE_LIGHTING
};

#endif // WI_EMITTEDPARTICLE_HF
