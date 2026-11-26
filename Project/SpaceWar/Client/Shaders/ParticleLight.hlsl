// ============================================================
//  ParticleLight.hlsl — [0b] 파티클 빛 (컴퓨트 cs_6_6 · 바인드리스 · 인라인 RayQuery, 2026-11-25)
//
//  사용자(2026-11-25): «그늘에서 파티클이 튀어도 태양 빛을 받은것 처럼 밝아» → 원인 = [7b] PSParticle 에 해 그림자 · AO 가 없다
//    ([4a] 그림자 마스크 · [2] AO 는 G-buffer 픽셀마다라 G-buffer 에 없는 파티클은 못 받는다).
//  → 사용자 «그렇게 작업해줘»(C안 — 파티클마다 미리 계산 · 비동기 컴퓨트 큐의 TLAS 빌드 뒤):
//    파티클 하나에 스레드 8개. 구 가운데(ParticleRayCenter)에서
//      · 해 쪽 레이 — 0 번 스레드: 본 TLAS(TraceSunVisibility — [4a] 와 같은 투과 · 알파 마스크 규칙)
//                     1 번 스레드: 파티클 TLAS(ParticleTransmittanceSkip — 자기 빼고 다른 연기 · 먼지 · 피가 가리는 몫) — 2026-11-26 뺌(아래 ★★)
//      · AO 레이 8개 — 스레드마다 하나. 카메라 자리의 위(gPlanetUp) 반구의 코사인 분포 고정 방향(프레임마다 같다 → 깜빡이지 않음),
//                     본 TLAS 만(월드 AO). 결과 = 안 가려진 레이 비율.
//    결과 float4(해 가시도 RGB, AO) 를 파티클 번호 자리에 쓴다 → [7b] Particles.hlsl 이 VS 에서 읽어
//    해 몫에 가시도를, 주변광 몫에 AO 를 곱한다(월드 Lighting.hlsl 과 같은 자리 — AO 는 주변광에만).
//  ★ 레이는 구 표면(가운데 + 반지름)부터 본다(TMin = 반지름 = size / 2).
//    피는 몸에 맞은 자리에서 나와 판 가운데가 몬스터 몸속일 수 있다 — 가운데부터 쏘면 늘 가려져 까맣게 된다.
//    (파티클 레이 쪽 규약과 같다: 파티클 = 지름 size 인 구 — ParticleRt.hlsli)
//  ★ 루트 = 컴퓨트 공용(computeRootSig): b0 ParticleLightCB · t0 본 TLAS · t1 재질 표 · t2 인스턴스 표 · s0 선형 반복.
//    파티클 배열 · 파티클 TLAS = 바인드리스 84 · 83(ParticleRt.hlsli), 결과 = 바인드리스 UAV(gOutputUav).
//  ★ 판 하나가 값 하나를 받는다 — 그림자 경계가 큰 판(2 ~ 3 m 연기)을 가로질러도 반쪽만 어두워지지 않는다(2026-11-25 사용자에게 알린 한계).
//  ★ 지금은 EndFrame 이 GPU 를 기다려, 이 패스가 도는 동안 앞 프레임 그래픽 작업은 다 끝나 있다.
//    CPU · GPU 를 겹치게 바꾸면 결과 버퍼는 슬롯 2벌이라 괜찮지만, 재질 텍스처 등 같이 읽는 것을 다시 확인할 것.
//
//  ★★ 2026-11-26 임시 — 태어날 때 한 번 재고 사라질 때까지 고정 · 파티클끼리 해 가림 뺌
//    사용자: «파티클이 그림자가 적용이 되었다면 사라지기 전까지는 주변광 값 고정 하면 안돼? 중간에 빛을 만나 바뀌는거 때문에 깜빡인다며»
//            → «파티클 가림도 없애줘. 나중에 갓 레이 어떻게 할지 고민하면서 새로 수정할거고 임시니까 진행해줘».
//    깜빡임 원인(11-25) = 한 점 · 한 번 · 0/1 을 프레임마다 다시 재서 — 연기가 움직이며 그림자 경계 · 잎 틈 · 다른 파티클을 지날 때 판 전체가 뒤집힌다.
//    · 고정: 결과 버퍼 뒤에 «풀 칸 기록»(칸마다 float4 두 개 = (해 가시도 RGB, AO) · (열쇠, 0, 0, 0))을 둔다(gFirstState 부터).
//      C++ 가 flags 위 비트에 풀 칸 번호 · 그 칸의 열쇠(태어날 때마다 +1)를 싣는다(ParticleCommon.hlsli kParticleSlotShift).
//      칸 기록의 열쇠가 다르면 = 이 패스가 처음 보는 파티클 → 레이를 쏴 재고 적는다. 같으면 적어 둔 값을 그대로 쓴다(레이 없음).
//      → 해 몫(그림자) · AO 가 수명 내내 같다. GI 프로브 주변광은 [7b] 가 픽셀마다 읽는 그대로(공간에서 부드럽게 변해 깜빡임 원인이 아니다).
//    · 파티클끼리 해 가림(파티클 TLAS 해 레이, 예전 1 번 스레드)은 뺐다 — 태어나는 순간 같은 발 형제는 거의 투명(처음 10 % 짙어짐)이라
//      고정하면 의미가 없고, 매 프레임 재면 9 ~ 18 % 씩 흔들린다. 파티클 → 월드 그림자(P5, [4a])는 그대로.
//    · 잃는 것: 그늘 ↔ 햇빛을 오가도 안 바뀐다(연기 표로 계산하면 사는 동안 1 m 안쪽 이동 — 측정 아님).
// ============================================================

#include "RtCommon.hlsli"     // RtMaterial · RtInstance · TraceSunVisibility · CandidateIsSolid · BuildOrthonormalBasis
#include "ParticleRt.hlsli"   // ParticleSprite · ParticleRayCenter · ParticleTransmittanceSkip · kBindlessParticleSrv

// GRenderer.cpp 의 ParticleLightConstants 와 같은 배치 (48바이트)
cbuffer ParticleLightCB : register(b0)
{
	float3 gSunDir;       uint  gCount;          // 해 → 장면 방향(빛 = −gSunDir) · 이번 프레임 파티클 수
	float3 gPlanetUp;     uint  gOutputUav;      // 카메라 자리의 위(AO 반구 축 — PSParticle 이 주변광을 읽는 방향과 같다) · 결과 UAV(바인드리스)
	float  gMaxDistance;  float gAoRadius;       // 해 쪽 레이 길이(m, [4a] 와 같다) · AO 반경(m, [2] 와 같다)
	uint   gFirstOutput;  uint  gFirstState;     // 결과 버퍼에서 이번 프레임 슬롯의 첫 칸 · 2026-11-26 풀 칸 기록의 첫 칸(칸마다 2 칸)
};

RaytracingAccelerationStructure gScene       : register(t0);
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);
StructuredBuffer<RtInstance>    gRtInstances : register(t2);
SamplerState                    gWrapSampler : register(s0);

static const uint kLanes             = 8;   // 파티클 하나에 스레드 8개 = AO 레이 8개
static const uint kParticlesPerGroup = 8;   // 그룹 하나 = 64 스레드 (GRenderer RecordParticleLight 의 Dispatch 와 같다)

// AO 방향 — 반구 위 고정 8점(Hammersley: x = (i + 0.5) / 8, y = 비트 뒤집기). 코사인 분포는 Malley 로 올린다(AO.hlsl CosineDirection 과 같은 식).
static const float2 kAoPattern[kLanes] = {
	float2(0.0625, 0.000), float2(0.1875, 0.500), float2(0.3125, 0.250), float2(0.4375, 0.750),
	float2(0.5625, 0.125), float2(0.6875, 0.625), float2(0.8125, 0.375), float2(0.9375, 0.875) };

groupshared float  gsAo[kParticlesPerGroup * kLanes];
groupshared float3 gsSunWorld[kParticlesPerGroup];
// 2026-11-26 — 파티클끼리 해 가림(gsSunParticles)은 뺐다(위 머리말 ★★)

// 본 TLAS 에서 tMin ~ tMax 안에 «있는 것» 이 맞는가 — AO.hlsl 의 «모름» 레이와 같은 후보 판정(알파 마스크 빈 곳은 지나간다)
bool WorldOccluded(float3 origin, float3 dir, float tMin, float tMax)
{
	RayDesc ray;
	ray.Origin    = origin;
	ray.Direction = dir;
	ray.TMin      = tMin;
	ray.TMax      = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
	while (q.Proceed())
	{
		if (CandidateIsSolid(gRtInstances, gRtMaterials, q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(),
				q.CandidateTriangleBarycentrics(), gWrapSampler))
			q.CommitNonOpaqueTriangleHit();
	}
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(64, 1, 1)]
void CSParticleLight(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
	const uint lane  = groupIndex % kLanes;
	const uint local = groupIndex / kLanes;
	const uint index = groupId.x * kParticlesPerGroup + local;

	RWStructuredBuffer<float4> output = ResourceDescriptorHeap[gOutputUav];

	float  ao       = 1.0;
	float3 sunWorld = float3(1.0, 1.0, 1.0);
	bool   measure  = false;   // 2026-11-26 — 이번에 새로 재나(칸 기록의 열쇠가 다르다 = 처음 보는 파티클)
	uint   stateAt  = 0;       // 풀 칸 기록 자리(빛 · 열쇠 두 칸 중 앞 칸)
	uint   key      = 0;
	float4 kept     = float4(1.0, 1.0, 1.0, 1.0);
	if (index < gCount)
	{
		StructuredBuffer<ParticleSprite> particles = ResourceDescriptorHeap[kBindlessParticleSrv];
		const ParticleSprite p = particles[index];
		if ((p.flags & kParticleLit) != 0)   // 해 · 하늘 빛을 받는 것(먼지 · 연기 · 피)만 — 섬광 · 불꽃은 스스로 빛난다
		{
			// 열쇠 = 그 칸에 태어난 차례 + 30 번 비트(0 = 한 번도 안 잰 칸 — 버퍼는 0 으로 만들어진다).
			//  30 번 비트를 켜 두면 float 로 볼 때 보통 수(2 ~ 4)라, float4 칸에 적고 읽어도 비정규 수처럼 0 으로 눌릴 걱정이 없다.
			key     = ((p.flags >> kParticleLightKeyShift) & kParticleKeyMask) | 0x40000000u;
			stateAt = gFirstState + ((p.flags >> kParticleSlotShift) & kParticleKeyMask) * 2u;
			measure = asuint(output[stateAt + 1u].x) != key;
			if (!measure)
				kept = output[stateAt];   // 태어날 때 잰 값 그대로
		}
		if (measure)
		{
			const float3 center = ParticleRayCenter(p);
			const float  radius = 0.5 * p.size;
			const float3 toSun  = -gSunDir;

			float3 T, B;
			const float3 up = normalize(gPlanetUp);
			BuildOrthonormalBasis(up, T, B);
			const float2 u   = kAoPattern[lane];
			const float  r   = sqrt(u.x);
			const float  phi = kTwoPi * u.y;
			const float3 dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + up * sqrt(saturate(1.0 - u.x)));
			ao = WorldOccluded(center, dir, radius, radius + gAoRadius) ? 0.0 : 1.0;

			if (lane == 0)
				sunWorld = TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gWrapSampler, center + toSun * radius, toSun, gMaxDistance);
		}
	}

	// 그룹 안에서 모은다 — 배리어는 모든 스레드가 지나가야 하므로 분기 밖에 둔다.
	//  칸 기록은 배리어 뒤 0 번 스레드만 쓴다 — 같은 파티클의 다른 스레드는 배리어 앞에서 읽기만 했다(풀 칸은 파티클마다 다르다).
	gsAo[groupIndex] = ao;
	if (lane == 0) gsSunWorld[local] = sunWorld;
	GroupMemoryBarrierWithGroupSync();

	if (lane == 0 && index < gCount)
	{
		float4 result = kept;   // 빛을 안 받는 것(1) · 고정된 값
		if (measure)
		{
			float visible = 0.0;
			[unroll]
			for (uint k = 0; k < kLanes; ++k)
				visible += gsAo[local * kLanes + k];
			result = float4(gsSunWorld[local], visible / float(kLanes));
			output[stateAt]      = result;
			output[stateAt + 1u] = float4(asfloat(key), 0.0, 0.0, 0.0);
		}
		output[gFirstOutput + index] = result;
	}
}
