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
//  ★ 2026-10-27 렌더 2단계 — shadow 를 그림자 마스크(t5)에서 읽는다.
//    gShadowEnabled = 0 (RT 미지원 · R 키로 끔 · TLAS 비어 있음)이면 마스크를 읽지 않고 1 로 둔다 → 1단계 화면과 같다.
//    월드 위치 되살리기는 GBuffer.hlsli 의 ReconstructWorldPosition 으로 옮겼다(그림자 셰이더와 같이 쓴다).
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"

// GRenderer.cpp 의 LightingConstants 와 같은 순서 · 크기(112바이트)
cbuffer LightingCB : register(b0)
{
	float4x4 gInvViewProj;   // 깊이 → 월드 위치 (행벡터 규약: mul(v, M))
	float3   gEyePos;     float _pad0;
	float3   gSunDir;     float _pad1;
	uint2    gSize;       // 화면 픽셀 크기
	uint     gDebugMode;
	uint     gShadowEnabled;   // 1 = 이번 프레임에 [4a] RT 그림자가 돌았다
};

Texture2D<float4>   gAlbedo   : register(t0);   // GB0
Texture2D<float2>   gNormal   : register(t1);   // GB1 (팔면체)
Texture2D<float4>   gMaterial : register(t2);   // GB2
Texture2D<float3>   gEmissive : register(t3);   // GB3
Texture2D<float>    gDepth    : register(t4);
Texture2D<float4>   gShadow   : register(t5);   // [4a] 그림자 마스크 — rgb = 태양 가시도(투과 물체면 연하게)
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

	// ── 아직 없는 단계의 자리 — 지금 화면과 같게 두는 값이다 ──
	const float  ao         = 1.0;                       // [2]  AO
	const float3 reflection = float3(0.0, 0.0, 0.0);     // [4b] 반사

	float3 color = (lit.diffuse + lit.specular) * shadow
	             + lit.ambient * ao
	             + lit.F_env * reflection
	             + emissive;

	// 디버그 뷰 — 번호는 Forward.hlsl PSMain 과 같다. 5 · 6 · 7 · 9 는 비어 있다(검정).
	switch (gDebugMode)
	{
	case 1:  color = albedo;                         break;
	case 2:  color = N * 0.5 + 0.5;                  break;
	case 3:  color = shadow;                         break;   // [4a] 태양 가시도 (흰 = 빛 받음, 검정 = 그림자)
	case 4:  color = lit.F_env;                      break;
	case 8:  color = lit.specular;                   break;
	case 10: color = float3(rough, metallic, 0.0);   break;
	case 5: case 6: case 7: case 9:
		color = float3(0.0, 0.0, 0.0);
		break;
	}

	gHdr[pixel] = float4(color, 1.0);
}
