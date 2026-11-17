// ============================================================
//  ParticleRt.hlsli — 파티클 가속 구조 조회 (2026-11-17 P5, 사용자 «반사와 GI AO 단계 진행해줘»)
//
//  파티클은 본 TLAS 에 없다(명세 7절 Particle[]). GRenderer 가 매 프레임 파티클마다 AABB 하나로 BLAS 를 만들고,
//  인스턴스 하나짜리 «파티클 TLAS» 를 비동기 컴퓨트 큐에서 본 TLAS 뒤에 만든다. 레이를 쏘는 패스는 본 TLAS 를 쏜 뒤
//  이 TLAS 를 한 번 더 조회한다(BF V 방식 — 불투명 TLAS → 그 거리까지 파티클 TLAS → 섞기).
//    · ParticleTransmittance — 남는 빛(0 ~ 1). [4a] 그림자 · [2] AO
//    · TraceParticles        — 사이에 있는 파티클의 빛 + 남는 빛. [4c] GI 프로브 · [4b] 반사
//  ★ 맞음 판정은 셰이더가 한다(AABB = 절차적 기하 → RayQuery 후보 CANDIDATE_PROCEDURAL_PRIMITIVE).
//    파티클 = 지름이 size 인 구 — 사각형과 달리 레이 방향과 상관없이 맞는다(BF V 의 «빌보드가 옆에서 보이는» 문제가 없다).
//  ★ 확정(Commit)하지 않고 후보를 다 돌며 곱하고 더한다 — 순서와 상관없는 식이다(BF V 의 any-hit WBOIT 와 같은 생각).
//  ★ 바인드리스 고정 칸 두 개(GRenderer Impl::BindlessParticleTlas · BindlessParticleSrv 와 같은 번호).
//    파티클 RT 를 못 쓰는 장치 · 실패면 렌더러가 두 칸에 빈 뷰를 넣는다 → 아무것도 안 맞는다.
//  ★ 레이 방향 dir 은 단위 벡터여야 한다.
// ============================================================
#ifndef PARTICLE_RT_HLSLI
#define PARTICLE_RT_HLSLI

#include "ParticleCommon.hlsli"

static const uint  kBindlessParticleTlas = 83;
static const uint  kBindlessParticleSrv  = 84;
// 래스터의 둥근 모양 (1 − r²)² 를 원판 전체에서 평균하면 1/3 — 레이 하나가 받는 평균 불투명도로 쓴다.
static const float kParticleRtCoverage = 0.33;

// 구(지름 = p.size)와 레이의 교점 — tMin ~ tMax 안의 가장 가까운 t. 구 안에서 출발하면 나가는 점.
bool IntersectParticle(ParticleSprite p, float3 origin, float3 dir, float tMin, float tMax, out float t)
{
	const float3 oc = origin - p.position;
	const float  r  = 0.5 * p.size;
	const float  b  = dot(oc, dir);
	const float  c  = dot(oc, oc) - r * r;
	const float  h  = b * b - c;
	t = 0.0;
	if (h < 0.0)
		return false;
	const float s = sqrt(h);
	t = -b - s;
	if (t < tMin)
		t = -b + s;
	return t >= tMin && t <= tMax;
}

float ParticleRayAlpha(ParticleSprite p)
{
	return saturate(p.color.a * kParticleRtCoverage);
}

// 남는 빛 — 알파 묶음(먼지 · 연기 · 피)만 가린다. 섬광 · 불꽃(가산)은 빛을 거의 가리지 않아 뺀다.
float ParticleTransmittance(float3 origin, float3 dir, float tMax)
{
	RaytracingAccelerationStructure scene     = ResourceDescriptorHeap[kBindlessParticleTlas];
	StructuredBuffer<ParticleSprite> particles = ResourceDescriptorHeap[kBindlessParticleSrv];
	RayDesc ray;
	ray.Origin    = origin;
	ray.Direction = dir;
	ray.TMin      = 0.0;
	ray.TMax      = tMax;
	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

	float transmittance = 1.0;
	while (q.Proceed())
	{
		if (q.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
			continue;
		const ParticleSprite p = particles[q.CandidatePrimitiveIndex()];
		if ((p.flags & kParticleAdditive) != 0)
			continue;
		float t;
		if (!IntersectParticle(p, origin, dir, 0.0, tMax, t))
			continue;
		transmittance *= 1.0 - ParticleRayAlpha(p);
		if (transmittance < 0.01)
		{
			transmittance = 0.0;
			q.Abort();
		}
	}
	return transmittance;
}

struct ParticleRayResult
{
	float3 radiance;        // 레이에 더할 파티클 빛
	float  transmittance;   // 뒤(맞은 표면 · 하늘)에서 오는 빛이 남는 몫
};

// 사이의 파티클 — 결과 빛 = radiance + transmittance × 뒤의 빛.
//  알파 묶음: 불투명도로 가중 평균한 색 × (1 − 남는 빛) — 순서 없는 섞기(WBOIT 와 같은 생각). 빛을 받는 것은 litScale 을 곱한다.
//  가산 묶음: 색 × 불투명도를 그대로 더한다(가리지 않는다).
//  litScale = 그 패스가 아는 해 · 하늘 빛(부르는 쪽이 정한다 — 그림자는 따지지 않는다).
ParticleRayResult TraceParticles(float3 origin, float3 dir, float tMax, float3 litScale)
{
	RaytracingAccelerationStructure scene     = ResourceDescriptorHeap[kBindlessParticleTlas];
	StructuredBuffer<ParticleSprite> particles = ResourceDescriptorHeap[kBindlessParticleSrv];
	RayDesc ray;
	ray.Origin    = origin;
	ray.Direction = dir;
	ray.TMin      = 0.0;
	ray.TMax      = tMax;
	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

	float3 emissive      = float3(0.0, 0.0, 0.0);
	float3 weightedColor = float3(0.0, 0.0, 0.0);
	float  weightSum     = 0.0;
	float  transmittance = 1.0;
	while (q.Proceed())
	{
		if (q.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE)
			continue;
		const ParticleSprite p = particles[q.CandidatePrimitiveIndex()];
		float t;
		if (!IntersectParticle(p, origin, dir, 0.0, tMax, t))
			continue;
		const float a = ParticleRayAlpha(p);
		if ((p.flags & kParticleAdditive) != 0)
		{
			emissive += p.color.rgb * a;
			continue;
		}
		float3 c = p.color.rgb;
		if ((p.flags & kParticleLit) != 0)
			c *= litScale;
		weightedColor += c * a;
		weightSum     += a;
		transmittance *= 1.0 - a;
	}

	ParticleRayResult r;
	r.transmittance = transmittance;
	r.radiance = emissive;
	if (weightSum > 0.0)
		r.radiance += weightedColor / weightSum * (1.0 - transmittance);
	return r;
}

#endif
