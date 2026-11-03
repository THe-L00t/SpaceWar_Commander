// ============================================================
//  Forward.hlsl — 표면 셰이더 (정점 · 재질 샘플링 공용, 픽셀 진입점 둘)
//
//  ★ 2026-10-27 파이프라인 1단계 — 진입점이 둘이 됐다
//    PSGBuffer : 메인 패스. [1] G-buffer 에 표면 정보만 쓴다. 조명은 Lighting.hlsl([5])이 컴퓨트로 한다.
//    PSMain    : 배경 패스(먼 행성). forward 로 HDR 컬러에 바로 그린다.
//                배경 패스는 자기 투영(넓은 near/far)을 쓰고 끝나면 깊이를 지우므로
//                메인 G-buffer 와 깊이를 섞을 수 없다 — 그래서 forward 로 남겨 둔다.
//    BRDF · 임시 광원 상수는 Shading.hlsli, G-buffer 배치는 GBuffer.hlsli 로 옮겼다.
//
//  ★ 2026-10-27 파이프라인 2단계 — RT 코드를 뺐다
//    예전 PSMain 은 «픽셀 셰이더 안 인라인 RayQuery 반사 1개 + 러시안 룰렛(FGPS)» 을 했다.
//    1단계 뒤로 메인 패스는 PSGBuffer 를 쓰고 배경 패스는 RT 를 쓰지 않아서, 그 코드는 한 번도 돌지 않았다.
//    RT 는 이제 G-buffer 뒤 단계가 맡는다 — [4a] 그림자는 RtShadow.hlsl, [4b] 반사는 RtReflection.hlsl(4단계).
//    그래서 이 파일에는 RayQuery · TLAS · RT_SUPPORTED 가 없고 항상 SM 6.0 으로 컴파일한다.
//
//  ★ 2026-10-27 렌더 5단계 — 모션 벡터
//    VSMain 이 같은 정점의 «이전 프레임 클립 좌표»(prevWorld × gPrevViewProj)를 같이 넘기고,
//    PSGBuffer 가 두 클립 좌표의 화면 UV 차이를 GB4 에 쓴다. [4d] 반사 노이즈 제거의 시간 누적이 이걸로 이전 프레임 결과를 찾는다.
//    배경 패스(PSMain)는 G-buffer 를 안 쓰므로 모션 벡터도 없다.
//
//  ★ 2026-09-26 재질 도입 (client2 `46567fd` 이식)
//    roughness/metallic 이 더 이상 자리표시자 상수가 아니다. 재질 상수(b2)와
//    텍스처 5장(t1~t5)에서 온다. 텍스처가 없는 재질에는 렌더러가 흰색·평면 노멀
//    폴백을 묶어 주므로, 셰이더는 항상 샘플해도 된다(분기 없음).
//    지형·더미 메시는 0번 기본 재질(roughness 0.10, 흰 폴백)을 쓰므로
//    이 변경 전과 같은 화면이 나와야 한다 — 회귀 확인 기준이다.
// ============================================================

//  ★ 2026-10-28 지형 레이어 — 세 번째 판(TERRAIN_LAYERS=1)
//    예시 행성 지형(레이어 4종 + LayerMask)을 섞어 그린다. 재질 상수 · 텍스처 5칸을 다른 뜻으로 읽는다(아래 TerrainCB).
//    GRenderer 가 PSMain · PSGBuffer 를 TERRAIN_LAYERS=1 로 한 번 더 컴파일해 지형 재질(CreateTerrainMaterial)에만 쓴다.
//    정점 셰이더는 모든 판이 같다 — VOut 에 메시 좌표(마스크 UV 계산용)가 늘 들어 있다.

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "TerrainLayers.hlsli"

// GRenderer.cpp 의 FrameConstants 와 같은 배치 (176바이트 — 5단계에서 96 → 160, 6단계에서 → 176)
cbuffer FrameCB : register(b0)
{
	float4x4 gViewProj;
	float3   gEyePos;        float _pad0;
	float3   gSunDir;        uint  gDebugMode;
	float4x4 gPrevViewProj;  // 5단계 — 이전 프레임 viewProj (모션 벡터용). 첫 프레임 · 배경 패스는 gViewProj 와 같다
	float3   gSunIlluminance; float _pad1;   // 6단계 — 땅에 닿는 태양 조도(RGB, 대기 투과 포함). 배경 패스 PSMain 이 쓴다
};

// ── 인스턴스 (수정방향 3.2, 2026-10-11) ──
//  같은 (PSO, 재질, 메시)를 쓰는 인스턴스를 ExecuteIndirect 한 번으로 그린다.
//  인스턴스 수는 Cull.hlsl 이 보이는 것만 세어 간접 인자에 넣었다.
//  gVisible[gInstanceBase + SV_InstanceID] = 그 묶음에서 보이는 인스턴스 번호.
//  ★ row_major — CPU 의 XMFLOAT4X4 를 전치 없이 그대로 올린다. mul(v, M) 행벡터 규약은 예전과 같다.
cbuffer DrawCB : register(b1)
{
	uint gInstanceBase;      // 이 묶음의 보이는 목록 시작 번호 (루트 상수 1개)
};

struct InstanceGpu           // GRenderer 의 Impl::InstanceGpu 와 같은 128바이트
{
	row_major float4x4 world;
	row_major float4x4 prevWorld;   // 이전 프레임 world — 5단계부터 VSMain 이 모션 벡터에 쓴다
};
StructuredBuffer<InstanceGpu> gInstances : register(t6);
StructuredBuffer<uint>        gVisible   : register(t7);

// 2026-10-28 알파 마스크 — GRenderer 가 PSMain · PSGBuffer 를 ALPHA_MASK=1 로 한 번 더 컴파일해 마스크 재질에만 쓴다.
#ifndef ALPHA_MASK
#define ALPHA_MASK 0
#endif
// 2026-10-28 지형 레이어 판 (위 머리말)
#ifndef TERRAIN_LAYERS
#define TERRAIN_LAYERS 0
#endif

#if TERRAIN_LAYERS
// ── 지형 재질 (GRenderer 의 TerrainConstants 와 같은 배치 — 재질 루트 상수 자리 b2, 4단계에서 12 → 20 DWORD) ──
cbuffer TerrainCB : register(b2)
{
	float4 gHeightBlend;                       // 레이어(마스크 R G B A)마다 Height 에 곱하는 값 (시안 0.5, 빙하 눈 0.35)
	float  gTriTileA;      float gTriTileB;     // 3단계 — 세 방향 투영 크기 A · B (m)
	float  gWarpScale;     float gBlendDepth;   // 3단계 — 좌표 비틀기 노이즈 규모(m, 0 = 안 함) · 경계 폭(시안 0.2)
	float  gNormalStrength;
	uint   gTriLayers;                          // 3단계 — 세 방향 투영 레이어 비트(R 1 · G 2 · B 4 · A 8)
	float  gWarpAmount;                         // 3단계 — 좌표 비틀기 최대 거리(m)
	float  gTriOffsetB;                         // 3단계 — 1 = 크기 B 좌표에 (3.7, 1.9, 5.3)
	float  gMacroAmount;   float gMacroBias;    // 4단계 — 큰 규모 색 흔들림: 노이즈 × amount + bias
	float  gBumpDistance;                       // 4단계 — 범프 1(레이어 높이맵) Distance(m)
	float  gReliefDistance;                     // 4단계 — 범프 2(지면 요철) Distance(m)
	float  gReliefStrength;                     // 4단계 — 범프 2 Strength
	float3 gTerrainOrigin;                      // 2026-10-29 — 레이어 좌표의 원점 = 월드의 행성 중심(예전 패딩 자리)
};

// 텍스처 5칸 = 같은 종류 4장(마스크 R G B A 순)을 묶은 배열 4개 + 마스크. 슬롯 번호는 일반 재질과 같은 자리다.
Texture2DArray        gLayerBaseColor : register(t1);   // sRGB
Texture2DArray        gLayerNormal    : register(t2);   // 탄젠트 공간, OpenGL(+Y)
Texture2DArray        gLayerOrm       : register(t3);   // R = AO · G = 거칠기 · B = 금속(0)
Texture2DArray<float> gLayerHeight    : register(t4);   // 0 ~ 1
Texture2D             gLayerMask      : register(t5);   // RGBA = 레이어 4개 비율(합 1)
#else
// ── 재질 (GRenderer 의 MaterialConstants 와 16바이트 단위로 같아야 한다) ──
cbuffer MaterialCB : register(b2)
{
	float4 gBaseColor;
	float3 gEmissive;      float gRoughness;
	float  gMetallic;      uint  gHasNormalMap;   float gAlphaCutoff;   float _pad3;   // 2026-10-28 — 알파 마스크 기준(0 = 마스크 아님)
};

// 슬롯 순서는 ModelData.h 의 TextureSlot 과 같다.
Texture2D gBaseColorMap : register(t1);
Texture2D gNormalMap    : register(t2);
Texture2D gRoughnessMap : register(t3);
Texture2D gMetallicMap  : register(t4);
Texture2D gEmissiveMap  : register(t5);
#endif

SamplerState gSampler      : register(s0);   // 루트 시그니처의 정적 샘플러 (이방성 16 · WRAP — 2026-10-28, 전에는 선형)
SamplerState gClampSampler : register(s1);   // 2026-10-28 — 선형 · 가장자리 고정 (지형 레이어 마스크)

struct VIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float3 col : COLOR;
	float2 uv  : TEXCOORD;
	float4 tan : TANGENT;      // xyz = 접선, w = 종법선 부호
};

struct VOut
{
	float4 pos   : SV_POSITION;
	float3 world : WORLDPOS;
	float3 nrm   : NORMAL;
	float3 col   : COLOR;
	float2 uv    : TEXCOORD;
	float4 tan   : TANGENT;
	// 5단계 — 모션 벡터용 클립 좌표 두 개. 나누기(w)는 픽셀 셰이더에서 한다(보간 뒤에 나눠야 원근이 맞다).
	float4 curClip  : CURCLIP;
	float4 prevClip : PREVCLIP;
	// (2026-10-29 — 지형 레이어 판이 쓰던 메시 좌표 MESHPOS 를 뺐다. 레이어 좌표는 world − gTerrainOrigin 이다)
};

VOut VSMain(VIn i, uint instance : SV_InstanceID)
{
	const InstanceGpu inst = gInstances[gVisible[gInstanceBase + instance]];
	const float4x4 world = inst.world;
	VOut o;
	float4 wp = mul(float4(i.pos, 1.0), world);
	o.world = wp.xyz;
	// ★ 2026-10-29 — 카메라 기준으로 곱한다: gViewProj 는 «T(eye) × viewProj»(GRenderer.cpp CameraRelativeViewProj).
	//   월드 viewProj 를 그대로 곱하면 카메라가 원점에서 멀 때(예시 행성 수백 m ~ 1 km) 큰 값끼리의 float 뺄셈으로
	//   깊이 버퍼의 깊이가 거리에 비례해 어긋났다 → 그 깊이로 되살린 위치에서 쏜 그림자 레이가 자기 땅에 맞아 검은 점 · 깜빡임.
	o.pos   = mul(float4(wp.xyz - gEyePos, 1.0), gViewProj);
	// ★ 5단계 — 같은 정점의 이전 프레임 위치: 이전 world(물체 움직임) × 이전 viewProj(카메라 움직임)
	//   (2026-10-29 — 이전 viewProj 도 «이번 카메라 기준» 이라 같은 eye 를 뺀다)
	o.curClip  = o.pos;
	o.prevClip = mul(float4(mul(float4(i.pos, 1.0), inst.prevWorld).xyz - gEyePos, 1.0), gPrevViewProj);
	o.nrm   = mul(float4(i.nrm, 0.0), world).xyz;
	o.col   = i.col;
	o.uv    = i.uv;
	// 접선도 월드로 옮긴다. 부호(w)는 그대로 넘긴다.
	o.tan   = float4(mul(float4(i.tan.xyz, 0.0), world).xyz, i.tan.w);
	return o;
}

// 노멀맵을 월드 법선으로 바꾼다.
//
// ★ 녹색 채널을 뒤집는 이유
//   Meshy/Blender 는 +Y(OpenGL) 규약으로 노멀맵을 굽고, 로더는 UV 의 V 를 뒤집어 읽는다.
//   V 를 뒤집으면 접선 공간의 Y 축도 뒤집히므로 녹색을 그대로 쓰면 요철이 반대로 보인다.
//
// ★★ 반전은 여기 한 곳에서만 한다 (2026-10-07)
//   로더에서 픽셀의 녹색을 또 뒤집으면 두 번이 되어 반전이 상쇄된다. 실제로 그랬다 —
//   FbxModelLoader 가 로드 때 255-G 를 하고 있었고, 그래서 FBX 모델의 요철이 반대로 보였다.
//   지금은 FBX 경로와 자체 파서 경로(ModelBuilder) 둘 다 픽셀을 건드리지 않는다.
//   ※ -Y(DirectX) 규약 원본을 받게 되면 이 줄을 지우는 대신 재질 플래그로 가려야 한다.
//   (2026-10-28 지형 레이어 판도 같은 규약 — 섞은 노멀에 같은 반전을 한 번 한다)
//
// 탄젠트 공간 노멀(녹색 반전을 이미 한 값) → 월드 법선. 일반 판 · 지형 판이 같이 쓴다(2026-10-28 나눔).
float3 TangentToWorld(float3 N, float4 tangent, float3 nTex)
{
	float3 T = tangent.xyz - N * dot(N, tangent.xyz);   // 그람-슈미트 직교화
	if (dot(T, T) < 1e-8) return N;                     // 접선이 퇴화했다 — 기하 법선을 쓴다

	T = normalize(T);
	float3 B = normalize(cross(N, T) * tangent.w);

	return normalize(T * nTex.x + B * nTex.y + N * nTex.z);
}

#if !TERRAIN_LAYERS
float3 ApplyNormalMap(float3 N, float4 tangent, float2 uv)
{
	float3 nTex = gNormalMap.Sample(gSampler, uv).rgb * 2.0 - 1.0;
	nTex.y = -nTex.y;
	return TangentToWorld(N, tangent, nTex);
}
#endif

// ── 재질 샘플링 (2026-10-27) ──
//  PSMain(배경 forward)과 PSGBuffer(메인 G-buffer)가 같은 표면 값을 써야 하므로 한 함수로 묶었다.
//  텍스처가 없는 재질에는 흰색·평면 노멀 폴백이 묶여 있어 분기 없이 곱해도 된다.
struct Surface
{
	float3 albedo;
	float3 N;          // 노멀맵까지 적용한 월드 법선 (뒷면이면 뒤집음)
	float  rough;      // kMinRoughness 이상으로 자른 값
	float  metallic;
	float3 emissive;
};

#if TERRAIN_LAYERS
// ── 지형 레이어 표면 (2026-10-28) ──
//  블렌더 시안(Resource_Work/<행성>/scripts/build_map.py draft_terrain_material)의 1 · 2 · 3 · 4단계.
//  표면 아래 산란(빙하 얼음 · 눈)은 아직 없다(노션 10-28 작업기록 12 ~ 15절).
//  ① 마스크: UV 를 메시 좌표 방향으로 계산한다. 팔면체 접힘 선에서 UV 가 튀어 하드웨어 미분(밉 선택)이 깨지므로
//     밉은 «화면 픽셀 하나가 덮는 표면 길이 ÷ 마스크 텍셀 한 변» 으로 직접 고른다(SampleLevel).
//  ② 높이 섞기(TerrainLayerWeights). 가중치가 0 인 레이어는 색 · ORM · 노멀을 읽지 않는다(Unity HDRP TerrainLit ·
//     Frostbite 2007 과 같은 동적 분기). 마스크만으로 이길 수 없는 레이어는 높이도 읽지 않는다(아래 canWin).
//     분기 안에서는 하드웨어 미분을 쓸 수 없어 좌표 미분을 밖에서 구해 SampleGrad 로 넘긴다.
//  ③ 반복 숨김(3단계): 레이어마다 크기가 다른 두 벌을 노이즈로 섞는다 — 일반 레이어는 UV 두 벌, 바위 레이어는 메시 좌표의
//     세 방향 투영(블렌더 BOX) 두 크기. 높이 · 색 · ORM · 노멀 모두 같은 섞기를 거친다(시안 sample()).
//     → 1 · 2단계에서 경계 지대가 4 m 마다 같은 얼룩으로 바뀌던 바둑판이 노이즈를 따라 흐트러진다.
//  섞는 값 = 색 · 거칠기(ORM.g) · AO(ORM.r) · 높이 · 탄젠트 공간 노멀(세 방향 투영 레이어는 평평 — 시안과 같음).
//  금속성 0(ORM.b 가 0 인 지형 규격), 발광 0.
//  ④ 4단계(시안 그대로): 색 × 큰 규모 흔들림(약 83 m) × AO 배율(AO × 0.55 + 0.45) × 바위 흔들림(바위 가중치만큼),
//     법선 = 노멀맵 → 범프 1(섞은 높이, 바위는 4배) → 범프 2(지면 요철 노이즈). 범프는 블렌더 Bump 노드 식(TerrainBump).
//     AO 는 G-buffer 에 자리가 없어, 시안처럼 색에 곱하는 것으로만 쓴다(일반 재질은 여전히 안 쓴다).

// 한 픽셀의 레이어 좌표 — 두 크기 UV · 세 방향 투영 좌표와 그 미분, 섞는 비율
struct TerrainCoords
{
	float  anti;                      // 크기 B 비율(0 = A 만, 1 = B 만)
	float2 uvA, uvB;
	float2 dxA, dyA, dxB, dyB;
	float3 boxA, boxB;                // 세 방향 투영 좌표(블렌더 축 · 타일 단위)
	float3 dBoxAx, dBoxAy, dBoxBx, dBoxBy;
	float3 boxN;                      // 블렌더 축 법선(면마다 좌우 뒤집기 부호)
	float3 boxWeight;                 // 면 가중치(X · Y · Z)
};

// BOX 투영 한 크기 — 면마다 Blender svm_node_tex_image_box 의 좌표 규칙(부호에 따라 좌우 뒤집기)
float4 SampleBox(Texture2DArray tex, float layer, float3 co, float3 dcx, float3 dcy, TerrainCoords c)
{
	float4 r = float4(0.0, 0.0, 0.0, 0.0);
	[branch] if (c.boxWeight.x > 0.0)
	{
		const float s = c.boxN.x < 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.x * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.y : co.y, co.z, layer),
			float2(s * dcx.y, dcx.z), float2(s * dcy.y, dcy.z));
	}
	[branch] if (c.boxWeight.y > 0.0)
	{
		const float s = c.boxN.y > 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.y * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.x : co.x, co.z, layer),
			float2(s * dcx.x, dcx.z), float2(s * dcy.x, dcy.z));
	}
	[branch] if (c.boxWeight.z > 0.0)
	{
		const float s = c.boxN.z > 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.z * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.y : co.y, co.x, layer),
			float2(s * dcx.y, dcx.x), float2(s * dcy.y, dcy.x));
	}
	return r;
}
float SampleBoxHeight(Texture2DArray<float> tex, float layer, float3 co, float3 dcx, float3 dcy, TerrainCoords c)
{
	float r = 0.0;
	[branch] if (c.boxWeight.x > 0.0)
	{
		const float s = c.boxN.x < 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.x * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.y : co.y, co.z, layer),
			float2(s * dcx.y, dcx.z), float2(s * dcy.y, dcy.z));
	}
	[branch] if (c.boxWeight.y > 0.0)
	{
		const float s = c.boxN.y > 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.y * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.x : co.x, co.z, layer),
			float2(s * dcx.x, dcx.z), float2(s * dcy.x, dcy.z));
	}
	[branch] if (c.boxWeight.z > 0.0)
	{
		const float s = c.boxN.z > 0.0 ? -1.0 : 1.0;
		r += c.boxWeight.z * tex.SampleGrad(gSampler, float3(s < 0.0 ? 1.0 - co.y : co.y, co.x, layer),
			float2(s * dcx.y, dcx.x), float2(s * dcy.y, dcy.x));
	}
	return r;
}

// 레이어 하나를 읽는다 — 두 크기를 c.anti 로 섞는다. triplanar 면 BOX 투영, 아니면 UV.
float4 SampleLayer(Texture2DArray tex, uint layer, bool triplanar, TerrainCoords c)
{
	float4 r = float4(0.0, 0.0, 0.0, 0.0);
	[branch] if (c.anti < 1.0)
		r += (1.0 - c.anti) * (triplanar ? SampleBox(tex, float(layer), c.boxA, c.dBoxAx, c.dBoxAy, c)
		                                 : tex.SampleGrad(gSampler, float3(c.uvA, float(layer)), c.dxA, c.dyA));
	[branch] if (c.anti > 0.0)
		r += c.anti * (triplanar ? SampleBox(tex, float(layer), c.boxB, c.dBoxBx, c.dBoxBy, c)
		                         : tex.SampleGrad(gSampler, float3(c.uvB, float(layer)), c.dxB, c.dyB));
	return r;
}
float SampleLayerHeight(uint layer, bool triplanar, TerrainCoords c)
{
	float r = 0.0;
	[branch] if (c.anti < 1.0)
		r += (1.0 - c.anti) * (triplanar ? SampleBoxHeight(gLayerHeight, float(layer), c.boxA, c.dBoxAx, c.dBoxAy, c)
		                                 : gLayerHeight.SampleGrad(gSampler, float3(c.uvA, float(layer)), c.dxA, c.dyA));
	[branch] if (c.anti > 0.0)
		r += c.anti * (triplanar ? SampleBoxHeight(gLayerHeight, float(layer), c.boxB, c.dBoxBx, c.dBoxBy, c)
		                         : gLayerHeight.SampleGrad(gSampler, float3(c.uvB, float(layer)), c.dxB, c.dyB));
	return r;
}

Surface SampleSurface(VOut i, bool frontFace)
{
	// 레이어 좌표 = 행성 중심 기준 위치(2026-10-29 — 예전 메시 좌표). 지형은 둘이 같고, 노드 변환이 있는
	//  오솔길 · 착륙 대지(리소스 규격: 지형과 같은 레이어 셰이더)는 월드 쪽이라야 지형과 같은 마스크 · 무늬를 읽는다.
	//  블렌더 시안도 착륙 원판의 마스크 UV 를 월드 위치로 계산한다(Verdant build_map.py 320 ~ 331줄).
	const float3 meshPos = i.world - gTerrainOrigin;
	uint maskWidth, maskHeight, maskMips;
	gLayerMask.GetDimensions(0, maskWidth, maskHeight, maskMips);
	// 등면적 매핑이라 마스크 텍셀 넓이 = 4πR² ÷ 픽셀 수 → 한 변 = R·√(4π) ÷ 폭
	const float texelSize = length(meshPos) * 3.5449077 / float(maskWidth);
	const float footprint = max(length(ddx(i.world)), length(ddy(i.world)));   // 화면 픽셀 하나가 덮는 표면 길이(m)
	const float maskLod   = clamp(log2(max(footprint / texelSize, 1e-4)), 0.0, float(maskMips - 1));
	const float4 mask     = gLayerMask.SampleLevel(gClampSampler, TerrainMaskUv(meshPos), maskLod);

	// ── 3단계 좌표 ──
	//  시안 노이즈 · 세 방향 투영은 블렌더 물체 좌표(Z 위)다 — glTF (x, y, z) → 블렌더 (x, −z, y) 로 바꿔 같은 면 · 방향을 쓴다.
	const float3 co = float3(meshPos.x, -meshPos.z, meshPos.y);
	TerrainCoords c;
	c.anti = saturate(TerrainFbm(co * kTerrainAntiScale, kTerrainNoiseOctave, footprint * kTerrainAntiScale) * 3.0 - 1.0);
	c.uvA  = i.uv;
	c.uvB  = i.uv * kTerrainUvBScale + kTerrainUvBOffset;
	c.dxA  = ddx(c.uvA);  c.dyA = ddy(c.uvA);
	c.dxB  = c.dxA * kTerrainUvBScale;  c.dyB = c.dyA * kTerrainUvBScale;
	float3 boxCo = co;
	if (gWarpScale > 0.0)   // 빙하 시안: 70 m 규모 노이즈로 최대 10 m 민다(지형 UV0 의 uv_warp 와 같은 뜻)
		boxCo += (TerrainFbm3(co / gWarpScale, kTerrainNoiseOctave, footprint / gWarpScale) - 0.5) * (2.0 * gWarpAmount);
	c.boxA = boxCo / gTriTileA;
	c.boxB = boxCo / gTriTileB + float3(3.7, 1.9, 5.3) * gTriOffsetB;
	c.dBoxAx = ddx(c.boxA);  c.dBoxAy = ddy(c.boxA);
	c.dBoxBx = ddx(c.boxB);  c.dBoxBy = ddy(c.boxB);
	const float3 nGeo = normalize(i.nrm);
	c.boxN      = float3(nGeo.x, -nGeo.z, nGeo.y);
	c.boxWeight = TerrainBoxWeights(c.boxN, kTerrainBoxBlend);

	// ── 높이 → 가중치 ──
	//  마스크만으로 이길 수 없는 레이어(마스크 + 높이 계수 ≤ 가장 큰 마스크 − 경계 폭)는 높이를 읽지 않는다.
	//  그 레이어의 점수는 문턱보다 늘 낮아 가중치가 0 이기 때문이다(높이를 0 으로 둬도 결과가 같다).
	const float maxMask = max(max(mask.x, mask.y), max(mask.z, mask.w));
	float4 height = float4(0.0, 0.0, 0.0, 0.0);
	[unroll] for (uint h = 0; h < 4; ++h)
	{
		[branch] if (mask[h] + gHeightBlend[h] > maxMask - gBlendDepth)
			height[h] = SampleLayerHeight(h, ((gTriLayers >> h) & 1u) != 0u, c);
	}
	const float4 w = TerrainLayerWeights(mask, height, gHeightBlend, gBlendDepth);

	float3 albedo = float3(0.0, 0.0, 0.0);
	float3 nTex   = float3(0.0, 0.0, 0.0);
	float  rough  = 0.0;
	float  ao     = 0.0;
	[unroll] for (uint k = 0; k < 4; ++k)
	{
		[branch] if (w[k] > 0.0)
		{
			const bool triplanar = ((gTriLayers >> k) & 1u) != 0u;
			albedo += w[k] * SampleLayer(gLayerBaseColor, k, triplanar, c).rgb;
			const float4 orm = SampleLayer(gLayerOrm, k, triplanar, c);
			rough  += w[k] * orm.g;
			ao     += w[k] * orm.r;   // 4단계 — 색에 곱한다(아래)
			// 세 방향 투영 레이어는 노멀맵 대신 평평한 노멀(시안 «(0.5, 0.5, 1.0) if tri»)
			nTex   += w[k] * (triplanar ? float3(0.0, 0.0, 1.0) : SampleLayer(gLayerNormal, k, false, c).rgb * 2.0 - 1.0);
		}
	}

	Surface s;
	// ── 4단계 색 흔들림 — 시안 «col × macro × aof × var» ──
	s.albedo   = albedo * TerrainColorScale(co, footprint, ao, w[kTerrainRockLayer], gMacroAmount, gMacroBias) * i.col;
	s.rough    = clamp(rough, kMinRoughness, 1.0);
	s.metallic = 0.0;
	s.emissive = float3(0.0, 0.0, 0.0);
	s.N = normalize(i.nrm);
	if (!frontFace) s.N = -s.N;
	// 섞은 탄젠트 공간 노멀 — 녹색 반전(ApplyNormalMap 과 같은 규약) 뒤 기울기에 노멀 세기를 곱한다.
	//  시안 Normal Map 노드의 Strength 를 «기울기(xy) 배율» 로 근사한 것이다.
	nTex.y  = -nTex.y;
	nTex.xy *= gNormalStrength;
	s.N = TangentToWorld(s.N, i.tan, nTex);

	// ── 4단계 범프 2단 — 시안: 노멀맵 → bump1(섞은 높이 × (바위 × 3 + 1)) → bump2(지면 요철) ──
	//  높이 변화는 화면 미분(ddx_fine · ddy_fine — 2×2 묶음 안에서도 픽셀 쌍마다)으로 잰다. 블렌더도 픽셀 한 칸 옆의 높이를
	//  다시 계산해 차이를 쓴다(Bump 노드). 가중치가 0 인 레이어는 높이를 안 읽었지만 섞은 높이에 0 을 더하므로 같다.
	const float3 dPdx = ddx_fine(i.world);
	const float3 dPdy = ddy_fine(i.world);
	const float  bumpHeight = dot(w, height) * (w[kTerrainRockLayer] * kTerrainRockBumpGain + 1.0);
	s.N = TerrainBump(s.N, dPdx, dPdy, ddx_fine(bumpHeight), ddy_fine(bumpHeight), gBumpDistance, 1.0);
	const float  relief = TerrainRelief(co, footprint);
	s.N = TerrainBump(s.N, dPdx, dPdy, ddx_fine(relief), ddy_fine(relief), gReliefDistance, gReliefStrength);
	return s;
}
#else
Surface SampleSurface(VOut i, bool frontFace)
{
	Surface s;
	float4 baseTex = gBaseColorMap.Sample(gSampler, i.uv);
#if ALPHA_MASK
	// glTF alphaMode MASK — baseColor 알파(텍스처 a × 계수 a)가 기준보다 작으면 그 픽셀은 없다:
	//  깊이 · G-buffer 에 아무것도 안 쓰므로 뒤 패스(AO 화면 추적 · 반사 분류 · 라이팅)도 없는 것으로 본다.
	if (baseTex.a * gBaseColor.a < gAlphaCutoff)
		discard;
#endif
	s.albedo   = baseTex.rgb * gBaseColor.rgb * i.col;
	// ★ 2026-10-28 — glTF metallicRoughness 텍스처는 거칠기 = G, 금속성 = B 다(R 은 차폐 — ORM 묶음).
	//   로더는 같은 이미지를 거칠기 · 금속성 두 슬롯에 건다. 전에는 둘 다 R(차폐)을 읽어 비금속 땅이 금속으로 그려졌다.
	//   따로 된 흑백 맵(OBJ map_Pr · map_Pm)은 세 채널이 같아 그대로 맞다. RtSurface.hlsli · Transparent.hlsl 도 같다.
	s.rough    = clamp(gRoughness * gRoughnessMap.Sample(gSampler, i.uv).g, kMinRoughness, 1.0);
	s.metallic = saturate(gMetallic * gMetallicMap.Sample(gSampler, i.uv).b);
	s.emissive = gEmissive * gEmissiveMap.Sample(gSampler, i.uv).rgb;

	s.N = normalize(i.nrm);
	if (!frontFace) s.N = -s.N;
	if (gHasNormalMap != 0)
		s.N = ApplyNormalMap(s.N, i.tan, i.uv);
	return s;
}
#endif

// ── [1] G-buffer (메인 패스) ──
//  조명 계산 없이 표면 정보만 쓴다. 배치는 GBuffer.hlsli.
GBufferOut PSGBuffer(VOut i, bool frontFace : SV_IsFrontFace)
{
	const Surface s = SampleSurface(i, frontFace);
	// 5단계 — GB4 모션 벡터 = 이전 UV − 지금 UV (카메라 · 물체 움직임 둘 다 들어 있다)
	const float2 motion = ClipToUv(i.prevClip) - ClipToUv(i.curClip);
	return PackGBuffer(s.albedo, s.N, s.rough, s.metallic, s.emissive, motion);
}

// ── 배경 패스 (forward) ──
//  메인 패스의 Lighting.hlsl 과 같은 식(Shading.hlsli)으로 태양 · 앰비언트만 계산한다.
//  배경(먼 행성)에는 RT 그림자 · 반사를 하지 않는다.
//  ★ 6단계 — 태양은 렌더러가 넘긴 조도(gSunIlluminance). 주변광은 아직 상수(kAmbient) —
//    배경 패스의 루트 시그니처는 재질 테이블 힙만 묶어서 하늘 조도 텍스처를 못 읽는다(미정: 배경 행성 주변광).
float4 PSMain(VOut i, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
	const Surface s = SampleSurface(i, frontFace);
	const float3 V = normalize(gEyePos - i.world);
	const DirectLighting lit = EvaluateSunAndAmbient(s.albedo, s.N, V, s.rough, s.metallic, gSunDir,
		gSunIlluminance, float3(kAmbient, kAmbient, kAmbient));

	float3 color = lit.diffuse + lit.specular + lit.ambient + s.emissive;

	// 디버그 뷰 — 번호는 Lighting.hlsl 과 같다.
	switch (gDebugMode)
	{
	case 1:  color = s.albedo;                         break;   // 알베도
	case 2:  color = s.N * 0.5 + 0.5;                  break;   // 월드 법선 (노멀맵 적용 후)
	case 3:  color = float3(1.0, 1.0, 1.0);            break;   // 그림자 가시도 — 배경은 그림자를 안 구하므로 1
	case 4:  color = lit.F_env;                        break;   // 환경 프레넬
	case 8:  color = lit.specular;                     break;   // 태양 하이라이트만
	case 10: color = float3(s.rough, s.metallic, 0.0); break;   // 거칠기(R) · 금속성(G)
	case 11: color = float3(kAmbient, kAmbient, kAmbient); break;   // 6단계 — 주변 조도 / π (배경은 상수)
	case 12: color = float3(1.0, 0.0, 0.0);            break;   // 7단계 — GI 프로브 안 씀(배경)
	case 13: color = float3(1.0, 1.0, 1.0);            break;   // 8단계 — AO 없음(배경)
	case 5: case 6: case 7: case 9:
		color = float3(0.0, 0.0, 0.0);                          // 5 · 7 · 9 는 메인 패스의 [4b] 반사, 6 은 메인 패스 모션 벡터(배경은 둘 다 없음)
		break;
	}

	return float4(color, 1.0);
}
