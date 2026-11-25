// ============================================================
//  Particles.hlsl — [7b] 파티클 사각형 (래스터 forward, vs/gs/ps_6_6 · 바인드리스, 2026-11-17)
//
//  사용자(2026-11-17): «파티클 전용 파이프라인», GS(지오메트리 셰이더)와 정점 셰이더 펴기 중 «예산이 싸게 먹힐지 확인».
//  → 둘 다 둔다. GRenderer.cpp kParticleExpandInGs 로 골라 PSO 를 만든다(자동 측정으로 비교).
//    · 정점 셰이더로 펴기(기본)  VSQuad            — 파티클 하나 = 정점 6개(삼각형 2개), DrawInstanced(개수 × 6)
//    · GS 로 펴기                VSPoint + GSQuad  — 점 하나 → 정점 4개(삼각형 띠), DrawInstanced(개수), POINTLIST
//  픽셀 셰이더(PSParticle)는 같다. 원 모양 · 깊이 페이드(땅에 닿는 경계를 흐리게) · 해와 하늘 빛(알파 묶음만).
//  2026-11-23 플립북 — 파티클에 플립북이 붙어 있으면(ParticleSprite::flipbook) 원 모양 대신 텍스처 칸(두 프레임 섞음)의 알파 · 색을 쓰고,
//    노멀 텍스처가 있으면 그 노멀로 해 · 하늘빛을 받는다(빛은 텍스처에 굽지 않는다). 표 = ParticleCB gFlipbooks.
//  알파 묶음(먼지 · 연기 · 피)은 «미리 곱한 알파» 블렌드, 가산 묶음(섬광 · 불꽃)은 더하기 블렌드 — 출력은 둘 다 (rgb × a, a).
//  ★ 파티클 배열 = FramePacket::particles(ParticleSystem 이 채움). 루트 시그니처는 [7] 반투명 것(transparentRootSig)을 다시 쓴다:
//    b0 = ParticleCB(루트 CBV), b1 = DrawCB(루트 상수 4), t0 = gParticles(루트 SRV), s1 = 하늘 샘플러.
//    2026-11-25 t1 = gParticleLight(루트 SRV — [0b] 결과, 빛 패스가 안 돌았으면 아무 버퍼나 묶고 gLightEnabled = 0), s2 = 고정 샘플러.
//  ★ 좌표는 카메라 기준 — (월드 − 눈) × 카메라 기준 viewProj (Transparent.hlsl · Forward.hlsl 과 같은 깊이).
//  ★ 2026-11-25 해 그림자 · AO · GI 프로브 (사용자 «그늘에서 파티클이 튀어도 태양 빛을 받은것 처럼 밝아» → «그렇게 작업해줘»):
//    · 해 가시도 · AO = [0b] ParticleLight.hlsl 이 파티클마다 미리 계산한 값(t1 gParticleLight, VS 에서 파티클 번호로 읽는다).
//      해 몫에 가시도, 주변광 몫에 AO — 월드(Lighting.hlsl)와 같은 자리. 패스가 안 돌았으면(RT 끔 등) gLightEnabled = 0 → 1 로 본다.
//    · 주변광 = GI 프로브(두 겹, Transparent.hlsl ShadeHitDiffuse 와 같은 식) → 쓸 프로브가 없으면 하늘 조도(예전 값).
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "Sky.hlsli"
#include "Probes.hlsli"           // 2026-11-25 — 주변광을 GI 프로브로
#include "ParticleCommon.hlsli"   // 2026-11-17 P5 — 파티클 한 칸 정의를 레이 쪽(ParticleRt.hlsli)과 함께 쓴다

// 플립북 표 칸 수 — C++ kMaxParticleFlipbooks(InstanceData.h)와 같아야 한다
#define PARTICLE_MAX_FLIPBOOKS 128

// GRenderer.cpp 의 ParticleConstants 와 같은 배치 (224바이트 + 2026-11-25 프로브 272바이트 + 플립북 표 16 B × 칸 수)
cbuffer ParticleCB : register(b0)
{
	float4x4 gViewProj;          // 카메라 기준, 전치
	float4x4 gInvViewProj;       // 카메라 기준 역행렬(GBuffer.hlsli ReconstructWorldPosition — eye 를 더해 월드로)
	float3   gEyePos;            uint  gDepthSrv;            // ↓ 바인드리스 번호
	float3   gCameraRight;       uint  gSkyIrradianceSrv;
	float3   gCameraUp;          float gSoftDistance;        // 깊이 페이드 폭(m)
	float3   gSunDir;            uint  gWidth;               // 해 → 장면 방향(Transparent.hlsl 과 같다 — 빛 = −gSunDir)
	float3   gSunIlluminance;    uint  gHeight;
	float3   gPlanetUp;          uint  gLightEnabled;        // 카메라 자리의 위(행성 중심 → 카메라) — 빛을 받는 면의 방향으로 쓴다
	                                                         //  · 2026-11-25 gLightEnabled(예전 _pad0) = 1 이면 t1 gParticleLight 를 읽는다
	// 2026-11-25 — GI 프로브(Transparent.hlsl TransparentCB 와 같은 값)
	uint     gProbeIrradianceSrv; uint gProbeDistanceSrv;    uint gProbeStateSrv;   uint _pad0;
	ProbeVolume gProbeVolume;        // GI 프로브 가까운 겹
	ProbeVolume gProbeVolumeFar;     // 먼 겹
	// 2026-11-23 플립북 표 — (색 바인드리스, 노멀 바인드리스, 열 | 행 << 16, 프레임 수). 번호 0xFFFFFFFF = 없음.
	//  칸 수 = C++ kMaxParticleFlipbooks(InstanceData.h). ParticleSprite::flipbook − 1 이 칸 번호.
	//  2026-11-24 8 → 128 (사용자 «먼지 전부, 연기 전부» — 층마다 여러 장, 69장 + 피)
	uint4    gFlipbooks[PARTICLE_MAX_FLIPBOOKS];
};
static const uint kNoTexture = 0xFFFFFFFFu;
cbuffer DrawCB : register(b1)
{
	uint gFirst;   // 이번 그리기의 첫 파티클(알파 묶음 0, 가산 묶음 = 알파 수)
	uint gCount;
	uint2 _pad1;
};

// ParticleSprite · kParticleAdditive · kParticleLit = ParticleCommon.hlsli(위 include, P5 레이 쪽과 같은 정의)
StructuredBuffer<ParticleSprite> gParticles : register(t0);
// 2026-11-25 — 파티클마다 (해 가시도 RGB, AO). [0b] ParticleLight.hlsl 이 같은 번호 자리에 쓴다(이번 프레임 슬롯을 루트 SRV 로 묶는다)
StructuredBuffer<float4>         gParticleLight : register(t1);
SamplerState gSkySampler : register(s1);
SamplerState gClampSampler : register(s2);   // 2026-11-23 플립북 — 선형 · 가장자리 고정(transparentRootSig 의 세 번째 정적 샘플러)
                                             //  2026-11-25 GI 프로브 아틀라스도 이 샘플러로 읽는다(Transparent.hlsl 과 같다)

struct VOut
{
	float4 pos   : SV_POSITION;
	float3 rel   : RELPOS;     // 카메라 기준 위치(깊이 페이드)
	float2 local : LOCAL;      // 사각형 안 좌표 −1 ~ 1
	nointerpolation float4 color : COLOR;
	nointerpolation uint   flags : FLAGS;
	// 2026-11-23 플립북 — 표 번호(0 = 없음) · 프레임 위치 · 화면 안 회전(노멀을 월드로 돌릴 때)
	nointerpolation uint   flipbook : FLIPBOOK;
	nointerpolation float  frame    : FRAME;
	// 2026-11-25 — 사각형 축(노멀 텍스처를 월드로 옮길 때). 예전 rotation 대신 — 카메라를 보는 사각형도 VS 에서 같은 축을 만든다.
	//  axisZ 는 고정 사각형(kParticleFixedAxes)만 쓴다(카메라를 보는 사각형은 픽셀마다 파티클 → 눈).
	nointerpolation float3 axisX    : AXISX;
	nointerpolation float3 axisY    : AXISY;
	nointerpolation float3 axisZ    : AXISZ;
	// 2026-11-25 — (해 가시도 RGB, AO) — 판 하나가 값 하나(ParticleLight.hlsl)
	nointerpolation float4 light    : LIGHT;
};

// 2026-11-25 — 파티클 번호의 (해 가시도, AO). 빛 패스가 안 돌았으면 가리는 것 없음(1)으로 본다.
float4 ParticleLightOf(uint index)
{
	return gLightEnabled != 0 ? gParticleLight[index] : float4(1.0, 1.0, 1.0, 1.0);
}

// 2026-11-25 — 주변광(조도 / π): GI 프로브 두 겹 → 쓸 프로브가 없으면 하늘 조도. Transparent.hlsl ShadeHitDiffuse 와 같은 식.
//  P = 월드 위치, N = 빛을 받는 방향, V = 눈 쪽.
float3 ParticleAmbient(float3 P, float3 N, float3 V)
{
	float  weight  = 0.0;
	float3 ambient = float3(0.0, 0.0, 0.0);
	if (gProbeVolume.enabled != 0 || gProbeVolumeFar.enabled != 0)
	{
		Texture2D<float4>      irradiance = ResourceDescriptorHeap[gProbeIrradianceSrv];
		Texture2D<float2>      distance   = ResourceDescriptorHeap[gProbeDistanceSrv];
		StructuredBuffer<int4> states     = ResourceDescriptorHeap[gProbeStateSrv];
		uint layer;
		ambient = SampleProbeLayers(irradiance, distance, states, gClampSampler, gProbeVolume, gProbeVolumeFar, P, N, V, weight, layer);
	}
	if (weight <= 0.0)
	{
		Texture2D<float4> skyIrradiance = ResourceDescriptorHeap[gSkyIrradianceSrv];
		ambient = SampleSkyIrradiance(skyIrradiance, gSkySampler, N);
	}
	return ambient;
}

static const float2 kCorners[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
static const uint   kQuadCorner[6] = { 0, 1, 2, 2, 1, 3 };   // 정점 셰이더 펴기 — 삼각형 2개

VOut MakeVertex(ParticleSprite p, float2 corner, float4 light)
{
	// 사각형 축 — 카메라를 보는 사각형은 카메라 오른쪽 · 위를 rotation 만큼 돌린 것(예전 식과 같다:
	//  right × (x c − y s) + up × (x s + y c) = x × axisX + y × axisY), 고정 사각형(2026-11-25)은 파티클에 실린 축.
	float3 axisX, axisY, axisZ;
	if ((p.flags & kParticleFixedAxes) != 0)
	{
		axisX = p.axisX;
		axisY = p.axisY;
		axisZ = p.axisZ;
	}
	else
	{
		float s, c;
		sincos(p.rotation, s, c);
		axisX = gCameraRight * c + gCameraUp * s;
		axisY = -gCameraRight * s + gCameraUp * c;
		axisZ = float3(0.0, 0.0, 0.0);   // PS 가 픽셀마다 파티클 → 눈으로
	}
	// 2026-11-25 — position 은 기준점(플립북 pivot — 피는 상처 자리). 사각형 가운데 = 기준점 + 축 × offset × 한 변.
	const float3 center = p.position + (axisX * p.offsetX + axisY * p.offsetY) * p.size;
	const float2 r = corner * (0.5 * p.size);
	const float3 rel = (center - gEyePos) + axisX * r.x + axisY * r.y;
	VOut o;
	o.pos   = mul(float4(rel, 1.0), gViewProj);
	o.rel   = rel;
	o.local = corner;
	o.color = p.color;
	o.flags = p.flags;
	o.flipbook = p.flipbook;
	o.frame    = p.frame;
	o.axisX    = axisX;
	o.axisY    = axisY;
	o.axisZ    = axisZ;
	o.light    = light;
	return o;
}

// ── 플립북 한 칸의 UV (2026-11-23) — local(−1 ~ 1, 위 = +1) → 칸 안 UV(위 = 0), 칸 가장자리 반 텍셀 안쪽으로(옆 칸이 번지지 않게) ──
float2 FlipbookUv(float2 local, uint frameIndex, uint columns, uint rows, float2 texSize)
{
	const float2 cellUv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	const float2 cellSize = texSize / float2(columns, rows);
	const float2 inset = 0.5 / cellSize;
	const float2 uv = clamp(cellUv, inset, 1.0 - inset);
	const uint2 cell = uint2(frameIndex % columns, frameIndex / columns);
	return (float2(cell) + uv) / float2(columns, rows);
}

// 두 프레임(내림 · 다음)을 프레임 위치의 소수만큼 섞어 읽는다
float4 SampleFlipbook(Texture2D<float4> tex, float2 local, float frame, uint columns, uint rows, uint frameCount)
{
	float2 texSize;
	tex.GetDimensions(texSize.x, texSize.y);
	const float clamped = clamp(frame, 0.0, float(max(frameCount, 1u) - 1u));
	const uint  f0 = uint(floor(clamped));
	const uint  f1 = min(f0 + 1u, max(frameCount, 1u) - 1u);
	const float w  = clamped - float(f0);
	const float4 a = tex.SampleLevel(gClampSampler, FlipbookUv(local, f0, columns, rows, texSize), 0);
	const float4 b = tex.SampleLevel(gClampSampler, FlipbookUv(local, f1, columns, rows, texSize), 0);
	return lerp(a, b, w);
}

// ── 정점 셰이더로 펴기 (기본) — 정점 번호로 파티클 · 모서리를 고른다(입력 레이아웃 없음) ──
VOut VSQuad(uint vertexId : SV_VertexID)
{
	const uint index = gFirst + vertexId / 6;
	const ParticleSprite p = gParticles[index];
	return MakeVertex(p, kCorners[kQuadCorner[vertexId % 6]], ParticleLightOf(index));
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
	const float4 light = ParticleLightOf(input[0].particle);
	[unroll]
	for (uint i = 0; i < 4; ++i)
		stream.Append(MakeVertex(p, kCorners[i], light));
}

float4 PSParticle(VOut i) : SV_Target
{
	// 2026-11-23 플립북 — 붙어 있으면 텍스처가 모양(알파) · 색 · 노멀을 정하고, 없으면 예전처럼 둥근 모양을 계산한다.
	uint4 fb = uint4(kNoTexture, kNoTexture, 0u, 0u);
	if (i.flipbook != 0u && i.flipbook <= PARTICLE_MAX_FLIPBOOKS)
		fb = gFlipbooks[i.flipbook - 1u];

	float  shape = 1.0;
	float3 texColor = float3(1.0, 1.0, 1.0);
	float3 normalCam = float3(0.0, 0.0, 1.0);   // 카메라 공간(X 사각형 오른쪽 · Y 위 · Z 카메라 쪽)
	bool   hasNormal = false;
	if (fb.x != kNoTexture)
	{
		const uint columns = max(fb.z & 0xFFFFu, 1u);
		const uint rows    = max(fb.z >> 16, 1u);
		// 한 그리기 안에서 파티클마다 플립북이 다를 수 있다 → NonUniformResourceIndex
		Texture2D<float4> colorTex = ResourceDescriptorHeap[NonUniformResourceIndex(fb.x)];
		const float4 c = SampleFlipbook(colorTex, i.local, i.frame, columns, rows, fb.w);   // sRGB 텍스처 → 선형으로 읽힌다
		shape = c.a;
		texColor = c.rgb;
		if (shape <= 0.002) discard;
		if (fb.y != kNoTexture)
		{
			Texture2D<float4> normalTex = ResourceDescriptorHeap[NonUniformResourceIndex(fb.y)];
			normalCam = SampleFlipbook(normalTex, i.local, i.frame, columns, rows, fb.w).xyz * 2.0 - 1.0;
			hasNormal = dot(normalCam, normalCam) > 1e-4;
		}
	}
	else
	{
		const float r2 = dot(i.local, i.local);
		if (r2 > 1.0) discard;
		shape = 1.0 - r2;        // 가운데 진하고 가장자리 0 — 둥근 모양
		shape *= shape;
	}

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

	float3 rgb = i.color.rgb * texColor;
	if ((i.flags & kParticleLit) != 0)
	{
		// 2026-11-25 — 해 몫 × 해 가시도(그림자), 주변광 = GI 프로브(→ 하늘 조도) × AO
		const float3 sunVisibility = i.light.rgb;
		const float  ao            = i.light.a;
		const float3 P = gEyePos + i.rel;      // 월드 위치(프로브를 읽는 자리)
		const float3 V = normalize(-i.rel);    // 눈 쪽
		if (hasNormal)
		{
			// 2026-11-23 — 플립북 노멀로 비춘다(빛은 굽지 않고 게임이 준다). 카메라 공간 노멀을 사각형의 축으로 월드에 옮긴다:
			//  X = 사각형 오른쪽 · Y = 사각형 위(MakeVertex 가 만든 축), Z = 파티클 → 눈.
			//  2026-11-25 고정 사각형 — Z = 사각형이 보는 쪽. 뒤에서 보면 Z 를 뒤집는다(얇은 판 — 보는 사람 쪽 면이 빛을 받게).
			float3 axisZ = normalize(-i.rel);
			if ((i.flags & kParticleFixedAxes) != 0)
				axisZ = dot(i.axisZ, -i.rel) >= 0.0 ? i.axisZ : -i.axisZ;
			const float3 n = normalize(i.axisX * normalCam.x + i.axisY * normalCam.y + axisZ * normalCam.z);
			const float3 ambient = ParticleAmbient(P, n, V);
			rgb *= gSunIlluminance * (saturate(dot(n, -gSunDir)) / kPi) * sunVisibility + ambient * ao;
		}
		else
		{
			// 해(반쯤 감싼 빛 — 사각형이라 앞뒤가 없다) + 주변광. 단위는 Transparent.hlsl ShadeHitDiffuse 와 같다.
			const float3 ambient = ParticleAmbient(P, gPlanetUp, V);
			const float  wrap    = saturate(dot(gPlanetUp, -gSunDir)) * 0.5 + 0.5;
			rgb *= gSunIlluminance * (wrap / kPi) * sunVisibility + ambient * ao;
		}
	}
	const float a = saturate(i.color.a * shape * fade);
	return float4(rgb * a, a);
}
