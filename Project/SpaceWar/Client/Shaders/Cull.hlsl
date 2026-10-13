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
};

struct InstanceGpu                   // Forward.hlsl 과 같은 128바이트
{
	row_major float4x4 world;
	row_major float4x4 prevWorld;
};

struct BatchBounds                   // GRenderer.cpp 의 Impl::BatchBoundsGpu
{
	float3 center;  uint first;        // 메시 로컬 AABB 중심 · 이 묶음 목록의 시작 번호
	float3 extents; uint pad;
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
	return true;
}

[numthreads(64, 1, 1)]               // GRenderer.cpp 의 Impl::CullGroupSize
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gEntryCount) return;

	const uint2 entry = gEntries[id.x];
	const BatchBounds bounds = gBounds[entry.y];
	if (gCullEnabled != 0 && !Visible(gInstances[entry.x].world, bounds)) return;

	uint slot;
	gArgs.InterlockedAdd(entry.y * kArgsStride + kInstanceCountOffset, 1, slot);
	gVisible[bounds.first + slot] = entry.x;
}
