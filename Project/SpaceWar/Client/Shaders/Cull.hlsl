// ============================================================
//  Cull.hlsl — GPU 인스턴스 컬링 (2026-10-11)
//    「멀티스레딩 분류 명세서」 §2 원칙 5: 컬링은 처음부터 GPU 로.
//    「렌더러 수정방향」 3.2: 인스턴스는 구조화 버퍼, 그리기에는 인덱스 하나만.
//
//  스레드 하나 = 컬링 항목 하나(인스턴스 번호 + 묶음 번호).
//  보이면 그 묶음의 간접 인자(InstanceCount)를 하나 올리고, 받은 자리에
//  인스턴스 번호를 적는다. 묶음 b 의 목록은 gVisible[first(b) .. first(b) + 개수).
//  Forward.hlsl 의 VSMain 이 gVisible[gInstanceBase + SV_InstanceID] 로 읽는다.
//
//  판정은 예전 CPU 판정(GRenderer.cpp OutsideFrustum, client2 2f15728)과 같다:
//  메시 로컬 AABB 를 월드로 옮긴 외곽 AABB 가 평면 하나라도 완전히 밖이면 버린다.
// ============================================================

cbuffer CullCB : register(b0)        // GRenderer.cpp 의 CullConstants 와 같은 순서
{
	float4 gPlanes[6];                 // xyz = 법선, w = d (정규화). 안쪽이 양수
	uint   gEntryCount;
	uint   gCullEnabled;               // 0 이면 전부 보이는 것으로 둔다
	// 2026-11-20 메시 LOD (render/tlas-select 시험) — 물체 경계 구 반지름 r · 눈까지 거리 d 에서 d > r × gLodK? 이면 다음 단계.
	//  (화면 지름 px 문턱을 거리로 바꾼 값, GRenderer.cpp Impl::FillLodConstants). gLodK0 ≤ 0 이면 LOD 를 고르지 않는다(늘 LOD0)
	float  gLodK0;
	float  gLodK1;
	// 2026-11-17 인스턴스 지평선 판정 — ClusterCull.hlsl 과 같은 «가리는 공». 반지름 0 = 판정 안 함
	float3 gEye;           float gOccluderRadius;
	float3 gPlanetCenter;  float gLodK2;
};

struct InstanceGpu                   // Forward.hlsl 과 같은 128바이트
{
	row_major float4x4 world;
	row_major float4x4 prevWorld;
};

struct BatchBounds                   // GRenderer.cpp 의 Impl::BatchBoundsGpu
{
	float3 center;  uint first;        // 메시 로컬 AABB 중심 · 이 묶음 목록의 시작 번호
	float3 extents; uint lodSpan;      // 2026-11-20 — LOD0 묶음이면 단계 수(LOD0 포함, 2 이상이면 바로 뒤 묶음들이 LOD1 ~), 아니면 0
	//  ★ LOD0 묶음 바로 뒤(LOD1) 칸의 center · extents 는 그 묶음의 컬링에 쓰이지 않고 «단계 판정용 물체 전체 경계» 다.
};

StructuredBuffer<InstanceGpu> gInstances : register(t0);
StructuredBuffer<uint2>       gEntries   : register(t1);   // x = 인스턴스 번호, y = 묶음 번호
StructuredBuffer<BatchBounds> gBounds    : register(t2);
RWByteAddressBuffer           gArgs      : register(u0);   // D3D12_DRAW_INDEXED_ARGUMENTS × 묶음 수
RWStructuredBuffer<uint>      gVisible   : register(u1);

static const uint kArgsStride = 20;          // sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
static const uint kInstanceCountOffset = 4;  // IndexCountPerInstance 다음

bool Visible(float4x4 world, BatchBounds bounds)
{
	// 행벡터 규약: 월드 점 = c.x·행0 + c.y·행1 + c.z·행2 + 행3
	const float3 center = mul(float4(bounds.center, 1.0), world).xyz;
	// 비균일 배율·회전·반전을 모두 포함하는 월드 AABB 의 외곽 반경
	const float3 extents =
		abs(bounds.extents.x * world[0].xyz) +
		abs(bounds.extents.y * world[1].xyz) +
		abs(bounds.extents.z * world[2].xyz);
	// 부동소수 오차로 화면 경계의 메시가 사라지지 않게 여유를 둔다(CPU 판정과 같은 식).
	const float margin = 1.0e-5 * (1.0 + abs(center.x) + abs(center.y) + abs(center.z) +
		extents.x + extents.y + extents.z);

	[unroll]
	for (uint i = 0; i < 6; ++i)
	{
		const float distance = dot(gPlanes[i].xyz, center) + gPlanes[i].w;
		const float radius = dot(abs(gPlanes[i].xyz), extents);
		if (distance + radius < -margin) return false;
	}

	// 지평선 — 외곽 상자를 감싸는 구(c, |extents|)가 가리는 공 뒤에 통째로 숨었으면 버린다(ClusterCull.hlsl BehindHorizon 과 같은 식).
	if (gOccluderRadius > 0.0)
	{
		const float3 toPlanet = gPlanetCenter - gEye;
		const float  d2 = dot(toPlanet, toPlanet);
		const float  R  = gOccluderRadius;
		const float  r  = length(extents);
		const float3 toObject = center - gEye;
		const float  w = length(toObject);
		if (d2 > R * R && w > r)
		{
			const float tangent = sqrt(d2 - R * R);       // 눈 → 행성 실루엣 접점 거리
			if (w - r > tangent)                          // 접점보다 가까운 부분이 없다
			{
				const float d = sqrt(d2);
				const float alpha = asin(saturate(R / d));
				const float beta  = acos(clamp(dot(toObject, toPlanet) / (w * d), -1.0, 1.0));
				const float gamma = asin(saturate(r / w));
				if (beta + gamma < alpha) return false;
			}
		}
	}
	return true;
}

[numthreads(64, 1, 1)]               // GRenderer.cpp 의 Impl::CullGroupSize
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gEntryCount) return;

	const uint2 entry = gEntries[id.x];
	const BatchBounds bounds = gBounds[entry.y];
	const float4x4 world = gInstances[entry.x].world;
	if (gCullEnabled != 0 && !Visible(world, bounds)) return;

	// 2026-11-20 메시 LOD — 물체 전체 경계 구(형제 LOD1 칸)의 화면 지름으로 단계를 고르고 «LOD0 묶음 + 단계» 에 넣는다.
	//  한 물체의 여러 메시(껍질 · 잎)가 같은 경계를 써서 같은 단계를 고른다. 노션 「LOD 적용 준비」 8.6 SelectLod.
	uint batch = entry.y;
	uint first = bounds.first;
	if (bounds.lodSpan > 1 && gLodK0 > 0.0)
	{
		const BatchBounds whole = gBounds[entry.y + 1];
		const float3 center = mul(float4(whole.center, 1.0), world).xyz;
		const float3 extents =
			abs(whole.extents.x * world[0].xyz) +
			abs(whole.extents.y * world[1].xyz) +
			abs(whole.extents.z * world[2].xyz);
		const float r = length(extents);
		const float d = length(center - gEye);
		uint lod = 0;
		if (d > r * gLodK0) lod = 1;
		if (d > r * gLodK1) lod = 2;
		if (d > r * gLodK2) lod = 3;
		lod = min(lod, bounds.lodSpan - 1);
		batch += lod;
		first = gBounds[batch].first;
	}

	uint slot;
	gArgs.InterlockedAdd(batch * kArgsStride + kInstanceCountOffset, 1, slot);
	gVisible[first + slot] = entry.x;
}
