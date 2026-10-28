// ============================================================
//  Transparent.hlsl — [7] 반투명 (래스터 forward, vs/ps_6_6 · 바인드리스 · 픽셀 셰이더 RayQuery, 2026-10-27 렌더 9단계)
//
//  투과 재질(transmission > 0, 얼음) 물체는 G-buffer 에 그리지 않고, 불투명 라이팅이 끝난 뒤 이 패스에서 뒤에서 앞으로 그린다.
//  정해진 것(10-27): 얼음 물체는 «(나) 뒤가 흐리게 보인다». 방법은 미정이라 임시로 HDRP 방식(화면 색 피라미드 굴절)을 골랐다.
//
//  픽셀 하나에서 (HDRP Lit.hlsl · Refraction.hlsl 의 순서 그대로):
//    ① 굴절 — 상자 모델: R = refract(−V, N, 1/ior), 얼음 속 거리 = 두께 / (R·−N), 나가는 점 = P + R × 거리
//    ② 그 점을 화면에 찍어 [화면 색 피라미드]에서 «거칠기^1.3 × (단계 수 − 1)» 단계를 읽는다(흐린 굴절)
//       그 자리의 불투명 표면이 얼음보다 앞이면 굴절 오프셋 없이 이 픽셀 자리를 읽는다(HDRP 와 같은 규칙)
//    ③ 흡수 — 비어-람베르트: exp(−흡수 계수 × 얼음 속 거리), 흡수 계수 = −ln(attenuationColor) / attenuationDistance
//    ④ 표면 반사 — 반사 레이 1개를 픽셀 셰이더 RayQuery 로(DXR 1.1). 맞으면 확산 셰이딩(태양 · 그림자 · GI 프로브), 아니면 하늘
//    ⑤ 태양 하이라이트 × 그림자(RayQuery, 투과 물체는 연하게 — [4a] 와 같은 규칙)
//    결과 = (1 − F) × 굴절 × 흡수 × 투과 색 + F × 반사 + 하이라이트 + 투과 안 되는 몫의 확산 + 발광
//  ★ 얼음 뒤의 얼음은 안 보인다 — 피라미드를 한 번만 뜨므로(선택지 (a)). 앞면만 그린다(뒷면 컬링).
// ============================================================

#include "Shading.hlsli"
#include "RtSurface.hlsli"
#include "Sky.hlsli"
#include "Probes.hlsli"

// GRenderer.cpp 의 TransparentConstants 와 같은 배치 (480바이트 — 10-28 GI 두 겹 288 → 384, 지면에 맞춘 축 → 480)
cbuffer TransparentCB : register(b0)
{
	float4x4 gViewProj;
	float4x4 gInvViewProj;
	float3   gEyePos;          float gMaxDistance;
	float3   gSunDir;          uint  gSceneColorSrv;      // ↓ 바인드리스 번호
	float3   gSunIlluminance;  uint  gDepthSrv;
	uint2    gSize;            uint  gMipCount;   uint gRtEnabled;
	uint     gSkySrv;          uint  gSkyIrradianceSrv;   uint gProbeIrradianceSrv;  uint gProbeDistanceSrv;
	uint     gProbeStateSrv;   uint3 _pad0;
	ProbeVolume gProbeVolume;      // GI 프로브 가까운 겹
	ProbeVolume gProbeVolumeFar;   // 2026-10-28 — 먼 겹 (같은 아틀라스의 뒤쪽)
};
cbuffer DrawCB : register(b1)
{
	uint gInstanceIndex;   // 패킷 순번 = 인스턴스 버퍼 줄
	uint gMaterialIndex;   // 재질 표 줄
	uint2 _pad1;
};

struct InstanceGpu   // GRenderer 의 Impl::InstanceGpu (128 B)
{
	row_major float4x4 world;
	row_major float4x4 prevWorld;
};

RaytracingAccelerationStructure gScene       : register(t0);
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);
StructuredBuffer<RtInstance>    gRtInstances : register(t2);
StructuredBuffer<InstanceGpu>   gInstances   : register(t3);
SamplerState gWrapSampler  : register(s0);
SamplerState gSkySampler   : register(s1);
SamplerState gClampSampler : register(s2);

struct VIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float3 col : COLOR;
	float2 uv  : TEXCOORD;
	float4 tan : TANGENT;
};
struct VOut
{
	float4 pos   : SV_POSITION;
	float3 world : WORLDPOS;
	float3 nrm   : NORMAL;
	float3 col   : COLOR;
	float2 uv    : TEXCOORD;
};

VOut VSMain(VIn i)
{
	const float4x4 world = gInstances[gInstanceIndex].world;
	VOut o;
	const float4 wp = mul(float4(i.pos, 1.0), world);
	o.world = wp.xyz;
	o.pos   = mul(wp, gViewProj);
	o.nrm   = mul(float4(i.nrm, 0.0), world).xyz;
	o.col   = i.col;
	o.uv    = i.uv;
	return o;
}

// 맞은 점의 확산 휘도(태양 × 그림자 + 주변광) — ProbeGI.hlsl 의 추적과 같은 식.
//  RayQuery 객체를 넘기지 않고 맞은 값들만 받는다.
float3 ShadeHitDiffuse(uint instanceIndex, uint primitive, float2 bary, float3x4 worldToObject, float3 P, float t, float3 D)
{
	const RtInstance inst = gRtInstances[instanceIndex];
	const RtMaterial mat  = gRtMaterials[inst.material];
	const HitSurface s = FetchHitSurface(inst, mat, primitive, bary, worldToObject, gWrapSampler);
	float3 N = s.N;
	if (dot(N, D) > 0.0) N = -N;
	const float3 L = -gSunDir;
	const float  NdotL = dot(N, L);
	float3 sun = float3(0.0, 0.0, 0.0);
	if (NdotL > 0.0)
		sun = gSunIlluminance * (NdotL / kPi) * TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gWrapSampler, P + N * RayOffsetBias(t), L, gMaxDistance);
	float  weight = 0.0;
	float3 ambient = float3(0.0, 0.0, 0.0);
	if (gProbeVolume.enabled != 0 || gProbeVolumeFar.enabled != 0)
	{
		Texture2D<float4>      irradiance = ResourceDescriptorHeap[gProbeIrradianceSrv];
		Texture2D<float2>      distance   = ResourceDescriptorHeap[gProbeDistanceSrv];
		StructuredBuffer<int4> states     = ResourceDescriptorHeap[gProbeStateSrv];
		uint layer;   // 2026-10-28 두 겹 — 가까운 겹 → 먼 겹
		ambient = SampleProbeLayers(irradiance, distance, states, gClampSampler, gProbeVolume, gProbeVolumeFar, P, N, -D, weight, layer);
	}
	if (weight <= 0.0)
	{
		Texture2D<float4> skyIrradiance = ResourceDescriptorHeap[gSkyIrradianceSrv];
		ambient = SampleSkyIrradiance(skyIrradiance, gSkySampler, N);
	}
	return s.albedo * (1.0 - s.metallic) * (sun + ambient) + s.emissive;
}

float4 PSIce(VOut i, bool frontFace : SV_IsFrontFace) : SV_Target
{
	const RtMaterial mat = gRtMaterials[gMaterialIndex];
	Texture2D baseTex     = ResourceDescriptorHeap[mat.textures.x];
	Texture2D roughTex    = ResourceDescriptorHeap[mat.textures.z];
	Texture2D emissiveTex = ResourceDescriptorHeap[mat.emissiveTexture];
	const float3 tint     = baseTex.Sample(gWrapSampler, i.uv).rgb * mat.baseColor.rgb * i.col;   // glTF: 투과광을 물들이는 baseColor
	const float  rough    = clamp(mat.roughness * roughTex.Sample(gWrapSampler, i.uv).g, kMinRoughness, 1.0);   // glTF: 거칠기 = G (2026-10-28)
	const float3 emissive = mat.emissive * emissiveTex.Sample(gWrapSampler, i.uv).rgb;

	float3 N = normalize(i.nrm);
	if (!frontFace) N = -N;
	const float3 P = i.world;
	const float3 V = normalize(gEyePos - P);
	const float  NdotV = saturate(dot(N, V));

	// 표면 반사율 — 굴절률에서 F0 (얼음 1.31 → 0.018), 거칠기를 고려한 슐릭(설계 문서 6.6 의 Fr 과 같은 식)
	const float  f0 = ((mat.ior - 1.0) / (mat.ior + 1.0)) * ((mat.ior - 1.0) / (mat.ior + 1.0));
	const float3 F0 = float3(f0, f0, f0);
	const float3 F  = F0 + (max(1.0 - rough, F0) - F0) * pow(1.0 - NdotV, 5.0);

	// ① 굴절 — HDRP RefractionModelBox
	const float3 R     = refract(-V, N, 1.0 / mat.ior);
	const float  inside = mat.thickness / max(dot(R, -N), 1.0e-5);   // 얼음 속 거리
	const float3 exitP = P + R * inside;

	// ② 화면 색 피라미드에서 흐린 단계로 읽기
	const float2 pixelUv = i.pos.xy / float2(gSize);
	const float  mip = pow(rough, 1.3) * float(max(gMipCount, 1u) - 1u);
	float offsetMul = max(0.0, 1.0 - mip * 0.08);   // HDRP: 흐릴수록 오프셋을 줄여 테두리 번짐(헤일로)을 줄인다
	const float4 clip = mul(float4(exitP, 1.0), gViewProj);
	float2 exitUv = pixelUv;
	if (clip.w > 1.0e-4)
	{
		exitUv = ClipToUv(clip);
		if (any(exitUv < 0.0) || any(exitUv > 1.0))
			offsetMul = 0.0;   // 화면 밖 — 이 픽셀 자리로
		else
		{
			// 그 자리의 불투명 표면이 얼음보다 앞이면 오프셋 없이(HDRP: hitLinearDepth <= 이 픽셀 깊이)
			Texture2D<float> depthTex = ResourceDescriptorHeap[gDepthSrv];
			const uint2 q = min(uint2(exitUv * float2(gSize)), gSize - 1);
			const float d = depthTex[q];
			if (d < 1.0)
			{
				const float3 hitP = ReconstructWorldPosition(q, d, gSize, gInvViewProj);
				if (length(hitP - gEyePos) <= length(P - gEyePos))
					offsetMul = 0.0;
			}
		}
	}
	else
		offsetMul = 0.0;
	Texture2D<float4> sceneColor = ResourceDescriptorHeap[gSceneColorSrv];
	const float3 behind = sceneColor.SampleLevel(gClampSampler, lerp(pixelUv, exitUv, offsetMul), mip).rgb;

	// ③ 흡수 — 흡수 거리 0 = 흡수 없음(재질 표에 그렇게 적는다)
	float3 absorbed = float3(1.0, 1.0, 1.0);
	if (mat.attenuationDistance > 0.0)
		absorbed = exp(log(max(mat.attenuationColor, 1.0e-4)) / mat.attenuationDistance * inside);   // = exp(−σ · 거리)
	const float3 transmitted = behind * absorbed * tint * mat.transmission;

	// ④ 표면 반사 — RT 가 켜져 있으면 반사 레이 1개, 아니면 하늘
	const float3 Rr = reflect(-V, N);
	float3 reflection;
	Texture2D<float4> sky = ResourceDescriptorHeap[gSkySrv];
	if (gRtEnabled != 0)
	{
		RayDesc ray;
		ray.Origin    = P + N * RayOffsetBias(length(P - gEyePos));
		ray.Direction = Rr;
		ray.TMin      = 0.0;
		ray.TMax      = gMaxDistance;
		// 2026-10-28 알파 마스크 — 전에는 FORCE_OPAQUE. 빈 곳은 지나가고 나머지 비불투명 후보(마스크의 있는 곳 · 얼음)는 확정
		RayQuery<RAY_FLAG_NONE> q;
		q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
		while (q.Proceed())
		{
			if (CandidateIsSolid(gRtInstances, gRtMaterials, q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(),
					q.CandidateTriangleBarycentrics(), gWrapSampler))
				q.CommitNonOpaqueTriangleHit();
		}
		if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			const float t = q.CommittedRayT();
			reflection = ShadeHitDiffuse(q.CommittedInstanceIndex(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(),
				q.CommittedWorldToObject3x4(), ray.Origin + Rr * t, t, Rr);
		}
		else
			reflection = SampleSkyRadiance(sky, gSkySampler, Rr);
	}
	else
		reflection = SampleSkyRadiance(sky, gSkySampler, Rr);

	// ⑤ 태양 하이라이트 + 투과 안 되는 몫의 확산 — 그림자는 RayQuery([4a] 와 같은 규칙)
	const float3 L = -gSunDir;
	const float3 H = normalize(L + V);
	const float  NdotL = saturate(dot(N, L));
	float3 sunVisibility = float3(1.0, 1.0, 1.0);
	if (gRtEnabled != 0 && NdotL > 0.0)
		sunVisibility = TraceSunVisibility(gScene, gRtMaterials, gRtInstances, gWrapSampler, P + N * RayOffsetBias(length(P - gEyePos)), L, gMaxDistance);
	const float3 specular = SpecularGGX(NdotL, NdotV, saturate(dot(N, H)), rough, FresnelSchlick(F0, saturate(dot(V, H))))
		* NdotL * gSunIlluminance * sunVisibility;
	const float3 opaqueDiffuse = (1.0 - mat.transmission) * tint / kPi * NdotL * gSunIlluminance * sunVisibility;

	const float3 color = (1.0 - F) * transmitted + F * reflection + specular + opaqueDiffuse + emissive;
	return float4(color, 1.0);
}
