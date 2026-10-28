// ============================================================
//  RtReflection.hlsl — [4b] RT 반사 (DispatchRays 셰이더 라이브러리, lib_6_6, 2026-10-27 렌더 4단계)
//
//  DispatchRays(화면 크기) 한 번:
//    RayGen      G-buffer 픽셀마다 반사 레이 1개 → 반사 텍스처(바인드리스 UAV)에 결과 색
//    Miss        아무것도 안 맞으면 하늘색
//    ClosestHit  맞은 물체를 그 자리에서 셰이딩 — 정점 · 재질 · 텍스처를 SM 6.6 바인드리스로 읽는다
//                (태양 직접광 × 태양 가시도(RayQuery) + 앰비언트 + 자체발광, 재귀 반사 없음)
//  [4d] RtReflectionDenoise.hlsl 이 노이즈를 지우고, [5] Lighting.hlsl 이 F_env × 반사 색으로 더한다.
//
//  ★ 2026-10-27 렌더 5단계 — 반사 레이 방향을 B 방식으로 (사용자 결정 10-27)
//    거울 방향 하나가 아니라, 거칠기 분포(GGX 의 «보이는 법선», Heitz 2018)에서 픽셀 · 프레임마다 방향을 하나 뽑는다.
//    한 프레임만 보면 점박이 노이즈 → [4d] 가 이웃 픽셀 · 이전 프레임과 섞어 거칠기만큼 흐린 반사로 만든다.
//    출력 a 채널 = 레이가 맞은 거리(미스면 gMaxDistance) — 나중에 NRD · DLSS RR 이 요구하는 «반사 히트 거리» 자리.
//    원문 추정식의 가중치 F·G2/G1 중 F 는 라이팅의 F_env 가 맡고, G2/G1(≤ 1, 매끈할수록 1)은 지금 생략한다.
//  ★ 반사 레이는 투과 재질 물체도 불투명 표면으로 본다(얼음 패스 [7] 전까지).
//    2026-10-28 알파 마스크 — 전에는 RAY_FLAG_FORCE_OPAQUE 로 쐈다. 이제 플래그 없이 쏘고, 비불투명 인스턴스(마스크 · 투과)는
//    AnyHit 이 받는다: 마스크로 빈 곳이면 IgnoreHit(지나감), 그 밖(마스크의 있는 곳 · 얼음)은 그대로 받아들인다 → 얼음은 예전과 같다.
//  ★ 바인드리스: 루트 시그니처에 CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, 힙은 GRenderer 의 bindlessHeap.
//    히트 셰이더 안 번호는 레이마다 다를 수 있어 NonUniformResourceIndex 로 감싼다(사양 요구).
//  ★ 2026-10-27 렌더 6단계 — 하늘
//    · Miss 는 [6] 하늘 텍스처(Sky.hlsli)를 레이 방향으로 읽는다(예전: 지우기 색 상수).
//    · ClosestHit 의 태양은 조도(gSunIlluminance), 주변광은 하늘 조도 텍스처(맞은 점 법선 방향) — 메인 라이팅과 같은 단위.
//  ★ 2026-10-27 렌더 11단계 — [3] 분류
//    GRenderer 가 ExecuteIndirect 로 «레이가 필요한 8×8 타일 수 × 64» 만큼만 RayGen 을 띄운다(ReflectionClassify.hlsl).
//    RayGen 은 1차원 순번 → 타일 목록 칸(순번 / 64) · 타일 안 자리(순번 % 64) → 픽셀. 고르지 않은 타일의 0 은 분류가 이미 썼다.
//    gTileListIndex == kNoTileList 면 예전처럼 DispatchRays(화면 크기) · 픽셀 = DispatchRaysIndex().xy.
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "RtSurface.hlsli"   // 7단계 — FetchHitSurface (RtCommon.hlsli 포함)
#include "Sky.hlsli"
#include "Probes.hlsli"      // 7단계 — [4c] GI 프로브 샘플
#include "ReflectionTiles.hlsli"   // 11단계 — [3] 분류와 같은 «레이가 필요한가» 판정 · 타일 번호 규칙

// GRenderer.cpp 의 ReflectionConstants 와 같은 배치 (416바이트 — 7단계에서 144 → 224, 10-28 GI 두 겹 → 320, 지면에 맞춘 축 → 416)
cbuffer ReflectionCB : register(b0)
{
	float4x4 gInvViewProj;
	float3   gEyePos;        float gMaxDistance;     // 레이 길이 상한(m)
	float3   gSunDir;        float gMaxRoughness;    // 이 거칠기 이상은 반사 레이를 쏘지 않는다
	uint2    gSize;          uint  gNormalIndex;     uint gMaterialIndex;   // ↓ 바인드리스 힙 번호
	uint     gDepthIndex;    uint  gOutputIndex;     uint  gFrameIndex;   uint gSkyIndex;   // 5단계 난수 씨앗 · 6단계 하늘 텍스처
	float3   gSunIlluminance; uint gSkyIrradianceIndex;   // 6단계 — 태양 조도(RGB) · 하늘 조도 텍스처
	ProbeVolume gProbeVolume;                             // 7단계 — [4c] GI 프로브 가까운 겹 (10-28 64 → 128 B)
	uint     gProbeIrradianceIndex; uint gProbeDistanceIndex; uint gProbeStateIndex;
	uint     gTileListIndex;   // 11단계 — [3] 분류 타일 목록 SRV (kNoTileList 면 화면 전체 디스패치)
	ProbeVolume gProbeVolumeFar;                          // 2026-10-28 — 먼 겹 (같은 아틀라스의 뒤쪽)
};

RaytracingAccelerationStructure gScene       : register(t0);   // TLAS (루트 SRV)
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);   // 재질 표 (루트 SRV)
StructuredBuffer<RtInstance>    gRtInstances : register(t2);   // TLAS 인스턴스 표 (루트 SRV, 프레임마다)
SamplerState                    gSampler     : register(s0);   // 정적 샘플러 (선형 · WRAP)
SamplerState                    gSkySampler  : register(s1);   // 6단계 — 선형 · U 반복 · V 고정 (등장방형 하늘)
SamplerState                    gClampSampler : register(s2);  // 7단계 — 선형 · 고정 (프로브 아틀라스)

struct ReflectionPayload
{
	float3 color;
	float  hitT;   // 맞은 거리 (미스면 -1) — 나중에 디노이즈 · 흐림 정도에 쓸 자리
};

// 반사 레이 방향 — B 방식 (5단계).
//  ① 법선 N 기준 접선 공간을 만들고 ② 시선을 그 공간으로 옮겨 ③ GGX 보이는 법선(미세면) H 를 하나 뽑고
//  ④ 시선을 H 에 대해 반사한다(원문 식 16: L = reflect(V, N_i)).
//  뽑힌 방향이 표면 아래로 가면(노멀맵 · 비스듬한 시선에서 생긴다) 거울 방향으로 대신한다.
float3 ReflectionDirection(float3 N, float3 V, float rough, float2 u)
{
	float3 T, B;
	BuildOrthonormalBasis(N, T, B);
	const float3 Ve = float3(dot(V, T), dot(V, B), dot(V, N));
	const float3 He = SampleGgxVndf(Ve, rough * rough, u);
	const float3 H  = He.x * T + He.y * B + He.z * N;
	const float3 L  = reflect(-V, H);
	return (dot(L, N) > 0.0) ? L : reflect(-V, N);
}

[shader("raygeneration")]
void RayGen()
{
	// 11단계 — [3] 분류 타일 목록이 있으면 1차원 순번 → 타일 → 픽셀
	uint2 pixel;
	if (gTileListIndex != kNoTileList)
	{
		StructuredBuffer<uint> tiles = ResourceDescriptorHeap[gTileListIndex];
		const uint ray = DispatchRaysIndex().x;
		pixel = ReflectionTilePixel(tiles[ray / kReflectionTilePixels], ray % kReflectionTilePixels);
		if (any(pixel >= gSize))
			return;   // 오른쪽 · 아래 끝 타일의 화면 밖 자리
	}
	else
	{
		pixel = DispatchRaysIndex().xy;
	}
	RWTexture2D<float4> output      = ResourceDescriptorHeap[gOutputIndex];
	Texture2D<float>    depthTex    = ResourceDescriptorHeap[gDepthIndex];
	Texture2D<float2>   normalTex   = ResourceDescriptorHeap[gNormalIndex];
	Texture2D<float4>   materialTex = ResourceDescriptorHeap[gMaterialIndex];

	// 메인 패스 물체 없음(깊이 1) · 거친 면 → 레이 없음(라이팅은 반사 0). 판정은 [3] 분류와 같은 함수(ReflectionTiles.hlsli).
	const float depth = depthTex[pixel];
	const float rawRough = materialTex[pixel].r;
	if (!NeedsReflectionRay(depth, rawRough, gMaxRoughness))
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);
		return;
	}
	const float rough = max(rawRough, kMinRoughness);

	const float3 P   = ReconstructWorldPosition(pixel, depth, gSize, gInvViewProj);
	const float3 N   = DecodeNormalOct(normalTex[pixel]);
	const float3 V   = normalize(gEyePos - P);
	const float3 dir = ReflectionDirection(N, V, rough, Random2(pixel, gFrameIndex));
	if (dot(dir, N) <= 0.0)
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 노멀맵 때문에 반사 방향이 표면 아래로 — 쏘지 않는다
		return;
	}

	RayDesc ray;
	ray.Origin    = P + N * RayOffsetBias(length(P - gEyePos));
	ray.Direction = dir;
	ray.TMin      = 0.0;
	ray.TMax      = gMaxDistance;

	ReflectionPayload payload;
	payload.color = float3(0.0, 0.0, 0.0);
	payload.hitT  = -1.0;
	// 히트 그룹 0 번 하나, 미스 0 번 하나. 기하 기여 배수 1(BLAS 마다 기하 1개).
	// 2026-10-28 — FORCE_OPAQUE 를 뺐다: 알파 마스크 인스턴스는 AnyHit 이 빈 곳을 거른다.
	TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

	// a = 맞은 거리 (5단계 — 미스는 레이 길이 상한으로)
	output[pixel] = float4(payload.color, payload.hitT >= 0.0 ? payload.hitT : gMaxDistance);
}

[shader("miss")]
void Miss(inout ReflectionPayload payload)
{
	Texture2D<float4> sky = ResourceDescriptorHeap[gSkyIndex];   // 6단계 — [6] 하늘 텍스처 (해 원반 제외)
	payload.color = SampleSkyRadiance(sky, gSkySampler, WorldRayDirection());
	payload.hitT  = -1.0;
}

// 2026-10-28 알파 마스크 — 비불투명 인스턴스(FORCE_NON_OPAQUE: 마스크 · 투과)를 맞을 때만 불린다.
//  빈 곳이면 IgnoreHit 으로 지나가고, 아니면 아무것도 안 해서 그대로 받아들인다(가장 가까운 것이 ClosestHit 으로 간다).
[shader("anyhit")]
void AnyHit(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
	const RtInstance inst = gRtInstances[InstanceIndex()];
	if (AlphaMaskedOut(inst, gRtMaterials[inst.material], PrimitiveIndex(), attr.barycentrics, gSampler))
		IgnoreHit();
}

[shader("closesthit")]
void ClosestHit(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
	const RtInstance inst = gRtInstances[InstanceIndex()];
	const RtMaterial mat  = gRtMaterials[inst.material];

	// ── 맞은 점 표면 — 정점 세 개 무게중심 보간 + 재질 텍스처 (7단계에서 RtSurface.hlsli FetchHitSurface 로 옮김) ──
	//  래스터(Forward.hlsl SampleSurface)와 같은 식. 노멀맵은 아직 읽지 않는다.
	const HitSurface surface = FetchHitSurface(inst, mat, PrimitiveIndex(), attr.barycentrics, WorldToObject3x4(), gSampler);
	float3       N = surface.N;
	const float3 V = -WorldRayDirection();
	if (dot(N, V) < 0.0) N = -N;   // 뒷면을 맞았으면 뒤집는다(양면처럼)
	const float3 albedo   = surface.albedo;
	const float  rough    = surface.rough;
	const float  metallic = surface.metallic;
	const float3 emissive = surface.emissive;

	// ── 맞은 점의 조명 — 메인 라이팅과 같은 식(Shading.hlsli) + 같은 그림자 규칙(RtCommon.hlsli) ──
	const float3 P   = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
	// 주변광: 7단계 — [4c] GI 프로브(쓸 수 있으면), 아니면 6단계 하늘 조도/π
	float3 ambientEOverPi = float3(0.0, 0.0, 0.0);
	float  probeWeight = 0.0;
	if (gProbeVolume.enabled != 0 || gProbeVolumeFar.enabled != 0)
	{
		Texture2D<float4>      probeIrradiance = ResourceDescriptorHeap[gProbeIrradianceIndex];
		Texture2D<float2>      probeDistance   = ResourceDescriptorHeap[gProbeDistanceIndex];
		StructuredBuffer<int4> probeStates     = ResourceDescriptorHeap[gProbeStateIndex];
		uint probeLayer;   // 2026-10-28 두 겹 — 가까운 겹 → 먼 겹
		ambientEOverPi = SampleProbeLayers(probeIrradiance, probeDistance, probeStates, gClampSampler,
			gProbeVolume, gProbeVolumeFar, P, N, V, probeWeight, probeLayer);
	}
	if (probeWeight <= 0.0)
	{
		Texture2D<float4> skyIrradiance = ResourceDescriptorHeap[gSkyIrradianceIndex];   // 6단계 — 하늘 조도/π
		ambientEOverPi = SampleSkyIrradiance(skyIrradiance, gSkySampler, N);
	}
	const DirectLighting lit = EvaluateSunAndAmbient(albedo, N, V, rough, metallic, gSunDir,
		gSunIlluminance, ambientEOverPi);
	const float3 L   = normalize(-gSunDir);
	const float3 sun = (dot(N, L) > 0.0) ? TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gSampler, P + N * 0.02, L, gMaxDistance)
	                                     : float3(0.0, 0.0, 0.0);

	payload.color = (lit.diffuse + lit.specular) * sun + lit.ambient + emissive;
	payload.hitT  = RayTCurrent();
}
