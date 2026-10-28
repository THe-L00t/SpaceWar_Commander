// ============================================================
//  RtShadow.hlsl — [4a] RT 그림자 (컴퓨트 · 인라인 RayQuery, 2026-10-27 렌더 파이프라인 2단계)
//
//  G-buffer 픽셀마다 태양 쪽으로 그림자 레이 1개를 쏘고, 남는 빛(RGB 가시도)을 그림자 마스크에 쓴다.
//  [5] Lighting.hlsl 이 태양 확산 · 정반사에 이 값을 곱한다.
//
//  ★ 불투명 물체: BLAS 가 OPAQUE 라 시스템이 바로 확정하고 탐색을 끝낸다(ACCEPT_FIRST_HIT_AND_END_SEARCH) → 가시도 0.
//  ★ 투과 물체(재질 transmission > 0): TLAS 인스턴스가 FORCE_NON_OPAQUE 라 «통과 후보» 로 이 루프에 온다.
//    확정하지 않고 재질 표의 «남는 빛» 을 곱한 뒤 계속 간다 → 연한 그림자(정해진 사항: 빛이 투과되어 연한 그림자).
//    · 물체 하나는 한 번만 곱한다 — 레이가 들어가는 면 · 나오는 면에서 같은 물체가 두 번 오고,
//      DXR 사양은 any-hit 중복 호출을 허용한다(RayQuery 후보에도 같은지는 사양에 없음). 그래서 인스턴스 번호로 걸러낸다.
//    · 곱셈은 순서와 상관없다 — 후보가 오는 순서는 정해져 있지 않다.
//    · 남는 빛이 1 % 밑이면 불투명처럼 확정하고 끝낸다.
//  ★ 알파 마스크 물체(2026-10-28, glTF alphaMode MASK): 인스턴스가 FORCE_NON_OPAQUE 라 후보로 온다.
//    잎 · 풀 사이 빈 곳(baseColor 알파 < alphaCutoff)이면 지나가고, 있는 곳이면 불투명으로 확정한다(RtCommon.hlsli).
//  ★ 2026-10-28 — 알파 판정에 재질 텍스처 · 정점이 필요해 cs_6_5 · 테이블 방식에서 cs_6_6 · 컴퓨트 공용 바인드리스 루트로 옮겼다.
//    법선 · 깊이 · 그림자 마스크는 바인드리스 번호(ShadowCB)로 읽고 쓴다. RT 미지원 장치에서는 만들지 않는다.
// ============================================================

#include "GBuffer.hlsli"
#include "RtCommon.hlsli"   // RtMaterial · RtInstance · TraceSunVisibility · AlphaMaskedOut

// GRenderer.cpp 의 ShadowConstants 와 같은 배치 (128바이트)
cbuffer ShadowCB : register(b0)
{
	float4x4 gInvViewProj;
	float3   gEyePos;     float gMaxDistance;   // 레이 길이 상한(m)
	float3   gSunDir;     float _pad0;          // 빛이 나아가는 방향 — 레이는 그 반대(-gSunDir)로 쏜다
	uint2    gSize;       uint  gNormalIndex;   uint gDepthIndex;   // ↓ 바인드리스 힙 번호
	uint     gShadowUav;  uint3 _pad1;
};

// 컴퓨트 공용 루트 — t0 TLAS · t1 재질 표 · t2 인스턴스 표 (루트 SRV), s0 선형 반복
RaytracingAccelerationStructure gScene       : register(t0);
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);
StructuredBuffer<RtInstance>    gRtInstances : register(t2);
SamplerState                    gWrapSampler : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;

	Texture2D<float2>   normalTex = ResourceDescriptorHeap[gNormalIndex];   // GB1 (팔면체 법선)
	Texture2D<float>    depthTex  = ResourceDescriptorHeap[gDepthIndex];
	RWTexture2D<float4> shadow    = ResourceDescriptorHeap[gShadowUav];     // rgb = 가시도

	const float depth = depthTex[pixel];
	if (depth >= 1.0)
	{
		shadow[pixel] = float4(1.0, 1.0, 1.0, 1.0);   // 메인 패스 물체 없음 — 라이팅도 이 픽셀을 건너뛴다
		return;
	}

	const float3 P = ReconstructWorldPosition(pixel, depth, gSize, gInvViewProj);
	const float3 N = DecodeNormalOct(normalTex[pixel]);
	const float3 L = normalize(-gSunDir);
	if (dot(N, L) <= 0.0)
	{
		shadow[pixel] = float4(0.0, 0.0, 0.0, 1.0);   // 해를 등진 면 — 직접광이 어차피 0 이라 레이를 아낀다
		return;
	}

	// 깊이에서 되살린 위치는 멀수록 오차가 커서 레이 시작점을 법선 쪽으로 거리에 맞춰 띄운다(RtCommon.hlsli RayOffsetBias).
	// 안 띄우면 자기 표면에 맞아 줄무늬 그림자가 생긴다.
	const float  bias       = RayOffsetBias(length(P - gEyePos));
	const float3 visibility = TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gWrapSampler, P + N * bias, L, gMaxDistance);
	shadow[pixel] = float4(visibility, 1.0);
}
