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
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"

// GRenderer.cpp 의 LightingConstants 와 같은 순서 · 크기(128바이트 — 4단계에서 112 → 128)
cbuffer LightingCB : register(b0)
{
	float4x4 gInvViewProj;   // 깊이 → 월드 위치 (행벡터 규약: mul(v, M))
	float3   gEyePos;     float _pad0;
	float3   gSunDir;     float _pad1;
	uint2    gSize;       // 화면 픽셀 크기
	uint     gDebugMode;
	uint     gShadowEnabled;       // 1 = 이번 프레임에 [4a] RT 그림자가 돌았다
	uint     gReflectionEnabled;   // 1 = 이번 프레임에 [4b] RT 반사가 돌았다 (4단계)
	uint3    _pad2;
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
RWTexture2D<float4> gHdr      : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;

	const float depth = gDepth[pixel];
	if (depth >= 1.0) return;   // 메인 패스 물체 없음 — 하늘 · 배경 패스 결과를 건드리지 않는다

	const float3 P        = ReconstructWorldPosition(pixel, depth, gSize, gInvViewProj);
	const float3 albedo   = gAlbedo[pixel].rgb;
	const float3 N        = DecodeNormalOct(gNormal[pixel]);
	const float4 material = gMaterial[pixel];
	const float  rough    = max(material.r, kMinRoughness);   // 8비트 양자화로 바닥값 밑으로 내려가지 않게
	const float  metallic = material.g;
	const float3 emissive = gEmissive[pixel];

	const float3 V = normalize(gEyePos - P);
	const DirectLighting lit = EvaluateSunAndAmbient(albedo, N, V, rough, metallic, gSunDir);

	// [4a] RT 그림자 — 돌지 않은 프레임이면 1 (그림자 없음)
	const float3 shadow = (gShadowEnabled != 0) ? gShadow[pixel].rgb : float3(1.0, 1.0, 1.0);

	// [4b] RT 반사 — 돌지 않은 프레임이면 0 (반사 없음 = 3단계까지와 같은 화면)
	const float3 reflection = (gReflectionEnabled != 0) ? gReflection[pixel].rgb : float3(0.0, 0.0, 0.0);

	// ── 아직 없는 단계의 자리 — 지금 화면과 같게 두는 값이다 ──
	const float  ao         = 1.0;                       // [2]  AO

	float3 color = (lit.diffuse + lit.specular) * shadow
	             + lit.ambient * ao
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
	}

	gHdr[pixel] = float4(color, 1.0);
}
