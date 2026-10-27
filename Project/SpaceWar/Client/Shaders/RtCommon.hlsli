// ============================================================
//  RtCommon.hlsli — RT 단계 공용 (2026-10-27 렌더 4단계)
//
//  [4a] RtShadow.hlsl (그림자, 컴퓨트 RayQuery) 와 [4b] RtReflection.hlsl (반사, DispatchRays) 가 같이 쓴다.
//    · RtMaterial  : 재질 표 한 칸 — GRenderer.cpp 의 RtMaterialGpu 와 같은 배치(96바이트)
//    · RtInstance  : TLAS 인스턴스 표 한 칸 — GRenderer.cpp 의 RtInstanceGpu 와 같은 배치(16바이트)
//    · TraceSunVisibility : 태양 가시도(RGB). 2단계 RtShadow.hlsl 의 루프를 옮겨 왔다 —
//                           반사 히트 지점의 그림자도 같은 규칙(투과 물체는 한 번씩 곱함)으로 구하려고.
// ============================================================
#ifndef RT_COMMON_HLSLI
#define RT_COMMON_HLSLI

// 재질 표 — 재질 번호(TLAS InstanceID)로 찾는다.
//  텍스처 번호는 SM 6.6 바인드리스 힙(ResourceDescriptorHeap) 번호다. 텍스처가 없는 슬롯에는 렌더러가 흰색 · 평면 노멀 폴백 번호를 넣는다.
struct RtMaterial
{
	float4 baseColor;
	float3 emissive;          float roughness;
	float  metallic;          uint  hasNormalMap;     uint2 _pad0;
	uint4  textures;          // x = BaseColor  y = Normal  z = Roughness  w = Metallic
	uint   emissiveTexture;   uint3 _pad1;
	float3 transmittance;     uint  transmissive;     // 그림자 레이가 이 물체 하나를 지나고 남는 빛 (2단계)
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

// 태양 쪽 가시도(RGB). 0 = 완전히 가림, 1 = 다 보임, 그 사이 = 투과 물체를 지나 남은 빛.
//  · 불투명 물체: BLAS 가 OPAQUE 라 시스템이 바로 확정 → ACCEPT_FIRST_HIT_AND_END_SEARCH 로 끝 → 0
//  · 투과 물체: 인스턴스가 FORCE_NON_OPAQUE 라 후보로 온다 → 확정하지 않고 재질의 남는 빛을 곱한다.
//    물체 하나는 한 번만 곱한다(들어감 · 나옴 두 면 + DXR 의 any-hit 중복 허용 때문에 인스턴스 순번으로 거른다).
//    곱셈이라 후보 순서와 상관없다. 남는 빛이 1 % 밑이면 불투명처럼 확정하고 끝낸다.
float3 TraceSunVisibility(RaytracingAccelerationStructure scene, StructuredBuffer<RtMaterial> materials,
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
		bool already = false;
		for (uint k = 0; k < countedCount; ++k)
			already = already || (counted[k] == instance);
		if (already)
			continue;
		if (countedCount < kMaxTrackedObjects)
			counted[countedCount++] = instance;

		transmittance *= materials[q.CandidateInstanceID()].transmittance;   // InstanceID = 재질 번호
		if (max(transmittance.r, max(transmittance.g, transmittance.b)) < kOpaqueCutoff)
			q.CommitNonOpaqueTriangleHit();
	}
	return (q.CommittedStatus() == COMMITTED_NOTHING) ? transmittance : float3(0.0, 0.0, 0.0);
}

#endif
