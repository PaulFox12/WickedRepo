#ifndef GG_DECALHF
#define GG_DECALHF

// GameGuru's projected decals, applied by the object shaders' decal loops (objectHF.hlsli) and the terrain's
// (GGTerrainVirtualPBR_PS.hlsl) through GGDecalColor. A decal without a GameGuru flag is Wicked's planar decal, unchanged.
// - ENTITY_FLAG_DECAL_FACING (wiRenderer.cpp, from DecalComponent::facing): paints only surfaces whose geometric normal
//   faces along the decal's Z by more than the cosine in its cone angle slot, fading in over the next 0.1, so the inside
//   face of a thin wall and the ground under an upright box stay clean
// - ENTITY_FLAG_DECAL_BLAST (from DecalComponent::blast): a blast round the decal's centre. Its box is a cube of half-size
//   radius, and it paints the sphere inside it, fading out over the outer quarter, on every surface facing the centre by
//   more than the cone angle cosine. Each surface takes the texture from the box's planes it faces most (triplanar), the
//   weights sharpened by the power in the direction slot's x, so a flat face takes one unstretched sample. userdata holds
//   how far the first surface lies along each of the box's six axes (5 bits each from bit 0: +x, -x, +y, -y, +z, -z, in
//   30ths of the radius, 31 for none), and a point further along the axis nearest its direction from the centre lies
//   behind that surface and stays clean. When the surface hit is a flat face across one of the box's axes, the direction
//   slot's y is the direction into it (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z; below 0 for none) and its z how far along it a
//   point may lie (a little past the face): anything behind the surface hit stays clean, such as a room behind a wall
// This file is in both GameGuru's CustomShaders and Wicked's shaders folder (for Wicked's own objectHF.hlsli, which the
// offline shader compiler builds), and the two copies must stay the same

// the geometric normal of the surface being shaded (before normal mapping), set by the pixel shader before its lighting
static float3 decal_faceN = float3(0, 1, 0);

// one of a blast decal's three planes: the position and its gradients in the box's units (-1 to 1 across it)
inline float4 GGBlastDecalSample(in float2 planePos, in float2 planeDX, in float2 planeDY, in float4 texMulAdd)
{
	const float2 uvScale = float2(0.5, -0.5) * texMulAdd.xy;
	return texture_decalatlas.SampleGrad(sampler_linear_clamp, (planePos * float2(0.5, -0.5) + 0.5) * texMulAdd.xy + texMulAdd.zw, planeDX * uvScale, planeDY * uvScale);
}

// a blast decal's colour at P, boxPos being P in its box (-1 to 1 across it)
inline float4 GGBlastDecalColor(in ShaderEntity decal, in float4x4 decalProjection, in float4 texMulAdd, in float3 boxPos, in float3 P, in float3 P_dx, in float3 P_dy, out float edgeBlend)
{
	edgeBlend = 0;
	float4 decalColor = 0;

	// the sphere, fading out over its outer quarter (none from its edge out), on surfaces facing the centre, fading in over
	// the 0.1 past the cutoff
	const float3 toCentre = decal.position - P;
	const float lenToCentre = length(toCentre);
	const float facing = lenToCentre > 0.001 ? dot(decal_faceN, toCentre) / lenToCentre : 1;
	float blend = (1 - smoothstep(0.75, 1, length(boxPos))) * saturate((facing - decal.GetConeAngleCos()) * 10);

	// nothing behind the first surface along the box's axis nearest the point's direction from the centre
	const float3 absPos = abs(boxPos);
	const uint axis = absPos.x >= absPos.y ? (absPos.x >= absPos.z ? 0 : 2) : (absPos.y >= absPos.z ? 1 : 2);
	const float along = axis == 0 ? boxPos.x : (axis == 1 ? boxPos.y : boxPos.z);
	const uint firstSurface = (decal.userdata >> ((axis * 2 + (along < 0 ? 1 : 0)) * 5)) & 31;
	if (firstSurface < 31 && abs(along) > firstSurface / 30.0 + 0.06)
		blend = 0;

	// nothing behind the surface hit, when it is a flat face across one of the box's axes
	const float3 blastParams = decal.GetDirection();
	if (blastParams.y >= 0)
	{
		const uint hitDirection = (uint)(blastParams.y + 0.5);
		const uint hitAxis = hitDirection / 2;
		const float alongHit = (hitAxis == 0 ? boxPos.x : (hitAxis == 1 ? boxPos.y : boxPos.z)) * ((hitDirection & 1) ? -1 : 1);
		if (alongHit > blastParams.z)
			blend = 0;
	}

	[branch]
	if (blend > 0)
	{
		// triplanar: each plane weighted by how squarely the surface faces it, sharpened, and planes under 0.01 skipped
		const float3 boxN = normalize(mul((float3x3)decalProjection, decal_faceN));
		const float sharpness = max(1, blastParams.x);
		float3 weights = pow(abs(boxN), float3(sharpness, sharpness, sharpness));
		weights /= weights.x + weights.y + weights.z;
		const float3 boxDX = mul((float3x3)decalProjection, P_dx);
		const float3 boxDY = mul((float3x3)decalProjection, P_dy);
		float weightSum = 0;
		[branch]
		if (weights.x > 0.01)
		{
			decalColor += weights.x * GGBlastDecalSample(boxPos.zy, boxDX.zy, boxDY.zy, texMulAdd);
			weightSum += weights.x;
		}
		[branch]
		if (weights.y > 0.01)
		{
			decalColor += weights.y * GGBlastDecalSample(boxPos.xz, boxDX.xz, boxDY.xz, texMulAdd);
			weightSum += weights.y;
		}
		[branch]
		if (weights.z > 0.01)
		{
			decalColor += weights.z * GGBlastDecalSample(boxPos.xy, boxDX.xy, boxDY.xy, texMulAdd);
			weightSum += weights.z;
		}
		decalColor /= max(weightSum, 0.0001);

		edgeBlend = blend;
		decalColor.a *= blend;
		decalColor *= decal.GetColor();
	}
	return decalColor;
}

// the decal's colour at P, and in edgeBlend how much of it applies there (0 where it paints nothing), which also scales
// its emissive
inline float4 GGDecalColor(in ShaderEntity decal, in float3 P, in float3 P_dx, in float3 P_dy, out float edgeBlend)
{
	edgeBlend = 0;
	float4 decalColor = 0;
	float4x4 decalProjection = MatrixArray[decal.GetMatrixIndex()];
	const float4 texMulAdd = decalProjection[3];
	decalProjection[3] = float4(0, 0, 0, 1);
	const float3 clipSpacePos = mul(decalProjection, float4(P, 1)).xyz;

	[branch]
	if (decal.GetFlags() & ENTITY_FLAG_DECAL_BLAST)
	{
		decalColor = GGBlastDecalColor(decal, decalProjection, texMulAdd, clipSpacePos, P, P_dx, P_dy, edgeBlend);
	}
	else
	{
		const float3 uvw = clipSpacePos.xyz * float3(0.5, -0.5, 0.5) + 0.5;
		float facingBlend = 1;
		if (decal.GetFlags() & ENTITY_FLAG_DECAL_FACING)
		{
			// the decal's Z in world space is the gradient of its box's z
			const float facing = dot(decal_faceN, normalize(decalProjection[2].xyz));
			facingBlend = saturate((facing - decal.GetConeAngleCos()) * 10);
		}
		[branch]
		if (is_saturated(uvw) && facingBlend > 0)
		{
			// mipmapping needs to be performed by hand:
			const float2 decalDX = mul(P_dx, (float3x3)decalProjection).xy * texMulAdd.xy;
			const float2 decalDY = mul(P_dy, (float3x3)decalProjection).xy * texMulAdd.xy;
			decalColor = texture_decalatlas.SampleGrad(sampler_linear_clamp, uvw.xy * texMulAdd.xy + texMulAdd.zw, decalDX, decalDY);
			// blend out if close to cube Z:
			edgeBlend = (1 - pow(saturate(abs(clipSpacePos.z)), 8)) * facingBlend;
			decalColor.a *= edgeBlend;
			decalColor *= decal.GetColor();
		}
	}
	return decalColor;
}

#endif // GG_DECALHF
