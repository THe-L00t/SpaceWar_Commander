// ============================================================
//  RtCommon.hlsli — RT 단계 공용 (2026-10-27 렌더 4단계)
//
//  [4a] RtShadow.hlsl (그림자, 컴퓨트 RayQuery) 와 [4b] RtReflection.hlsl (반사, DispatchRays) 가 같이 쓴다.
//    · RtMaterial  : 재질 표 한 칸 — GRenderer.cpp 의 RtMaterialGpu 와 같은 배치(96바이트 → 9단계에서 128바이트)
//    · RtInstance  : TLAS 인스턴스 표 한 칸 — GRenderer.cpp 의 RtInstanceGpu 와 같은 배치(16바이트)
//    · TraceSunVisibility : 태양 가시도(RGB). 2단계 RtShadow.hlsl 의 루프를 옮겨 왔다 —
//                           반사 히트 지점의 그림자도 같은 규칙(투과 물체는 한 번씩 곱함)으로 구하려고.
//    · (5단계) PcgHash · Random2 · BuildOrthonormalBasis · SampleGgxVndf : B 방식 반사 레이 방향 뽑기
// ============================================================
#ifndef RT_COMMON_HLSLI
#define RT_COMMON_HLSLI

// 재질 표 — 재질 번호(TLAS InstanceID)로 찾는다.
//  텍스처 번호는 SM 6.6 바인드리스 힙(ResourceDescriptorHeap) 번호다. 텍스처가 없는 슬롯에는 렌더러가 흰색 · 평면 노멀 폴백 번호를 넣는다.
struct RtMaterial
{
	float4 baseColor;
	float3 emissive;          float roughness;
	float  metallic;          uint  hasNormalMap;     uint  doubleSided;   uint _pad0;   // 7단계 — 양면 재질(뒷면을 맞아도 «벽 속» 이 아니다)
	uint4  textures;          // x = BaseColor  y = Normal  z = Roughness  w = Metallic
	uint   emissiveTexture;   uint3 _pad1;
	float3 transmittance;     uint  transmissive;     // 그림자 레이가 이 물체 하나를 지나고 남는 빛 (2단계)
	// 9단계 — [7] 반투명 패스가 읽는 투과 · 부피 값 (glTF KHR_materials_transmission · volume · ior 그대로)
	float  transmission;      float ior;              float thickness;     float attenuationDistance;
	float3 attenuationColor;  float alphaCutoff;      // 2026-10-28 — 알파 마스크 기준(glTF alphaCutoff). 0 = 마스크 아님
};

// TLAS 인스턴스 표 — 인스턴스 순번(InstanceIndex / CandidateInstanceIndex)으로 찾는다. 프레임마다 다시 채운다.
struct RtInstance
{
	uint vertexBuffer;   // 바인드리스 힙 번호 — 정점(ByteAddressBuffer, 60바이트 간격: 위치 0 · 법선 12 · 색 24 · UV 36 · 접선 44)
	uint indexBuffer;    // 바인드리스 힙 번호 — 인덱스(ByteAddressBuffer, 삼각형마다 uint 3개)
	uint material;       // 재질 번호 (InstanceID 와 같은 값)
	uint _pad;
};

static const uint  kMaxTrackedObjects = 4;     // 한 번만 곱하려고 기억해 두는 투과 물체 수
static const float kOpaqueCutoff      = 0.01;  // 남는 빛이 이보다 작으면 불투명처럼 끝낸다

// 레이 시작점을 법선 쪽으로 띄우는 거리(m).
//  깊이(D32, near 0.1 m)에서 되살린 위치는 오차가 거리² 에 비례한다(1 km 에서 약 0.6 m) — 그만큼 더 띄운다.
float RayOffsetBias(float viewDistance)
{
	return 0.02 + viewDistance * viewDistance * 1.0e-6;
}

// ── 알파 마스크 (2026-10-28) — 후보 삼각형의 그 자리가 «없는 것» 인가 ──
//  glTF alphaMode MASK: baseColor 알파(텍스처 a × baseColor.a) < alphaCutoff 면 그 자리는 완전 투명 = 없다.
//  alphaCutoff 가 0 인 재질(불투명 · 투과)은 텍스처를 읽지 않고 «있음» 으로 본다.
//  마스크 재질은 TLAS 인스턴스가 FORCE_NON_OPAQUE 라 RayQuery 후보 · any-hit 로 온다(GRenderer AddInstance).
//  UV 는 후보의 무게중심으로 정점 세 개를 보간한다(RtSurface.hlsli FetchHitSurface 와 같은 정점 배치 60바이트 · UV 36).
//  밉맵이 없어 0 단계를 읽는다(재질 텍스처 MipLevels = 1).
bool AlphaMaskedOut(RtInstance inst, RtMaterial mat, uint primitive, float2 bary, SamplerState textureSampler)
{
	if (mat.alphaCutoff <= 0.0)
		return false;
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(inst.vertexBuffer)];
	ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(inst.indexBuffer)];
	const uint3  tri = ib.Load3(primitive * 12);
	const float3 w   = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
	float2 uv = float2(0.0, 0.0);
	[unroll]
	for (uint k = 0; k < 3; ++k)
		uv += asfloat(vb.Load2(tri[k] * 60 + 36)) * w[k];
	Texture2D baseTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.x)];
	return baseTex.SampleLevel(textureSampler, uv, 0).a * mat.baseColor.a < mat.alphaCutoff;
}

// RayQuery 후보가 «있는 것» 인가 — 알파 마스크로 빈 곳이면 거짓(지나간다), 그 밖의 비불투명 후보는 참.
//  AO · GI · 반투명 패스의 후보 루프가 쓴다: 참이면 CommitNonOpaqueTriangleHit.
//  투과 물체(얼음)도 참이다 — 그 패스들은 전에 FORCE_OPAQUE 로 쏴서 얼음을 불투명으로 봤고, 그 동작을 그대로 둔다.
bool CandidateIsSolid(StructuredBuffer<RtInstance> instances, StructuredBuffer<RtMaterial> materials,
	uint instanceIndex, uint primitive, float2 bary, SamplerState textureSampler)
{
	const RtInstance inst = instances[instanceIndex];
	return !AlphaMaskedOut(inst, materials[inst.material], primitive, bary, textureSampler);
}

// 태양 쪽 가시도(RGB). 0 = 완전히 가림, 1 = 다 보임, 그 사이 = 투과 물체를 지나 남은 빛.
//  · 불투명 물체: BLAS 가 OPAQUE 라 시스템이 바로 확정 → ACCEPT_FIRST_HIT_AND_END_SEARCH 로 끝 → 0
//  · 투과 물체: 인스턴스가 FORCE_NON_OPAQUE 라 후보로 온다 → 확정하지 않고 재질의 남는 빛을 곱한다.
//    물체 하나는 한 번만 곱한다(들어감 · 나옴 두 면 + DXR 의 any-hit 중복 허용 때문에 인스턴스 순번으로 거른다).
//    곱셈이라 후보 순서와 상관없다. 남는 빛이 1 % 밑이면 불투명처럼 확정하고 끝낸다.
//  · 알파 마스크 물체(2026-10-28): 후보로 온다 → 빈 곳이면 지나가고, 있는 곳이면 불투명처럼 확정한다.
float3 TraceSunVisibility(RaytracingAccelerationStructure scene, StructuredBuffer<RtMaterial> materials,
	StructuredBuffer<RtInstance> instances, SamplerState textureSampler,
	float3 origin, float3 toSun, float maxDistance)
{
	RayDesc ray;
	ray.Origin    = origin;
	ray.Direction = toSun;
	ray.TMin      = 0.0;
	ray.TMax      = maxDistance;

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

	float3 transmittance = float3(1.0, 1.0, 1.0);
	uint   counted[kMaxTrackedObjects];
	uint   countedCount = 0;

	while (q.Proceed())
	{
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;

		const uint instance = q.CandidateInstanceIndex();
		const RtMaterial candidateMat = materials[q.CandidateInstanceID()];
		// 알파 마스크 (2026-10-28) — 잎 · 풀 사이 빈 곳은 없는 것처럼 지나간다
		if (AlphaMaskedOut(instances[instance], candidateMat, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), textureSampler))
			continue;
		// 마스크의 «있는» 곳(투과 아님)은 불투명 — 확정하고 끝낸다(ACCEPT_FIRST_HIT_AND_END_SEARCH)
		if (candidateMat.transmissive == 0)
		{
			q.CommitNonOpaqueTriangleHit();
			continue;
		}

		bool already = false;
		for (uint k = 0; k < countedCount; ++k)
			already = already || (counted[k] == instance);
		if (already)
			continue;
		if (countedCount < kMaxTrackedObjects)
			counted[countedCount++] = instance;

		transmittance *= candidateMat.transmittance;   // InstanceID = 재질 번호
		if (max(transmittance.r, max(transmittance.g, transmittance.b)) < kOpaqueCutoff)
			q.CommitNonOpaqueTriangleHit();
	}
	return (q.CommittedStatus() == COMMITTED_NOTHING) ? transmittance : float3(0.0, 0.0, 0.0);
}

// ============================================================
//  무작위 수 · GGX 방향 샘플링 (2026-10-27 렌더 5단계 — [4b] B 방식)
// ============================================================

static const float kTwoPi = 6.28318531;

// PCG 해시 — 정수 하나를 잘 섞인 정수로. (Jarzynski · Olano, «Hash Functions for GPU Rendering», JCGT 2020)
uint PcgHash(uint v)
{
	const uint state = v * 747796405u + 2891336453u;
	const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// 픽셀 · 프레임마다 다른 [0,1) 균등 난수 두 개. 프레임이 바뀌면 같은 픽셀도 다른 값 → 시간 누적이 평균을 낸다.
float2 Random2(uint2 pixel, uint frame)
{
	const uint h1 = PcgHash(pixel.x + PcgHash(pixel.y + PcgHash(frame)));
	const uint h2 = PcgHash(h1);
	return float2(h1 >> 8, h2 >> 8) * (1.0 / 16777216.0);   // 위 24비트 → float 가 정확히 담는 범위
}

// 법선 n 에 수직인 두 축(접선 공간). (Duff 외, «Building an Orthonormal Basis, Revisited», JCGT 2017)
void BuildOrthonormalBasis(float3 n, out float3 b1, out float3 b2)
{
	const float s = n.z >= 0.0 ? 1.0 : -1.0;
	const float a = -1.0 / (s + n.z);
	const float b = n.x * n.y * a;
	b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
	b2 = float3(b, s + n.y * n.y * a, -n.y);
}

// GGX «보이는 법선» 분포에서 미세면 법선 하나를 뽑는다 — 접선 공간(z = 표면 법선).
//  (Heitz, «Sampling the GGX Distribution of Visible Normals», JCGT 2018, Listing 1 — 등방성 alpha 로 줄임)
//  Ve = 접선 공간 시선(표면 → 눈), alpha = 거칠기² (Shading.hlsli SpecularGGX 와 같은 규약), u = 균등 난수 2개.
//  눈에 보이는 미세면만 뽑으므로 일반 GGX 샘플링보다 버려지는 방향이 적다.
float3 SampleGgxVndf(float3 Ve, float alpha, float2 u)
{
	// 시선을 «반구» 배치로 늘린다
	const float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
	// 정규 직교 축 (외적이 0 인 경우 따로)
	const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	float3 T1 = float3(1.0, 0.0, 0.0);
	if (lensq > 0.0)
		T1 = float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq);
	const float3 T2 = cross(Vh, T1);
	// 투영 면적 위의 점
	const float r   = sqrt(u.x);
	const float phi = kTwoPi * u.y;
	const float t1  = r * cos(phi);
	float       t2  = r * sin(phi);
	const float s   = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
	// 반구로 되돌리고, 늘렸던 배치를 원래 타원체로
	const float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
	return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

#endif
