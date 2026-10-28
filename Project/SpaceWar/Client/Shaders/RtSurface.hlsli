// ============================================================
//  RtSurface.hlsli — RT 레이가 맞은 점의 표면 값 (SM 6.6 바인드리스 전용, 2026-10-27 렌더 7단계)
//
//  [4b] RtReflection.hlsl ClosestHit 안에 있던 «정점 세 개 읽기 → 무게중심 보간 → 재질 텍스처» 를 함수로 뺐다.
//  [4c] ProbeGI.hlsl(프로브 레이 · 컴퓨트 RayQuery)도 같은 표면 값을 써야 해서다.
//  ResourceDescriptorHeap 을 쓰므로 SM 6.6 셰이더만 include 한다.
//  (2026-10-28 — RtCommon.hlsli 도 알파 마스크 판정에 ResourceDescriptorHeap 을 쓰게 되어 RtShadow.hlsl 을 cs_6_6 로 올렸다)
// ============================================================
#ifndef RT_SURFACE_HLSLI
#define RT_SURFACE_HLSLI

#include "RtCommon.hlsli"
#include "Shading.hlsli"         // kMinRoughness
#include "TerrainLayers.hlsli"   // 2026-10-28 지형 레이어 — 마스크 UV · 높이 섞기

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

	float3 p  = float3(0.0, 0.0, 0.0);
	float3 n  = float3(0.0, 0.0, 0.0);
	float3 c  = float3(0.0, 0.0, 0.0);
	float2 uv = float2(0.0, 0.0);
	[unroll]
	for (uint k = 0; k < 3; ++k)
	{
		const uint base = tri[k] * 60;   // Shared::Vertex 60바이트
		p  += asfloat(vb.Load3(base + 0))  * w[k];   // POSITION (2026-10-28 — 지형 마스크 UV 용 메시 좌표)
		n  += asfloat(vb.Load3(base + 12)) * w[k];   // NORMAL
		c  += asfloat(vb.Load3(base + 24)) * w[k];   // COLOR
		uv += asfloat(vb.Load2(base + 36)) * w[k];   // TEXCOORD
	}

	// ── 지형 레이어 (2026-10-28) — 래스터 지형 판(Forward.hlsl TERRAIN_LAYERS)과 같은 마스크 · 높이 섞기의 가벼운 판 ──
	//  · 색 · 거칠기만 섞는다. 노멀맵은 일반 재질처럼 읽지 않는다(히트 법선 = 기하 법선).
	//  · 밉 0 을 읽는다 — DXR 셰이더에는 2×2 미분이 없다(DXR 명세). Unity HDRP 의 DXR 지형 패스도 미분 0(밉 0) 이다.
	//  · 마스크는 가장 가까운 텍셀 하나(Load) — 반복 샘플러로 읽으면 팔면체 가장자리에서 반대편과 섞이기 때문이다.
	//  · 4단계 색 흔들림(큰 규모 · 바위 · AO 배율)은 래스터와 같이 곱한다 — 반사 · GI 가 보는 지면 밝기가 화면과 같아야 해서다.
	//    노이즈는 거르지 않는다(footprint 0 — 레이에는 화면 미분이 없다). 행성 값은 재질 표 emissive.xy 에 있다.
	if (mat.terrain != 0)
	{
		Texture2DArray        layerBase   = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.x)];
		Texture2DArray        layerOrm    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.z)];
		Texture2DArray<float> layerHeight = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.w)];
		Texture2D             layerMask   = ResourceDescriptorHeap[NonUniformResourceIndex(mat.emissiveTexture)];

		uint maskWidth, maskHeight;
		layerMask.GetDimensions(maskWidth, maskHeight);
		const uint2  maskTexel = min(uint2(TerrainMaskUv(p) * float2(maskWidth, maskHeight)), uint2(maskWidth - 1, maskHeight - 1));
		const float4 mask      = layerMask.Load(int3(int2(maskTexel), 0));

		float4 layerHeights;
		[unroll] for (uint h = 0; h < 4; ++h)
			layerHeights[h] = layerHeight.SampleLevel(textureSampler, float3(uv, float(h)), 0);
		const float4 lw = TerrainLayerWeights(mask, layerHeights, mat.baseColor, mat.roughness);

		float3 albedo = float3(0.0, 0.0, 0.0);
		float  rough  = 0.0;
		float  ao     = 0.0;
		[unroll] for (uint l = 0; l < 4; ++l)
		{
			[branch] if (lw[l] > 0.0)
			{
				albedo += lw[l] * layerBase.SampleLevel(textureSampler, float3(uv, float(l)), 0).rgb;
				const float2 orm = layerOrm.SampleLevel(textureSampler, float3(uv, float(l)), 0).rg;
				rough  += lw[l] * orm.y;
				ao     += lw[l] * orm.x;
			}
		}
		const float3 co = float3(p.x, -p.z, p.y);   // 블렌더 물체 좌표(Forward.hlsl 지형 판과 같은 축)
		HitSurface t;
		t.N        = normalize(mul(n, (float3x3)worldToObject));
		t.albedo   = albedo * TerrainColorScale(co, 0.0, ao, lw[kTerrainRockLayer], mat.emissive.x, mat.emissive.y) * c;
		t.rough    = clamp(rough, kMinRoughness, 1.0);
		t.metallic = 0.0;
		t.emissive = float3(0.0, 0.0, 0.0);
		return t;
	}

	Texture2D baseTex     = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.x)];
	Texture2D roughTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.z)];
	Texture2D metalTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.w)];
	Texture2D emissiveTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.emissiveTexture)];

	HitSurface s;
	// 법선은 월드 → 물체 행렬로 옮긴다(역전치와 같다 — 비균일 배율에도 맞다)
	s.N        = normalize(mul(n, (float3x3)worldToObject));
	s.albedo   = baseTex.SampleLevel(textureSampler, uv, 0).rgb * mat.baseColor.rgb * c;
	// glTF metallicRoughness: 거칠기 = G, 금속성 = B (2026-10-28 — Forward.hlsl SampleSurface 와 같은 규약)
	s.rough    = clamp(mat.roughness * roughTex.SampleLevel(textureSampler, uv, 0).g, kMinRoughness, 1.0);
	s.metallic = saturate(mat.metallic * metalTex.SampleLevel(textureSampler, uv, 0).b);
	s.emissive = mat.emissive * emissiveTex.SampleLevel(textureSampler, uv, 0).rgb;
	return s;
}

#endif
