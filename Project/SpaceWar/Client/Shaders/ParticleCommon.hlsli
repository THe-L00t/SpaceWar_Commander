// ============================================================
//  ParticleCommon.hlsli — 파티클 사각형 한 칸 (2026-11-17)
//
//  [7b] Particles.hlsl(래스터)와 ParticleRt.hlsli(레이 — 그림자 · AO · GI · 반사)가 같은 배열을 읽는다.
//  ★ C++ 의 swc::ParticleSprite(InstanceData.h, 48 B) · ParticleFlags 와 같은 배치 · 같은 값이어야 한다.
// ============================================================
#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

struct ParticleSprite
{
	float3 position;  float size;    // 월드 위치(m) · 사각형 한 변(m) — 레이는 지름이 size 인 구로 본다
	float4 color;                    // 선형 RGB · a = 불투명도
	float  rotation;  uint  flags;  float2 _pad;
};
static const uint kParticleAdditive = 1u;   // 가산(섬광 · 불꽃 — 스스로 빛남)
static const uint kParticleLit      = 2u;   // 해 · 하늘 빛을 받는다(먼지 · 연기 · 피)

#endif
