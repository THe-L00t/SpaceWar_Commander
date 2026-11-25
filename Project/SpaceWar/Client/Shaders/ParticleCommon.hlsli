// ============================================================
//  ParticleCommon.hlsli — 파티클 사각형 한 칸 (2026-11-17)
//
//  [7b] Particles.hlsl(래스터)와 ParticleRt.hlsli(레이 — 그림자 · AO · GI · 반사)가 같은 배열을 읽는다.
//  ★ C++ 의 swc::ParticleSprite(InstanceData.h, 96 B) · ParticleFlags 와 같은 배치 · 같은 값이어야 한다.
//  2026-11-25 — 고정 사각형 축 · 기준점 → 가운데 · 레이 몫이 붙어 48 → 96 B
//    (사용자 «맞은 지점 기준으로 정면 측면 둘다 출력», «플립북 파티클 클래스에 해당 값도 넣어서 설정할 수 있게»).
// ============================================================
#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

struct ParticleSprite
{
	float3 position;  float size;    // 월드 위치(m — 사각형의 기준점) · 사각형 한 변(m) — 레이는 지름이 size 인 구로 본다
	float4 color;                    // 선형 RGB · a = 불투명도
	float  rotation;  uint  flags;
	uint   flipbook;  float frame;  // 2026-11-23 — 0 = 텍스처 없음, n = gFlipbooks[n − 1] · 프레임 위치(소수 = 다음 프레임과 섞는 몫)
	// 2026-11-25 — 고정 사각형(kParticleFixedAxes) 축: X 오른쪽 · Y 위 · Z 보는 쪽(월드 단위 벡터).
	//  offset = 기준점 → 사각형 가운데(한 변 단위). rayCoverage = 레이가 받는 불투명도 몫(color.a 에 곱한다).
	float3 axisX;     float offsetX;
	float3 axisY;     float offsetY;
	float3 axisZ;     float rayCoverage;
};
static const uint kParticleAdditive  = 1u;   // 가산(섬광 · 불꽃 — 스스로 빛남)
static const uint kParticleLit       = 2u;   // 해 · 하늘 빛을 받는다(먼지 · 연기 · 피)
static const uint kParticleFixedAxes = 4u;   // 2026-11-25 — 카메라를 보지 않고 axisX · Y · Z 로 고정된 사각형

// 레이가 보는 구의 가운데 — 고정 사각형은 기준점에서 사각형 가운데로 옮긴다.
//  카메라를 보는 사각형은 레이 쪽에서 카메라 축을 모르므로 기준점 그대로(offset 이 있으면 그만큼 어긋난다).
//  ★ GRenderer.cpp PrepareParticleAccel 의 AABB 가 같은 식이다.
float3 ParticleRayCenter(ParticleSprite p)
{
	if ((p.flags & kParticleFixedAxes) == 0)
		return p.position;
	return p.position + (p.axisX * p.offsetX + p.axisY * p.offsetY) * p.size;
}

#endif
