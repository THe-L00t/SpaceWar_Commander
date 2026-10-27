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
//    RT 는 이제 G-buffer 뒤 컴퓨트 단계가 맡는다 — [4a] 그림자는 RtShadow.hlsl, [4b] 반사는 미정.
//    그래서 이 파일에는 RayQuery · TLAS · RT_SUPPORTED 가 없고 항상 SM 6.0 으로 컴파일한다.
//
//  ★ 2026-09-26 재질 도입 (client2 `46567fd` 이식)
//    roughness/metallic 이 더 이상 자리표시자 상수가 아니다. 재질 상수(b2)와
//    텍스처 5장(t1~t5)에서 온다. 텍스처가 없는 재질에는 렌더러가 흰색·평면 노멀
//    폴백을 묶어 주므로, 셰이더는 항상 샘플해도 된다(분기 없음).
//    지형·더미 메시는 0번 기본 재질(roughness 0.10, 흰 폴백)을 쓰므로
//    이 변경 전과 같은 화면이 나와야 한다 — 회귀 확인 기준이다.
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"

// GRenderer.cpp 의 FrameConstants 와 같은 배치 (96바이트)
cbuffer FrameCB : register(b0)
{
	float4x4 gViewProj;
	float3   gEyePos;        float _pad0;
	float3   gSunDir;        uint  gDebugMode;
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
	row_major float4x4 prevWorld;   // 모션 벡터용 — 아직 읽지 않는다
};
StructuredBuffer<InstanceGpu> gInstances : register(t6);
StructuredBuffer<uint>        gVisible   : register(t7);

// ── 재질 (GRenderer 의 MaterialConstants 와 16바이트 단위로 같아야 한다) ──
cbuffer MaterialCB : register(b2)
{
	float4 gBaseColor;
	float3 gEmissive;      float gRoughness;
	float  gMetallic;      uint  gHasNormalMap;   float2 _pad3;
};

// 슬롯 순서는 ModelData.h 의 TextureSlot 과 같다.
Texture2D gBaseColorMap : register(t1);
Texture2D gNormalMap    : register(t2);
Texture2D gRoughnessMap : register(t3);
Texture2D gMetallicMap  : register(t4);
Texture2D gEmissiveMap  : register(t5);

SamplerState gSampler : register(s0);   // 루트 시그니처의 정적 샘플러 (선형 · WRAP)

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
};

VOut VSMain(VIn i, uint instance : SV_InstanceID)
{
	const float4x4 world = gInstances[gVisible[gInstanceBase + instance]].world;
	VOut o;
	float4 wp = mul(float4(i.pos, 1.0), world);
	o.world = wp.xyz;
	o.pos   = mul(wp, gViewProj);
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
float3 ApplyNormalMap(float3 N, float4 tangent, float2 uv)
{
	float3 T = tangent.xyz - N * dot(N, tangent.xyz);   // 그람-슈미트 직교화
	if (dot(T, T) < 1e-8) return N;                     // 접선이 퇴화했다 — 기하 법선을 쓴다

	T = normalize(T);
	float3 B = normalize(cross(N, T) * tangent.w);

	float3 nTex = gNormalMap.Sample(gSampler, uv).rgb * 2.0 - 1.0;
	nTex.y = -nTex.y;

	return normalize(T * nTex.x + B * nTex.y + N * nTex.z);
}

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

Surface SampleSurface(VOut i, bool frontFace)
{
	Surface s;
	float4 baseTex = gBaseColorMap.Sample(gSampler, i.uv);
	s.albedo   = baseTex.rgb * gBaseColor.rgb * i.col;
	s.rough    = clamp(gRoughness * gRoughnessMap.Sample(gSampler, i.uv).r, kMinRoughness, 1.0);
	s.metallic = saturate(gMetallic * gMetallicMap.Sample(gSampler, i.uv).r);
	s.emissive = gEmissive * gEmissiveMap.Sample(gSampler, i.uv).rgb;

	s.N = normalize(i.nrm);
	if (!frontFace) s.N = -s.N;
	if (gHasNormalMap != 0)
		s.N = ApplyNormalMap(s.N, i.tan, i.uv);
	return s;
}

// ── [1] G-buffer (메인 패스) ──
//  조명 계산 없이 표면 정보만 쓴다. 배치는 GBuffer.hlsli.
GBufferOut PSGBuffer(VOut i, bool frontFace : SV_IsFrontFace)
{
	const Surface s = SampleSurface(i, frontFace);
	return PackGBuffer(s.albedo, s.N, s.rough, s.metallic, s.emissive);
}

// ── 배경 패스 (forward) ──
//  메인 패스의 Lighting.hlsl 과 같은 식(Shading.hlsli)으로 태양 · 앰비언트만 계산한다.
//  배경(먼 행성)에는 RT 그림자 · 반사를 하지 않는다.
float4 PSMain(VOut i, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
	const Surface s = SampleSurface(i, frontFace);
	const float3 V = normalize(gEyePos - i.world);
	const DirectLighting lit = EvaluateSunAndAmbient(s.albedo, s.N, V, s.rough, s.metallic, gSunDir);

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
	case 5: case 6: case 7: case 9:
		color = float3(0.0, 0.0, 0.0);                          // 비어 있는 번호
		break;
	}

	return float4(color, 1.0);
}
