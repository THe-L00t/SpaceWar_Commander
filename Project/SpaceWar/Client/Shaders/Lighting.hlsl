// ============================================================
//  Lighting.hlsl — [5] 라이팅 합성 (컴퓨트, 2026-10-27 파이프라인 1단계)
//
//  G-buffer(GBuffer.hlsli)를 읽어 픽셀마다 조명을 계산하고 HDR 컬러(u0)에 쓴다.
//  메인 패스 물체가 있는 픽셀(깊이 < 1)만 쓴다 — 나머지는 지우기 색 · 배경 패스 결과가 그대로 남는다.
//  식은 Shading.hlsli 의 EvaluateSunAndAmbient 하나를 배경 패스(Forward.hlsl PSMain)와 같이 쓴다.
//
//  합성 식 — 구조도의 각 단계가 들어올 자리를 이름으로 남긴다.
//    color = (태양 확산 + 태양 정반사) × shadow   ← [4a] RT 그림자 (2단계에서 연결 — RtShadow.hlsl 의 RGB 가시도)
//          + 앰비언트 확산 × ao                    ← [4c] GI 프로브 · [2] AO (아직 없음 → 상수 앰비언트, 1)
//          + F_env × reflection                    ← [4b] 반사 (아직 없음 → 0)
//          + 자체발광
//
//  ★ 2026-10-27 렌더 2단계 — shadow 를 그림자 마스크(지금 t6)에서 읽는다.
//    gShadowEnabled = 0 (RT 미지원 · R 키로 끔 · TLAS 비어 있음)이면 마스크를 읽지 않고 1 로 둔다 → 1단계 화면과 같다.
//    월드 위치 되살리기는 GBuffer.hlsli 의 ReconstructWorldPosition 으로 옮겼다(그림자 셰이더와 같이 쓴다).
//
//  ★ 2026-10-27 렌더 5단계 — 반사는 [4d] 노이즈 제거 후 것. 디버그용으로 제거 전 반사 · GB4 모션 벡터를 더 읽는다.
//    후속 — GB4 를 G-buffer 묶음(t0~t4)에 넣었다. 그래서 깊이 t4 → t5, 그림자 t5 → t6, 반사 t6 → t7, 제거 전 반사 t7 → t8.
//    (GRenderer.cpp 의 라이팅 테이블: t0~t5 · u0 · t6~t8 세 범위)
//
//  ★ 2026-10-27 렌더 6단계 — 하늘 · 조명 단위
//    · 태양은 렌더러가 넘긴 조도(gSunIlluminance — 대기 투과로 색이 붙고, 해가 지면 0). 확산은 알베도/π (Shading.hlsli).
//    · 주변광 = 하늘 조도 텍스처(t9, 법선 방향별 «조도/π»). 하늘을 못 만든 장치(gSkyEnabled 0)는 예전 상수 kAmbient.
//      [4c] GI 프로브가 들어오면 그 자리를 GI 가 맡는다.
//    · 물체가 없는 픽셀(깊이 1)은 [6] Sky.hlsl CSDrawSky 가 이미 하늘을 그려 두었다 — 여기서는 건드리지 않는다.
//
//  ★ 2026-10-27 렌더 7단계 — 주변광 = [4c] GI 프로브 (Probes.hlsli SampleProbes, t10 조도 · t11 거리 · t12 상태)
//    프로브를 쓸 수 없는 픽셀(격자 밖 · 아직 안 채워짐 · RT 꺼짐)은 6단계 하늘 조도로 대신한다.
//  ★ 2026-10-28 두 겹 — 가까운 겹(4 m) → 먼 겹(16 m, ±256 m) → 하늘 조도 순서(Probes.hlsli SampleProbeLayers).
//    디버그 뷰 12: 초록 = 가까운 겹 · 파랑 = 먼 겹 · 빨강 = 하늘 조도로 대신.
//
//  ★ 2026-10-27 렌더 8단계 — [2] AO (t13). 간접 확산(주변광)에만 곱한다 — 직접광 · 반사에는 곱하지 않는다.
//    곱하기 전에 GTAO 다중 반사 보정(Jimenez 외 2016 식 10)을 건다: 밝은 표면은 가려진 곳에서도 서로 튕긴 빛이 남는다.
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "Sky.hlsli"
#include "Probes.hlsli"

// GRenderer.cpp 의 LightingConstants 와 같은 순서 · 크기(304바이트 — 4단계에서 112 → 128, 6단계 → 144, 7단계 → 208, 10-28 두 겹 → 304, 지면에 맞춘 축 → 400)
cbuffer LightingCB : register(b0)
{
	float4x4 gInvViewProj;   // 깊이 → 월드 위치 (행벡터 규약: mul(v, M))
	float3   gEyePos;     float _pad0;
	float3   gSunDir;     float _pad1;
	uint2    gSize;       // 화면 픽셀 크기
	uint     gDebugMode;
	uint     gShadowEnabled;       // 1 = 이번 프레임에 [4a] RT 그림자가 돌았다
	uint     gReflectionEnabled;   // 1 = 이번 프레임에 [4b] RT 반사가 돌았다 (4단계)
	uint     gAoEnabled;           // 8단계 — 1 = 이번 프레임에 [2] AO 가 돌았다 (t13 을 읽는다)
	uint2    _pad2;
	float3   gSunIlluminance;      // 6단계 — 땅에 닿는 태양 조도(RGB)
	uint     gSkyEnabled;          // 6단계 — 1 = 하늘 조도 텍스처가 이번 프레임 값이다
	ProbeVolume gProbeVolume;      // 7단계 — [4c] GI 프로브 가까운 겹 (enabled = 0 이면 못 씀)
	ProbeVolume gProbeVolumeFar;   // 2026-10-28 — 먼 겹 (같은 아틀라스 t10 ~ t12 의 뒤쪽)
};

Texture2D<float4>   gAlbedo   : register(t0);   // GB0
Texture2D<float2>   gNormal   : register(t1);   // GB1 (팔면체)
Texture2D<float4>   gMaterial : register(t2);   // GB2
Texture2D<float3>   gEmissive : register(t3);   // GB3
Texture2D<float2>   gMotion   : register(t4);   // GB4 모션 벡터 (5단계 — 디버그 뷰 6 전용)
Texture2D<float>    gDepth    : register(t5);
Texture2D<float4>   gShadow   : register(t6);   // [4a] 그림자 마스크 — rgb = 태양 가시도(투과 물체면 연하게)
Texture2D<float4>   gReflection : register(t7); // [4b]→[4d] 반사 색 (노이즈 제거 후, F 곱하기 전, HDR) — 레이를 안 쏜 픽셀은 0
Texture2D<float4>   gReflectionRaw : register(t8); // 5단계 — 노이즈 제거 전 반사 (디버그 뷰 7 전용)
Texture2D<float4>   gSkyIrradiance : register(t9); // 6단계 — [6] 하늘 조도 / π (법선 방향 등장방형 32×16)
Texture2D<float4>   gProbeIrradiance : register(t10);   // 7단계 — [4c] 프로브 조도 아틀라스
Texture2D<float2>   gProbeDistance   : register(t11);   // 7단계 — [4c] 프로브 거리 아틀라스 (평균 · 거리²)
StructuredBuffer<int4> gProbeStates  : register(t12);   // 7단계 — [4c] 프로브 상태 (월드 칸 · 세대 · 활성)
Texture2D<float>    gAo         : register(t13);  // 8단계 — [2] AO (1 = 안 가려짐)
RWTexture2D<float4> gHdr      : register(u0);
SamplerState        gSkySampler : register(s1);    // 6단계 — 선형 · U 반복 · V 고정 (등장방형)
SamplerState        gClampSampler : register(s2);  // 7단계 — 선형 · 고정 (프로브 아틀라스)

// GTAO 다중 반사 보정 — Jimenez 외, «Practical Real-Time Strategies for Accurate Indirect Occlusion» (2016) 식 10
//   G(A, ρ) = a(ρ)A³ − b(ρ)A² + c(ρ)A,  a = 2.0404ρ − 0.3324,  b = 4.7951ρ − 0.6417,  c = 2.7552ρ + 0.6903
//  (논문 PDF 본문에서 확인). AO 보다 어두워지지 않게 max(A, G) — 보정은 «잃은 빛을 되돌리는» 것이라서.
float3 GtaoMultiBounce(float A, float3 albedo)
{
	const float3 a = 2.0404 * albedo - 0.3324;
	const float3 b = 4.7951 * albedo - 0.6417;
	const float3 c = 2.7552 * albedo + 0.6903;
	return max(A, ((a * A - b) * A + c) * A);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;

	const float depth = gDepth[pixel];
	if (depth >= 1.0) return;   // 메인 패스 물체 없음 — 하늘 · 배경 패스 결과를 건드리지 않는다

	const float3 P        = ReconstructWorldPosition(pixel, depth, gSize, gInvViewProj, gEyePos);
	const float3 albedo   = gAlbedo[pixel].rgb;
	const float3 N        = DecodeNormalOct(gNormal[pixel]);
	const float4 material = gMaterial[pixel];
	const float  rough    = max(material.r, kMinRoughness);   // 8비트 양자화로 바닥값 밑으로 내려가지 않게
	const float  metallic = material.g;
	const float3 emissive = gEmissive[pixel];

	const float3 V = normalize(gEyePos - P);
	// 주변 조도/π: 7단계 GI 프로브(10-28 가까운 겹 → 먼 겹) → (못 쓰면) 6단계 하늘 조도 → (그것도 없으면) 예전 상수
	float  probeWeight = 0.0;
	uint   probeLayer = 2;
	float3 ambientEOverPi = SampleProbeLayers(gProbeIrradiance, gProbeDistance, gProbeStates, gClampSampler,
		gProbeVolume, gProbeVolumeFar, P, N, V, probeWeight, probeLayer);
	if (probeWeight <= 0.0)
		ambientEOverPi = (gSkyEnabled != 0) ? SampleSkyIrradiance(gSkyIrradiance, gSkySampler, N)
		                                    : float3(kAmbient, kAmbient, kAmbient);
	const DirectLighting lit = EvaluateSunAndAmbient(albedo, N, V, rough, metallic, gSunDir, gSunIlluminance, ambientEOverPi);

	// [4a] RT 그림자 — 돌지 않은 프레임이면 1 (그림자 없음)
	const float3 shadow = (gShadowEnabled != 0) ? gShadow[pixel].rgb : float3(1.0, 1.0, 1.0);

	// [4b] RT 반사 — 돌지 않은 프레임이면 0 (반사 없음 = 3단계까지와 같은 화면)
	const float3 reflection = (gReflectionEnabled != 0) ? gReflection[pixel].rgb : float3(0.0, 0.0, 0.0);

	// [2] AO — 8단계. 돌지 않은 프레임이면 1 (가림 없음)
	const float  ao = (gAoEnabled != 0) ? gAo[pixel] : 1.0;
	const float3 aoMultiBounce = GtaoMultiBounce(ao, albedo);

	float3 color = (lit.diffuse + lit.specular) * shadow
	             + lit.ambient * aoMultiBounce
	             + lit.F_env * reflection
	             + emissive;

	// 디버그 뷰 — 번호는 Forward.hlsl PSMain 과 같다.
	switch (gDebugMode)
	{
	case 1:  color = albedo;                         break;
	case 2:  color = N * 0.5 + 0.5;                  break;
	case 3:  color = shadow;                         break;   // [4a] 태양 가시도 (흰 = 빛 받음, 검정 = 그림자)
	case 4:  color = lit.F_env;                      break;
	case 5:  color = reflection;                     break;   // [4d] 노이즈 제거 후 반사 색 (F 곱하기 전)
	case 6:                                                   // 5단계 — 모션 벡터: 픽셀 단위 |x| → R, |y| → G, 8 픽셀 = 1
		color = float3(abs(gMotion[pixel] * float2(gSize)) / 8.0, 0.0);
		break;
	case 7:                                                   // 5단계 — [4b] 노이즈 제거 전 반사 (B 방식 레이 1개 그대로)
		color = (gReflectionEnabled != 0) ? gReflectionRaw[pixel].rgb : float3(0.0, 0.0, 0.0);
		break;
	case 8:  color = lit.specular;                   break;
	case 9:  color = lit.F_env * reflection;         break;   // 반사 몫 (F 곱한 뒤 — 실제로 더해지는 값)
	case 10: color = float3(rough, metallic, 0.0);   break;
	case 11: color = ambientEOverPi;                 break;   // 6단계 — 주변 조도 / π (7단계부터 GI 프로브, 못 쓰면 하늘)
	case 12:                                                  // 7단계 — 프로브를 썼나. 10-28: 초록 가까운 겹 · 파랑 먼 겹 · 빨강 하늘로 대신
		color = (probeLayer == 0) ? float3(0.0, 1.0, 0.0) : (probeLayer == 1) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
		break;
	case 13: color = float3(ao, ao, ao);             break;   // 8단계 — [2] AO (노이즈 제거 후)
	}

	gHdr[pixel] = float4(color, 1.0);
}
