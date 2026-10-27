// ============================================================
//  Lighting.hlsl — [5] 라이팅 합성 (컴퓨트, 2026-10-27 파이프라인 1단계)
//
//  G-buffer(GBuffer.hlsli)를 읽어 픽셀마다 조명을 계산하고 HDR 컬러(u0)에 쓴다.
//  메인 패스 물체가 있는 픽셀(깊이 < 1)만 쓴다 — 나머지는 지우기 색 · 배경 패스 결과가 그대로 남는다.
//
//  ★ 지금은 배경 패스 forward(Forward.hlsl PSMain, RT 꺼짐)와 같은 숫자가 나와야 한다 — 회귀 확인 기준.
//    식은 Shading.hlsli 의 EvaluateSunAndAmbient 하나를 같이 쓴다.
//
//  합성 식 — 구조도의 각 단계가 들어올 자리를 이름으로 남긴다.
//    color = (태양 확산 + 태양 정반사) × shadow   ← [4a] RT 그림자 (아직 없음 → 1)
//          + 앰비언트 확산 × ao                    ← [4c] GI 프로브 · [2] AO (아직 없음 → 상수 앰비언트, 1)
//          + F_env × reflection                    ← [4b] 반사 (아직 없음 → 0)
//          + 자체발광
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
	uint     _pad2;
};

Texture2D<float4>   gAlbedo   : register(t0);   // GB0
Texture2D<float2>   gNormal   : register(t1);   // GB1 (팔면체)
Texture2D<float4>   gMaterial : register(t2);   // GB2
Texture2D<float3>   gEmissive : register(t3);   // GB3
Texture2D<float>    gDepth    : register(t4);
RWTexture2D<float4> gHdr      : register(u0);

// 픽셀 중심 + 깊이 → 월드 위치. 래스터와 같은 D3D 클립 규약(y 위가 +, z 0~1)을 거꾸로 푼다.
float3 ReconstructWorld(uint2 pixel, float depth)
{
	const float2 uv  = (float2(pixel) + 0.5) / float2(gSize);
	const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	const float4 w   = mul(ndc, gInvViewProj);
	return w.xyz / w.w;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;

	const float depth = gDepth[pixel];
	if (depth >= 1.0) return;   // 메인 패스 물체 없음 — 하늘 · 배경 패스 결과를 건드리지 않는다

	const float3 P        = ReconstructWorld(pixel, depth);
	const float3 albedo   = gAlbedo[pixel].rgb;
	const float3 N        = DecodeNormalOct(gNormal[pixel]);
	const float4 material = gMaterial[pixel];
	const float  rough    = max(material.r, kMinRoughness);   // 8비트 양자화로 바닥값 밑으로 내려가지 않게
	const float  metallic = material.g;
	const float3 emissive = gEmissive[pixel];

	const float3 V = normalize(gEyePos - P);
	const DirectLighting lit = EvaluateSunAndAmbient(albedo, N, V, rough, metallic, gSunDir);

	// ── 아직 없는 단계의 자리 — 지금 화면과 같게 두는 값이다 ──
	const float  shadow     = 1.0;                       // [4a] RT 그림자
	const float  ao         = 1.0;                       // [2]  AO
	const float3 reflection = float3(0.0, 0.0, 0.0);     // [4b] 반사

	float3 color = (lit.diffuse + lit.specular) * shadow
	             + lit.ambient * ao
	             + lit.F_env * reflection
	             + emissive;

	// 디버그 뷰 — 번호는 Forward.hlsl PSMain 과 같다.
	//  3 · 5 · 6 · 7 · 9 는 옛 인라인 RT 반사(러시안 룰렛) 전용이라 메인 패스에서는 검정이다.
	switch (gDebugMode)
	{
	case 1:  color = albedo;                         break;
	case 2:  color = N * 0.5 + 0.5;                  break;
	case 4:  color = lit.F_env;                      break;
	case 8:  color = lit.specular;                   break;
	case 10: color = float3(rough, metallic, 0.0);   break;
	case 3: case 5: case 6: case 7: case 9:
		color = float3(0.0, 0.0, 0.0);
		break;
	}

	gHdr[pixel] = float4(color, 1.0);
}
