#ifndef GG_DECALHF
#define GG_DECALHF

// GameGuru's projected decals, applied by the object shaders' decal loops (objectHF.hlsli) and the terrain's
// (GGTerrainVirtualPBR_PS.hlsl) through GGDecalColor. A decal without a GameGuru flag is Wicked's planar decal, unchanged.
// - ENTITY_FLAG_DECAL_FACING (wiRenderer.cpp, from DecalComponent::facing): paints only surfaces whose geometric normal
//   faces along the decal's Z by more than the cosine in its cone angle slot, fading in (smoothstep) over the width in the
//   top 16 bits of energy16_X16 (GGDecalFacingFade; 0.1 unless set), so the inside face of a thin wall and the ground under an
//   upright box stay clean
// - ENTITY_FLAG_DECAL_BLAST (from DecalComponent::blast): a blast round the decal's centre. Its box is a cube of half-size
//   radius, and it paints the sphere inside it, fading out over the outer quarter, on every surface facing the centre by
//   more than the cone angle cosine. Each surface takes the texture from the box's planes it faces most (triplanar), the
//   weights sharpened by the power in the direction slot's x, so a flat face takes one unstretched sample. userdata holds
//   how far the first surface lies along each of the box's six axes (5 bits each from bit 0: +x, -x, +y, -y, +z, -z, in
//   30ths of the radius, 31 for none), and a point further along the axis nearest its direction from the centre lies
//   behind that surface and stays clean. When the surface hit is a flat face across one of the box's axes, the direction
//   slot's y is the direction into it (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z; below 0 for none) and its z how far along it a
//   point may lie (a little past the face): anything behind the surface hit stays clean, such as a room behind a wall
// Every decal is a matte, non-metal coat (GGDecalCoat, after the decals are blended into the albedo), or a dark one would
// not show on metal, whose albedo is black and whose colour is all in its reflection.
// This file is in both GameGuru's CustomShaders and Wicked's shaders folder (for Wicked's own objectHF.hlsli, which the
// offline shader compiler builds), and the two copies must stay the same

// the geometric normal of the surface being shaded (before normal mapping), set by the pixel shader before its lighting
static float3 decal_faceN = float3(0, 1, 0);

// GameGuru's decals' normal maps (packed in the decal atlas beside their textures, their place in cubeRemap: wiRenderer.cpp),
// as a world space normal premultiplied by their coverage (in w): GGDecalColor gathers it in the order the decal loops blend
// the colour, and GGDecalCoat lays it over the surface's normal after the loop. The normal map's frame is the decal box's
// own round the geometric normal, so it needs no tangents on the surface (the terrain has none) and is right on every face
// a blast's triplanar planes cover
static float4 decal_normal = 0;

// a decal's normal map sample turned into a world space normal: its red along T (the texture's u in the world), its green
// along B (its v; DirectX normal maps, as the models'), both laid on the surface round N
inline float3 GGDecalNormalWorld(in float4 normalSample, in float3 T, in float3 B, in float3 N)
{
	const float2 tilt = normalSample.rg * 2 - 1;
	float3 Ts = T - N * dot(T, N);
	float3 Bs = B - N * dot(B, N);
	Ts /= max(length(Ts), 0.0001);
	Bs /= max(length(Bs), 0.0001);
	return normalize(tilt.x * Ts + tilt.y * Bs + N);
}

// how a GameGuru decal fades in past its facing cutoff: 0 at the cutoff, full at the cutoff plus the width kept in the top
// 16 bits of energy16_X16 (the emissive energy is the low 16)
inline float GGDecalFacingBlend(in ShaderEntity decal, in float facing)
{
	const float cutoff = decal.GetConeAngleCos();
	const float fade = max(f16tof32(decal.energy16_X16 >> 16), 0.001);
	return smoothstep(cutoff, cutoff + fade, facing);
}

// one of a blast decal's three planes: the position and its gradients in the box's units (-1 to 1 across it)
inline float4 GGBlastDecalSample(in float2 planePos, in float2 planeDX, in float2 planeDY, in float4 texMulAdd)
{
	const float2 uvScale = float2(0.5, -0.5) * texMulAdd.xy;
	return texture_decalatlas.SampleGrad(sampler_linear_clamp, (planePos * float2(0.5, -0.5) + 0.5) * texMulAdd.xy + texMulAdd.zw, planeDX * uvScale, planeDY * uvScale);
}

// a blast decal's colour at P, boxPos being P in its box (-1 to 1 across it); with a normal map (bNormal, at normalMulAdd in
// the atlas), decalN its normal there, each plane's sample laid on that plane's axes and weighted as the colour
inline float4 GGBlastDecalColor(in ShaderEntity decal, in float4x4 decalProjection, in float4 texMulAdd, in bool bNormal, in float4 normalMulAdd, in float3 boxPos, in float3 P, in float3 P_dx, in float3 P_dy, out float edgeBlend, out float3 decalN)
{
	edgeBlend = 0;
	decalN = 0;
	float4 decalColor = 0;

	// the sphere, fading out over its outer quarter (none from its edge out), on surfaces facing the centre, fading in past
	// the cutoff
	const float3 toCentre = decal.position - P;
	const float lenToCentre = length(toCentre);
	const float facing = lenToCentre > 0.001 ? dot(decal_faceN, toCentre) / lenToCentre : 1;
	float blend = (1 - smoothstep(0.75, 1, length(boxPos))) * GGDecalFacingBlend(decal, facing);

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
		// the box's axes in the world (its rows are the gradients of its x, y and z), for the normal map: a plane's u runs
		// along its first axis and its v against its second, as GGBlastDecalSample reads it
		const float3 axisX = normalize(decalProjection[0].xyz);
		const float3 axisY = normalize(decalProjection[1].xyz);
		const float3 axisZ = normalize(decalProjection[2].xyz);
		float weightSum = 0;
		[branch]
		if (weights.x > 0.01)
		{
			decalColor += weights.x * GGBlastDecalSample(boxPos.zy, boxDX.zy, boxDY.zy, texMulAdd);
			if (bNormal)
				decalN += weights.x * GGDecalNormalWorld(GGBlastDecalSample(boxPos.zy, boxDX.zy, boxDY.zy, normalMulAdd), axisZ, -axisY, decal_faceN);
			weightSum += weights.x;
		}
		[branch]
		if (weights.y > 0.01)
		{
			decalColor += weights.y * GGBlastDecalSample(boxPos.xz, boxDX.xz, boxDY.xz, texMulAdd);
			if (bNormal)
				decalN += weights.y * GGDecalNormalWorld(GGBlastDecalSample(boxPos.xz, boxDX.xz, boxDY.xz, normalMulAdd), axisX, -axisZ, decal_faceN);
			weightSum += weights.y;
		}
		[branch]
		if (weights.z > 0.01)
		{
			decalColor += weights.z * GGBlastDecalSample(boxPos.xy, boxDX.xy, boxDY.xy, texMulAdd);
			if (bNormal)
				decalN += weights.z * GGDecalNormalWorld(GGBlastDecalSample(boxPos.xy, boxDX.xy, boxDY.xy, normalMulAdd), axisX, -axisY, decal_faceN);
			weightSum += weights.z;
		}
		decalColor /= max(weightSum, 0.0001);
		if (bNormal) decalN = normalize(decalN);

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
	// the normal map, the texture's size, at the atlas place in cubeRemap (16 bits each of u and v, in 65535ths; 0 for none)
	const bool bNormal = decal.cubeRemap != 0;
	const float4 normalMulAdd = float4(texMulAdd.xy, float2(decal.cubeRemap & 0xFFFF, decal.cubeRemap >> 16) / 65535.0);
	float3 decalN = 0;

	[branch]
	if (decal.GetFlags() & ENTITY_FLAG_DECAL_BLAST)
	{
		decalColor = GGBlastDecalColor(decal, decalProjection, texMulAdd, bNormal, normalMulAdd, clipSpacePos, P, P_dx, P_dy, edgeBlend, decalN);
	}
	else
	{
		const float3 uvw = clipSpacePos.xyz * float3(0.5, -0.5, 0.5) + 0.5;
		float facingBlend = 1;
		if (decal.GetFlags() & ENTITY_FLAG_DECAL_FACING)
		{
			// the decal's Z in world space is the gradient of its box's z
			const float facing = dot(decal_faceN, normalize(decalProjection[2].xyz));
			facingBlend = GGDecalFacingBlend(decal, facing);
		}
		[branch]
		if (is_saturated(uvw) && facingBlend > 0)
		{
			// mipmapping needs to be performed by hand:
			const float2 decalDX = mul(P_dx, (float3x3)decalProjection).xy * texMulAdd.xy;
			const float2 decalDY = mul(P_dy, (float3x3)decalProjection).xy * texMulAdd.xy;
			decalColor = texture_decalatlas.SampleGrad(sampler_linear_clamp, uvw.xy * texMulAdd.xy + texMulAdd.zw, decalDX, decalDY);
			[branch]
			if (bNormal)
			{
				// u along the box's x, v against its y
				const float4 normalSample = texture_decalatlas.SampleGrad(sampler_linear_clamp, uvw.xy * normalMulAdd.xy + normalMulAdd.zw, decalDX, decalDY);
				decalN = GGDecalNormalWorld(normalSample, normalize(decalProjection[0].xyz), -normalize(decalProjection[1].xyz), decal_faceN);
			}
			// blend out if close to cube Z:
			edgeBlend = (1 - pow(saturate(abs(clipSpacePos.z)), 8)) * facingBlend;
			decalColor.a *= edgeBlend;
			decalColor *= decal.GetColor();
		}
	}
	// its normal gathered as the decal loops blend the colour, for GGDecalCoat
	[branch]
	if (bNormal && decalColor.a > 0 && any(decalN))
	{
		decal_normal.xyz = (1 - decal_normal.w) * decalColor.a * decalN + decal_normal.xyz;
		decal_normal.w = decalColor.a + (1 - decalColor.a) * decal_normal.w;
	}
	return decalColor;
}

// after a surface's decals are blended into its albedo, coverage being how much they cover: under them the surface is a
// matte, non-metal coat, its normal the decals' normal maps' where they have them (decal_normal). Its lighting terms were
// worked out before the decals, so they are worked out again, only where a decal lies
inline void GGDecalCoat(inout Surface surface, in float coverage)
{
	[branch]
	if (coverage > 0)
	{
		if (decal_normal.w > 0)
			surface.N = normalize(surface.N * (1 - decal_normal.w) + decal_normal.xyz);
		surface.f0 = lerp(surface.f0, 0.04, coverage);
		surface.metalness = lerp(surface.metalness, 0, coverage);
		surface.roughness = lerp(surface.roughness, 1, coverage);
		surface.update();
	}
}

#endif // GG_DECALHF
