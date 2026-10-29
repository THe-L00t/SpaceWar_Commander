// ============================================================
//  ProbeGI.hlsl — [4c] GI 프로브 갱신 (컴퓨트 cs_6_6 · 인라인 RayQuery · 바인드리스, 2026-10-27 렌더 7단계)
//
//  설계 문서 6.4 의 세 단계. 프레임마다 전체 프로브의 1/4 (gProbesThisFrame 개)를 돌아가며 갱신한다.
//    CSProbeTrace           그룹 하나 = 프로브 하나, 스레드 하나 = 레이 하나(64개).
//                           맞은 점 휘도 = 알베도 × (태양 × 그림자 레이 + 지난 프로브 조도) + 발광   → 레이 데이터(rgb, a = 거리)
//                           뒷면을 맞으면 a = −0.2 × 거리 (프로브가 벽 · 땅 속이라는 표시 — RTXGI 와 같은 규칙)
//                           아무것도 안 맞으면 [6] 하늘 휘도
//    CSProbeBlendDistance   그룹 = 프로브, 스레드 = 거리 텍셀 16×16. 평균 거리 · 거리² 를 섞는다(가중치 cos⁵⁰)
//    CSProbeBlendIrradiance 그룹 = 프로브, 스레드 = 조도 텍셀 8×8. 코사인 가중 평균 휘도(= 조도/π)를 섞는다.
//                           뒷면 레이가 25 % 이상이면 «비활성»(벽 속) 으로 두고 섞지 않는다. 상태 버퍼도 여기서 쓴다.
//  섞기: 새 값 = lerp(이번 값, 지난 값, 0.97) — 천천히 수렴해 깜빡임을 막는다(설계 문서). 새로 들어온 칸은 0.97 대신 0.
//  레이 방향: 구 위 피보나치 64개를 프레임마다 무작위 회전(고정 방향 줄무늬 방지 — 설계 문서).
//  ★ 프로브는 확산(조도)만 담는다 — 맞은 점의 정반사 · 금속 반사는 넣지 않는다(금속은 확산 0).
//  ★ 투과 재질(얼음)은 GI 에서 불투명으로 본다(설계 문서 「행성별 특징」 3.4 의 4번과 같은 선택).
//    2026-10-28 알파 마스크 — 전에는 RAY_FLAG_FORCE_OPAQUE 였다. 이제 후보 루프에서 마스크로 빈 곳은 지나가고,
//    그 밖의 비불투명 후보(마스크의 있는 곳 · 얼음)는 확정한다(RtCommon.hlsli CandidateIsSolid) → 얼음은 예전과 같다.
//  ★ 2026-10-28 꺼 둔 프로브 — 비활성(벽 · 땅 속) 프로브는 추적 · 두 섞기를 모두 건너뛴다(Majercik 외 2021 의 «Off»).
//    비활성은 원래 샘플에서도 빠지므로 화면은 같고 레이만 줄어든다. 상태는 «비활성» 그대로 남는다.
//    다만 움직이는 물체(캐릭터 등) 때문에 잘못 비활성이 된 프로브가 계속 꺼져 있지 않도록,
//    kProbeOffRecheck 바퀴(바퀴 = 그 겹 프로브를 한 번 다 도는 프레임 수)마다 한 번은 다시 잰다. 칸마다 다른 바퀴에 나눠 잰다.
//  ★ 2026-10-28 두 겹 — GRenderer 가 겹마다(가까운 겹 → 먼 겹) 세 진입점을 한 번씩 돌린다. gLayer 가 이번 겹이고,
//    두 겹 정보(gNearVolume · gFarVolume)를 다 받는다 — 맞은 점의 다중 반사는 두 겹을 차례로 읽는다(Probes.hlsli SampleProbeLayers).
//    프로브 번호 probe 는 겹 안의 번호, 아틀라스 · 상태 버퍼 번호는 storageOffset + probe (SlotOf).
// ============================================================

#include "Probes.hlsli"
#include "RtSurface.hlsli"
#include "Sky.hlsli"

// GRenderer.cpp 의 GiConstants 와 같은 배치 (400바이트 — 10-28 바퀴 번호 줄 192 → 208, 두 겹 → 304, 지면에 맞춘 축 → 400)
cbuffer GiCB : register(b0)
{
	ProbeVolume gNearVolume;                          // 128 — 가까운 겹
	ProbeVolume gFarVolume;                           // 128 — 먼 겹 (2026-10-28)
	float4  gRayRotation0;  float4 gRayRotation1;  float4 gRayRotation2;   // 이번 프레임 레이 회전(행 3개)
	float3  gSunDir;          uint gFirstProbe;      // 이번 프레임에 갱신하는 첫 번호(겹 안)
	float3  gSunIlluminance;  uint gProbeCount;      // 이번 겹의 프로브 수
	uint    gRayDataUav;      uint gRayDataSrv;      uint gIrradianceUav;   uint gIrradianceSrv;   // ↓ 바인드리스 번호
	uint    gDistanceUav;     uint gDistanceSrv;     uint gStateUav;        uint gStateSrv;
	uint    gSkySrv;          uint gSkyIrradianceSrv; uint gProbesThisFrame; float gMaxDistance;
	uint    gProbeCycle;      uint gLayer;           uint2 _pad0;   // 이번 겹을 다 돈 횟수 · 이번 겹(0 가까운 · 1 먼)
};

// 이번 겹의 격자 — 진입점마다 맨 처음 SelectLayer 로 채운다(스레드마다 같은 값)
static ProbeVolume gVolume;

void SelectLayer()
{
	if (gLayer == 0)
		gVolume = gNearVolume;
	else
		gVolume = gFarVolume;
}

// 겹 안의 번호 → 아틀라스 · 상태 버퍼 번호
uint SlotOf(uint probe)
{
	return gVolume.storageOffset + probe;
}

RaytracingAccelerationStructure gScene       : register(t0);
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);
StructuredBuffer<RtInstance>    gRtInstances : register(t2);
SamplerState gWrapSampler  : register(s0);   // 재질 텍스처
SamplerState gSkySampler   : register(s1);   // 등장방형 하늘
SamplerState gClampSampler : register(s2);   // 프로브 아틀라스

static const float kGoldenAngle = 2.39996323;   // π(3 − √5)

// 레이 번호 → 방향 (구 위 피보나치 + 이번 프레임 회전). 추적 · 섞기가 같은 방향을 쓴다.
float3 ProbeRayDirection(uint ray)
{
	const float y   = 1.0 - (2.0 * ray + 1.0) / kProbeRays;
	const float r   = sqrt(saturate(1.0 - y * y));
	const float phi = ray * kGoldenAngle;
	const float3 d  = float3(r * cos(phi), y, r * sin(phi));
	return normalize(float3(dot(gRayRotation0.xyz, d), dot(gRayRotation1.xyz, d), dot(gRayRotation2.xyz, d)));
}

// 이 그룹이 맡은 저장 번호
uint ProbeOfGroup(uint group)
{
	return (gFirstProbe + group) % gProbeCount;
}

// ── 꺼 둔 프로브 (2026-10-28) ──
//  이 칸 · 이 세대에서 이미 «비활성» 으로 잰 프로브는 이번 바퀴에 건너뛴다. 단 kProbeOffRecheck 바퀴에 한 번은 다시 잰다.
//  세 진입점이 같은 «옛 상태» 를 읽고 같은 답을 낸다 — 상태는 조도 섞기만 쓰고, 건너뛰면 그것도 안 쓴다.
static const uint kProbeOffRecheck = 8;   // 8 바퀴 = 32 프레임(60 fps 에서 약 0.5 초)

bool ProbeOff(int4 state, int3 cell)
{
	const bool inactive = all(state.xyz == cell) && (uint(state.w) >> 2) == gVolume.generation &&
		(uint(state.w) & 3u) == kProbeStateInactive;
	if (!inactive)
		return false;
	// 다시 잴 바퀴를 칸마다 흩는다(한 바퀴에 몰리지 않게) — 공간 해시(Teschner 외 2003 의 세 소수)
	const uint h = (uint(cell.x) * 73856093u) ^ (uint(cell.y) * 19349663u) ^ (uint(cell.z) * 83492791u);
	return (gProbeCycle + h) % kProbeOffRecheck != 0;
}

// ── ① 추적 ──
[numthreads(64, 1, 1)]
void CSProbeTrace(uint ray : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	SelectLayer();
	const uint   probe = ProbeOfGroup(group.x);
	const int3   cell  = ProbeCellFromStorage(probe, gVolume);
	StructuredBuffer<int4> states = ResourceDescriptorHeap[gStateSrv];
	if (ProbeOff(states[SlotOf(probe)], cell))
		return;   // 2026-10-28 꺼 둔 프로브 — 레이 데이터를 쓰지 않는다(두 섞기도 같은 판단으로 건너뛴다)
	const float3 O     = ProbeWorldPosition(cell, gVolume);
	const float3 D     = ProbeRayDirection(ray);

	RayDesc r;
	r.Origin    = O;
	r.Direction = D;
	r.TMin      = 0.0;
	r.TMax      = gMaxDistance;
	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
	while (q.Proceed())   // 비불투명 후보(알파 마스크 · 투과)만 여기로 온다
	{
		if (CandidateIsSolid(gRtInstances, gRtMaterials, q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(),
				q.CandidateTriangleBarycentrics(), gWrapSampler))
			q.CommitNonOpaqueTriangleHit();
	}

	float4 result;
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		const float t = q.CommittedRayT();
		const RtInstance inst = gRtInstances[q.CommittedInstanceIndex()];
		const RtMaterial mat  = gRtMaterials[inst.material];
		if (!q.CommittedTriangleFrontFace() && mat.doubleSided == 0)
		{
			result = float4(0.0, 0.0, 0.0, -0.2 * t);   // 뒷면 — 프로브가 벽 · 땅 속
		}
		else
		{
			const HitSurface s = FetchHitSurface(inst, mat, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(),
				q.CommittedWorldToObject3x4(), gWrapSampler);
			float3 N = s.N;
			if (dot(N, D) > 0.0) N = -N;   // 레이 쪽을 보게
			const float3 P = O + D * t;

			// 태양 — 확산만 (람베르트 알베도/π)
			const float3 L = -gSunDir;
			const float  NdotL = dot(N, L);
			float3 sun = float3(0.0, 0.0, 0.0);
			if (NdotL > 0.0)
				sun = gSunIlluminance * (NdotL / kPi) *
					TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gWrapSampler, P + N * RayOffsetBias(t), L, gMaxDistance);

			// 다중 반사 — 맞은 점의 간접 조도 = 지난 프레임까지의 프로브 (없으면 하늘 조도)
			Texture2D<float4>      irradiance = ResourceDescriptorHeap[gIrradianceSrv];
			Texture2D<float2>      distance   = ResourceDescriptorHeap[gDistanceSrv];
			float weightSum;
			uint  layerUsed;
			// 2026-10-28 두 겹 — 가까운 겹 → 먼 겹 순서(먼 겹 프로브의 레이도 가까운 겹 안을 맞으면 그 값을 쓴다)
			float3 indirect = SampleProbeLayers(irradiance, distance, states, gClampSampler, gNearVolume, gFarVolume,
				P, N, -D, weightSum, layerUsed);
			if (weightSum <= 0.0)
			{
				Texture2D<float4> skyIrradiance = ResourceDescriptorHeap[gSkyIrradianceSrv];
				indirect = SampleSkyIrradiance(skyIrradiance, gSkySampler, N);
			}
			result = float4(s.albedo * (1.0 - s.metallic) * (sun + indirect) + s.emissive, t);
		}
	}
	else
	{
		Texture2D<float4> sky = ResourceDescriptorHeap[gSkySrv];
		result = float4(SampleSkyRadiance(sky, gSkySampler, D), gMaxDistance);
	}

	RWTexture2D<float4> rayData = ResourceDescriptorHeap[gRayDataUav];
	rayData[uint2(ray, group.x)] = result;
}

// 테두리 텍셀 → 복사해 올 안쪽 텍셀 (팔면체 가장자리는 맞은편과 이어진다 — RTXGI 테두리 갱신과 같은 배치)
//  n = 안쪽 크기(6 · 14), 텍셀 좌표 0 ~ n+1.
uint2 BorderSource(uint2 t, uint n)
{
	const bool left = t.x == 0, right = t.x == n + 1, top = t.y == 0, bottom = t.y == n + 1;
	if (left && top)     return uint2(n, n);
	if (right && top)    return uint2(1, n);
	if (left && bottom)  return uint2(n, 1);
	if (right && bottom) return uint2(1, 1);
	if (top)    return uint2(n + 1 - t.x, 1);
	if (bottom) return uint2(n + 1 - t.x, n);
	if (left)   return uint2(1, n + 1 - t.y);
	if (right)  return uint2(n, n + 1 - t.y);
	return t;
}

// 안쪽 텍셀 → 그 텍셀이 맡은 방향
float3 TexelDirection(uint2 source, uint n)
{
	const float2 oct = ((float2(source) - 1.0 + 0.5) / n) * 2.0 - 1.0;
	return DecodeNormalOct(oct);
}

// 이 프로브의 상태가 «이 칸 · 이 세대» 의 데이터인가 — 아니면 새로 들어온 칸(섞지 않고 덮어쓴다)
bool ProbeHasHistory(int4 state, int3 cell)
{
	return all(state.xyz == cell) && (uint(state.w) >> 2) == gVolume.generation && (uint(state.w) & 3u) != kProbeStateNoData;
}

static const float kHysteresis = 0.97;   // 설계 문서 6.4

// ── ② 거리 섞기 ──
[numthreads(16, 16, 1)]
void CSProbeBlendDistance(uint3 tid : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	SelectLayer();
	const uint probe = ProbeOfGroup(group.x);
	const int3 cell  = ProbeCellFromStorage(probe, gVolume);
	const uint slot  = SlotOf(probe);   // 아틀라스 · 상태 번호
	RWStructuredBuffer<int4> states  = ResourceDescriptorHeap[gStateUav];
	if (ProbeOff(states[slot], cell))
		return;   // 2026-10-28 꺼 둔 프로브 — 추적이 레이 데이터를 쓰지 않았다
	Texture2D<float4>        rayData = ResourceDescriptorHeap[gRayDataSrv];
	RWTexture2D<float2>      target  = ResourceDescriptorHeap[gDistanceUav];

	const uint   n = kProbeDistanceTexels - 2;
	const float3 dirT = TexelDirection(BorderSource(tid.xy, n), n);
	const float  maxDistance = gVolume.spacing * 1.7320508 * 1.5;   // 격자 칸 대각선의 1.5 배 (RTXGI)

	float2 sum = float2(0.0, 0.0);
	float  weightSum = 0.0;
	for (uint ray = 0; ray < kProbeRays; ++ray)
	{
		const float w = pow(saturate(dot(dirT, ProbeRayDirection(ray))), 50.0);
		const float d = min(abs(rayData[uint2(ray, group.x)].a), maxDistance);
		sum += w * float2(d, d * d);
		weightSum += w;
	}
	const float2 value = weightSum > 1.0e-6 ? sum / weightSum : float2(maxDistance, maxDistance * maxDistance);

	const uint2 texel = uint2(slot % gVolume.probesPerRow, slot / gVolume.probesPerRow) * kProbeDistanceTexels + tid.xy;
	const float hysteresis = ProbeHasHistory(states[slot], cell) ? kHysteresis : 0.0;
	target[texel] = lerp(value, target[texel], hysteresis);
}

// ── ③ 조도 섞기 + 상태 ──
groupshared uint gsBackfaces;

[numthreads(8, 8, 1)]
void CSProbeBlendIrradiance(uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex, uint3 group : SV_GroupID)
{
	SelectLayer();
	const uint probe = ProbeOfGroup(group.x);
	const int3 cell  = ProbeCellFromStorage(probe, gVolume);
	const uint slot  = SlotOf(probe);   // 아틀라스 · 상태 번호
	RWStructuredBuffer<int4> states  = ResourceDescriptorHeap[gStateUav];
	Texture2D<float4>        rayData = ResourceDescriptorHeap[gRayDataSrv];
	RWTexture2D<float4>      target  = ResourceDescriptorHeap[gIrradianceUav];

	const int4 oldState = states[slot];   // 모두 쓰기 전에 읽는다
	if (ProbeOff(oldState, cell))
		return;   // 2026-10-28 꺼 둔 프로브 — 그룹 전체가 같은 판단(상태 «비활성» 그대로 둔다)
	const bool hasHistory = ProbeHasHistory(oldState, cell);
	if (gi == 0) gsBackfaces = 0;
	GroupMemoryBarrierWithGroupSync();

	const uint   n = kProbeIrradianceTexels - 2;
	const float3 dirT = TexelDirection(BorderSource(tid.xy, n), n);
	float3 sum = float3(0.0, 0.0, 0.0);
	float  weightSum = 0.0;
	uint   backfaces = 0;
	for (uint ray = 0; ray < kProbeRays; ++ray)
	{
		const float4 sample = rayData[uint2(ray, group.x)];
		if (sample.a < 0.0) { ++backfaces; continue; }   // 뒷면은 섞지 않는다
		const float w = saturate(dot(dirT, ProbeRayDirection(ray)));
		sum += w * sample.rgb;
		weightSum += w;
	}
	if (gi == 0) gsBackfaces = backfaces;   // 모든 스레드가 같은 수를 센다 — 하나만 적는다
	GroupMemoryBarrierWithGroupSync();

	const bool inside = gsBackfaces * 4 >= kProbeRays;   // 25 % 이상 뒷면
	if (gi == 0)
		states[slot] = int4(cell, int((gVolume.generation << 2) | (inside ? kProbeStateInactive : kProbeStateActive)));
	if (inside)
		return;   // 벽 · 땅 속 — 섞지 않는다(샘플에서도 빠진다)

	const float3 value = weightSum > 1.0e-6 ? sum / weightSum : float3(0.0, 0.0, 0.0);
	const uint2  texel = uint2(slot % gVolume.probesPerRow, slot / gVolume.probesPerRow) * kProbeIrradianceTexels + tid.xy;
	const float  hysteresis = hasHistory ? kHysteresis : 0.0;
	target[texel] = float4(lerp(value, target[texel].rgb, hysteresis), 1.0);
}
