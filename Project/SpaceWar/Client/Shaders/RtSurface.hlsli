// ============================================================
//  RtSurface.hlsli — RT 레이가 맞은 점의 표면 값 (SM 6.6 바인드리스 전용, 2026-10-27 렌더 7단계)
//
//  [4b] RtReflection.hlsl ClosestHit 안에 있던 «정점 세 개 읽기 → 무게중심 보간 → 재질 텍스처» 를 함수로 뺐다.
//  [4c] ProbeGI.hlsl(프로브 레이 · 컴퓨트 RayQuery)도 같은 표면 값을 써야 해서다.
//  ResourceDescriptorHeap 을 쓰므로 SM 6.6 셰이더만 include 한다(RtShadow.hlsl 은 cs_6_5 라 RtCommon.hlsli 만).
// ============================================================
#ifndef RT_SURFACE_HLSLI
#define RT_SURFACE_HLSLI

#include "RtCommon.hlsli"
#include "Shading.hlsli"   // kMinRoughness

struct HitSurface
{
	float3 albedo;
	float3 N;            // 월드 법선 (뒤집기 전 — 기하 그대로)
	float  rough;
	float  metallic;
	float3 emissive;
};

// inst · mat = 인스턴스 표 · 재질 표 한 줄, primitive = 삼각형 번호, bary = 무게중심(DXR 규약 y·z 성분),
// worldToObject = 인스턴스의 월드 → 물체 행렬(법선을 옮길 때 — 역전치와 같다).
HitSurface FetchHitSurface(RtInstance inst, RtMaterial mat, uint primitive, float2 bary, float3x4 worldToObject,
	SamplerState textureSampler)
{
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(inst.vertexBuffer)];
	ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(inst.indexBuffer)];
	const uint3  tri = ib.Load3(primitive * 12);
	const float3 w   = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

	float3 n  = float3(0.0, 0.0, 0.0);
	float3 c  = float3(0.0, 0.0, 0.0);
	float2 uv = float2(0.0, 0.0);
	[unroll]
	for (uint k = 0; k < 3; ++k)
	{
		const uint base = tri[k] * 60;   // Shared::Vertex 60바이트
		n  += asfloat(vb.Load3(base + 12)) * w[k];   // NORMAL
		c  += asfloat(vb.Load3(base + 24)) * w[k];   // COLOR
		uv += asfloat(vb.Load2(base + 36)) * w[k];   // TEXCOORD
	}

	Texture2D baseTex     = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.x)];
	Texture2D roughTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.z)];
	Texture2D metalTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.w)];
	Texture2D emissiveTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.emissiveTexture)];

	HitSurface s;
	// 법선은 월드 → 물체 행렬로 옮긴다(역전치와 같다 — 비균일 배율에도 맞다)
	s.N        = normalize(mul(n, (float3x3)worldToObject));
	s.albedo   = baseTex.SampleLevel(textureSampler, uv, 0).rgb * mat.baseColor.rgb * c;
	s.rough    = clamp(mat.roughness * roughTex.SampleLevel(textureSampler, uv, 0).r, kMinRoughness, 1.0);
	s.metallic = saturate(mat.metallic * metalTex.SampleLevel(textureSampler, uv, 0).r);
	s.emissive = mat.emissive * emissiveTex.SampleLevel(textureSampler, uv, 0).rgb;
	return s;
}

#endif
