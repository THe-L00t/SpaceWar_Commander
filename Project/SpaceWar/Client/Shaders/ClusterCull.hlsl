// ============================================================
//  ClusterCull.hlsl — 지형 조각(클러스터) 컬링 (2026-11-17)
//    사용자 «TLAS 인스턴스 선발하듯이 지평선 자르고, 행성 반대편 메시도 제외하고 그릴 방법 없을까»
//    → «클러스터드 컬링 … 코드로 실측해줘. 혹시나 해당 판정하는 오버헤드가 더 크진 않을지 확인해줘».
//
//  Cull.hlsl 은 인스턴스 하나를 통째로 거른다. 예시 행성 지형은 메시 한 덩어리(340만 ~ 400만 삼각형)라
//  프러스텀 안에 조금만 걸려도 행성 전체를 그렸다. 그래서 큰 메시는 GRenderer::CreateMesh 가 인덱스를
//  «조각(삼각형 kClusterTriangles 개)» 순서로 다시 놓고 조각마다 경계 구 · 법선 원뿔을 만든다.
//  이 셰이더는 스레드 하나 = 조각 하나로 판정해, 살아남은 조각만 간접 그리기 명령으로 쓴다.
//
//  판정 순서 — 앞에서 걸리면 뒤는 안 본다(뺀 수는 판정별로 센다):
//   ① 프러스텀: Cull.hlsl 과 같은 식(경계 구가 평면 하나라도 완전히 밖)
//   ② 지평선 · 행성 반대편: «가리는 공»(행성 중심 · 지형의 가장 낮은 반지름) 뒤에 숨은 조각.
//      노션 「LOD 적용 준비」 8.6 BehindHorizon 식 — 오브젝트 고르기(StreamObjects)의 «반대편 반구(내적 < 0)» 를
//      일반화한 것이다(눈이 땅에 있으면 반대편 반구는 늘 지평선 뒤라 여기서 같이 빠진다).
//   ③ 뒷면: 조각의 모든 면이 눈을 등지면 뺀다(법선 원뿔 — Wihlidal, GDC 2016 «Optimizing the Graphics Pipeline with Compute»).
//  래스터 전용 — BLAS · TLAS 는 메시 전체 그대로다(그림자 · 반사 · GI 는 바뀌지 않는다).
// ============================================================

cbuffer ClusterCullCB : register(b0)   // GRenderer.cpp 의 ClusterCullConstants 와 같은 순서 (루트 상수 36개)
{
	float4 gPlanes[6];                   // xyz = 법선, w = d (정규화, 월드). 안쪽이 양수
	float3 gEye;           uint  gClusterCount;
	float3 gPlanetCenter;  float gOccluderRadius;   // 가리는 공(월드). 반지름 0 = 지평선 판정 끔
	uint   gInstance;      uint  gCommandBase;      // 인스턴스 번호 · 이 메시 명령 구간의 시작(명령 단위)
	uint   gCounterSlot;   uint  gFlags;            // 조각 묶음 번호(카운터 · 정점 셰이더 루트 상수) · 판정 켜기 비트
};

static const uint kFlagFrustum = 1u;
static const uint kFlagHorizon = 2u;
static const uint kFlagCone    = 4u;

struct InstanceGpu                     // Forward.hlsl · Cull.hlsl 과 같은 128바이트
{
	row_major float4x4 world;
	row_major float4x4 prevWorld;
};

struct ClusterGpu                      // GRenderer.cpp 의 Impl::ClusterGpu (48 B)
{
	float3 center;     float radius;     // 메시 로컬 경계 구
	float3 coneAxis;   float coneCos;    // 법선 원뿔(바깥쪽 면 법선) — coneCos ≤ 0 이면 원뿔 판정 안 함
	uint   firstIndex; uint  indexCount; // 인덱스 버퍼 안 이 조각의 구간
	uint2  pad;
};

StructuredBuffer<InstanceGpu> gInstances : register(t0);
StructuredBuffer<ClusterGpu>  gClusters  : register(t1);
RWByteAddressBuffer           gCommands  : register(u0);   // 명령 24 B = 루트 상수 1개 + D3D12_DRAW_INDEXED_ARGUMENTS
RWByteAddressBuffer           gCounters  : register(u1);   // 조각 묶음마다 32 B (아래 오프셋)

static const uint kCommandStride   = 24;
static const uint kCounterStride   = 32;
static const uint kCountCommands   = 0;    // 그릴 조각 수 = ExecuteIndirect 의 개수 버퍼
static const uint kCountTriangles  = 4;    // 그릴 삼각형 수(통계)
static const uint kCountFrustum    = 8;    // ① 로 뺀 조각 수
static const uint kCountHorizon    = 12;   // ② 로 뺀 조각 수
static const uint kCountBackFacing = 16;   // ③ 으로 뺀 조각 수

// ② 가리는 공(중심 C, 반지름 R) 뒤에 경계 구(c, r)가 통째로 숨었나.
//  눈에서 본 공의 반각 α = asin(R / d) 원뿔 안으로 가는 레이는 공에 들어가고, 들어가는 점은 접점 거리 √(d² − R²) 보다 가깝다.
//  경계 구의 모든 점이 «원뿔 안(β + γ < α)» 이고 «접점보다 멀면(거리 − r > √(d² − R²))» 공을 지나서야 닿는다 = 숨었다.
//  R 은 지형의 가장 낮은 반지름(CPU 가 조각 경계 구로 아래쪽으로 어림)이라 공은 늘 땅 속에 있다 → 숨긴 것은 정말 안 보인다.
bool BehindHorizon(float3 c, float r)
{
	const float3 toPlanet = gPlanetCenter - gEye;
	const float  d2 = dot(toPlanet, toPlanet);
	const float  R  = gOccluderRadius;
	if (d2 <= R * R) return false;                    // 눈이 공 안 — 판정하지 않는다
	const float3 toCluster = c - gEye;
	const float  w = length(toCluster);
	if (w <= r) return false;                         // 눈이 경계 구 안
	const float  tangent = sqrt(d2 - R * R);          // 눈 → 행성 실루엣 접점 거리
	if (w - r <= tangent) return false;               // 접점보다 가까운 부분이 있다
	const float  d = sqrt(d2);
	const float  alpha = asin(saturate(R / d));
	const float  beta  = acos(clamp(dot(toCluster, toPlanet) / (w * d), -1.0, 1.0));
	const float  gamma = asin(saturate(r / w));
	return beta + gamma < alpha;
}

// ③ 조각의 모든 면이 뒷면인가. 면 법선 n 이 축 a 에서 θ(cos = coneCos) 안, 면의 점 p 가 경계 구(c, r) 안이면
//  dot(n, p − 눈) ≥ D·cos(φ + θ) − r  (D = |c − 눈|, φ = a 와 (c − 눈) 사이 각).  이것이 0 보다 크면 모든 면이 눈을 등진다.
bool BackFacing(float3 c, float r, float3 axis, float coneCos)
{
	const float3 v = c - gEye;
	const float  D = length(v);
	if (D <= r) return false;
	const float cosPhi   = dot(axis, v) / D;
	const float sinPhi   = sqrt(saturate(1.0 - cosPhi * cosPhi));
	const float sinTheta = sqrt(saturate(1.0 - coneCos * coneCos));
	const float cosSum   = cosPhi * coneCos - sinPhi * sinTheta;   // cos(φ + θ)
	return cosSum > r / D;
}

[numthreads(64, 1, 1)]                 // GRenderer.cpp 의 Impl::ClusterGroupSize
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gClusterCount) return;

	const ClusterGpu cluster = gClusters[id.x];
	const float4x4 world = gInstances[gInstance].world;
	const float3 center = mul(float4(cluster.center, 1.0), world).xyz;
	const float  scale  = max(length(world[0].xyz), max(length(world[1].xyz), length(world[2].xyz)));
	const float  radius = cluster.radius * scale;
	const uint   counter = gCounterSlot * kCounterStride;

	if ((gFlags & kFlagFrustum) != 0u)
	{
		// 부동소수 오차로 화면 경계의 조각이 사라지지 않게 여유를 둔다(Cull.hlsl 과 같은 식).
		const float margin = 1.0e-5 * (1.0 + abs(center.x) + abs(center.y) + abs(center.z) + radius);
		[unroll]
		for (uint i = 0; i < 6; ++i)
		{
			if (dot(gPlanes[i].xyz, center) + gPlanes[i].w < -radius - margin)
			{
				gCounters.InterlockedAdd(counter + kCountFrustum, 1u);
				return;
			}
		}
	}
	if ((gFlags & kFlagHorizon) != 0u && gOccluderRadius > 0.0 && BehindHorizon(center, radius))
	{
		gCounters.InterlockedAdd(counter + kCountHorizon, 1u);
		return;
	}
	if ((gFlags & kFlagCone) != 0u && cluster.coneCos > 0.0)
	{
		const float3 axis = normalize(mul(cluster.coneAxis, (float3x3)world));
		if (BackFacing(center, radius, axis, cluster.coneCos))
		{
			gCounters.InterlockedAdd(counter + kCountBackFacing, 1u);
			return;
		}
	}

	uint slot;
	gCounters.InterlockedAdd(counter + kCountCommands, 1u, slot);
	gCounters.InterlockedAdd(counter + kCountTriangles, cluster.indexCount / 3u);
	const uint address = (gCommandBase + slot) * kCommandStride;
	gCommands.Store(address, gCounterSlot);   // 루트 상수 → 정점 셰이더 gInstanceBase(조각 인스턴스 목록에서 이 묶음 칸)
	// D3D12_DRAW_INDEXED_ARGUMENTS: IndexCountPerInstance · InstanceCount · StartIndexLocation · BaseVertexLocation · StartInstanceLocation
	gCommands.Store4(address + 4u, uint4(cluster.indexCount, 1u, cluster.firstIndex, 0u));
	gCommands.Store(address + 20u, 0u);
}
