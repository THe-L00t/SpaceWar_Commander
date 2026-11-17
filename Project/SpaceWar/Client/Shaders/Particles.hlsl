// ============================================================
//  Particles.hlsl — [7b] 파티클 사각형 (래스터 forward, vs/gs/ps_6_6 · 바인드리스, 2026-11-17)
//
//  사용자(2026-11-17): «파티클 전용 파이프라인», GS(지오메트리 셰이더)와 정점 셰이더 펴기 중 «예산이 싸게 먹힐지 확인».
//  → 둘 다 둔다. GRenderer.cpp kParticleExpandInGs 로 골라 PSO 를 만든다(자동 측정으로 비교).
//    · 정점 셰이더로 펴기(기본)  VSQuad            — 파티클 하나 = 정점 6개(삼각형 2개), DrawInstanced(개수 × 6)
//    · GS 로 펴기                VSPoint + GSQuad  — 점 하나 → 정점 4개(삼각형 띠), DrawInstanced(개수), POINTLIST
//  픽셀 셰이더(PSParticle)는 같다. 원 모양 · 깊이 페이드(땅에 닿는 경계를 흐리게) · 해와 하늘 빛(알파 묶음만).
//  알파 묶음(먼지 · 연기 · 피)은 «미리 곱한 알파» 블렌드, 가산 묶음(섬광 · 불꽃)은 더하기 블렌드 — 출력은 둘 다 (rgb × a, a).
//  ★ 파티클 배열 = FramePacket::particles(ParticleSystem 이 채움). 루트 시그니처는 [7] 반투명 것(transparentRootSig)을 다시 쓴다:
//    b0 = ParticleCB(루트 CBV), b1 = DrawCB(루트 상수 4), t0 = gParticles(루트 SRV), s1 = 하늘 샘플러.
//  ★ 좌표는 카메라 기준 — (월드 − 눈) × 카메라 기준 viewProj (Transparent.hlsl · Forward.hlsl 과 같은 깊이).
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "Sky.hlsli"
#include "ParticleCommon.hlsli"   // 2026-11-17 P5 — 파티클 한 칸 정의를 레이 쪽(ParticleRt.hlsli)과 함께 쓴다

// GRenderer.cpp 의 ParticleConstants 와 같은 배치 (224바이트)
cbuffer ParticleCB : register(b0)
{
	float4x4 gViewProj;          // 카메라 기준, 전치
	float4x4 gInvViewProj;       // 카메라 기준 역행렬(GBuffer.hlsli ReconstructWorldPosition — eye 를 더해 월드로)
	float3   gEyePos;            uint  gDepthSrv;            // ↓ 바인드리스 번호
	float3   gCameraRight;       uint  gSkyIrradianceSrv;
	float3   gCameraUp;          float gSoftDistance;        // 깊이 페이드 폭(m)
	float3   gSunDir;            uint  gWidth;               // 해 → 장면 방향(Transparent.hlsl 과 같다 — 빛 = −gSunDir)
	float3   gSunIlluminance;    uint  gHeight;
	float3   gPlanetUp;          uint  _pad0;                // 카메라 자리의 위(행성 중심 → 카메라) — 빛을 받는 면의 방향으로 쓴다
};
cbuffer DrawCB : register(b1)
{
	uint gFirst;   // 이번 그리기의 첫 파티클(알파 묶음 0, 가산 묶음 = 알파 수)
	uint gCount;
	uint2 _pad1;
};

// ParticleSprite · kParticleAdditive · kParticleLit = ParticleCommon.hlsli(위 include, P5 레이 쪽과 같은 정의)
StructuredBuffer<ParticleSprite> gParticles : register(t0);
SamplerState gSkySampler : register(s1);

struct VOut
{
	float4 pos   : SV_POSITION;
	float3 rel   : RELPOS;     // 카메라 기준 위치(깊이 페이드)
	float2 local : LOCAL;      // 사각형 안 좌표 −1 ~ 1
	nointerpolation float4 color : COLOR;
	nointerpolation uint   flags : FLAGS;
};

static const float2 kCorners[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
static const uint   kQuadCorner[6] = { 0, 1, 2, 2, 1, 3 };   // 정점 셰이더 펴기 — 삼각형 2개

VOut MakeVertex(ParticleSprite p, float2 corner)
{
	float s, c;
	sincos(p.rotation, s, c);
	const float2 r = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c) * (0.5 * p.size);
	const float3 rel = (p.position - gEyePos) + gCameraRight * r.x + gCameraUp * r.y;
	VOut o;
	o.pos   = mul(float4(rel, 1.0), gViewProj);
	o.rel   = rel;
	o.local = corner;
	o.color = p.color;
	o.flags = p.flags;
	return o;
}

// ── 정점 셰이더로 펴기 (기본) — 정점 번호로 파티클 · 모서리를 고른다(입력 레이아웃 없음) ──
VOut VSQuad(uint vertexId : SV_VertexID)
{
	const ParticleSprite p = gParticles[gFirst + vertexId / 6];
	return MakeVertex(p, kCorners[kQuadCorner[vertexId % 6]]);
}

// ── GS 로 펴기 — 정점 셰이더는 파티클 번호만 넘기고, GS 가 점 하나를 사각형으로 편다 ──
struct PointOut
{
	uint particle : PARTICLE;
};
PointOut VSPoint(uint vertexId : SV_VertexID)
{
	PointOut o;
	o.particle = gFirst + vertexId;
	return o;
}

[maxvertexcount(4)]
void GSQuad(point PointOut input[1], inout TriangleStream<VOut> stream)
{
	const ParticleSprite p = gParticles[input[0].particle];
	[unroll]
	for (uint i = 0; i < 4; ++i)
		stream.Append(MakeVertex(p, kCorners[i]));
}

float4 PSParticle(VOut i) : SV_Target
{
	const float r2 = dot(i.local, i.local);
	if (r2 > 1.0) discard;
	float shape = 1.0 - r2;      // 가운데 진하고 가장자리 0 — 둥근 모양
	shape *= shape;

	// 깊이 페이드 — 뒤의 불투명 표면과 가까울수록 옅게(땅 · 벽에 닿는 경계가 칼로 자른 듯 보이지 않게)
	float fade = 1.0;
	Texture2D<float> depthTex = ResourceDescriptorHeap[gDepthSrv];
	const uint2 size = uint2(gWidth, gHeight);
	const uint2 px = min(uint2(i.pos.xy), size - 1);
	const float d = depthTex[px];
	if (d < 1.0)
	{
		const float3 sceneRel = ReconstructWorldPosition(px, d, size, gInvViewProj, gEyePos) - gEyePos;
		fade = saturate((length(sceneRel) - length(i.rel)) / gSoftDistance);
	}

	float3 rgb = i.color.rgb;
	if ((i.flags & kParticleLit) != 0)
	{
		// 해(반쯤 감싼 빛 — 사각형이라 앞뒤가 없다) + 하늘 조도. 단위는 Transparent.hlsl ShadeHitDiffuse 와 같다.
		Texture2D<float4> skyIrradiance = ResourceDescriptorHeap[gSkyIrradianceSrv];
		const float3 ambient = SampleSkyIrradiance(skyIrradiance, gSkySampler, gPlanetUp);
		const float  wrap    = saturate(dot(gPlanetUp, -gSunDir)) * 0.5 + 0.5;
		rgb *= gSunIlluminance * (wrap / kPi) + ambient;
	}
	const float a = saturate(i.color.a * shape * fade);
	return float4(rgb * a, a);
}
