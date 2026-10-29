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
//    ⑥ (2026-10-29) 물가 페이드 — 재질이 켰으면 둘레의 땅(면 위로 올라온 표면) 비율로 얼음을 옅게(얼어붙은 호수의 물가)
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
	o.pos   = mul(float4(wp.xyz - gEyePos, 1.0), gViewProj);   // 2026-10-29 — 카메라 기준(Forward.hlsl VSMain 과 같은 깊이가 나와야 한다)
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
	const float4 clip = mul(float4(exitP - gEyePos, 1.0), gViewProj);   // 2026-10-29 — 카메라 기준
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
				const float3 hitP = ReconstructWorldPosition(q, d, gSize, gInvViewProj, gEyePos);
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

	float3 color = (1.0 - F) * transmitted + F * reflection + specular + opaqueDiffuse + emissive;

	// ⑥ 물가 페이드 (2026-10-29, 사용자 «얼음 호수 경계도 뚜렷해. 모든 메시 경계도 흐릿하게 섞어줘»)
	//  재질 edgeFade(m) > 0 일 때만 — glTF 에 없는 값이라 씬이 켠다(예시 행성 얼어붙은 호수, GRenderer::SetMaterialEdgeFade).
	//  ★ «바닥까지 깊이» 로 옅게 하는 물의 물가 페이드(UE DepthFade)는 여기 안 맞는다 — 빙하 호수는 얼음 면 아래 지형이
	//    호수 전체에서 6 cm 로 평평하고(물가에서만 지형이 면 위로 올라온다) 깊이로는 물가와 가운데를 가를 수 없다.
	//  그래서 얼음 면을 따라 이 점 둘레(반지름 edgeFade 의 고리 셋 × 8 방향)에서 위에서 아래로 짧은 레이(RayQuery)를 쏘아
	//  그 자리 지형이 얼음 면보다 올라와 있는지(물가) 직접 잰다. 곧은 물가에서 «땅» 비율은 물가 선 위 0.5 → edgeFade 안쪽 0
	//  이라 얼음 = 1 − 2 × 비율 로 하면 물가 선에서 0 → edgeFade 안쪽에서 그대로가 된다.
	//  · 화면 깊이로 재던 첫 판은 낮은 각도에서 물가 둔덕이 찍은 점을 멀리 앞에서 가려 거의 못 쟀다 → 월드 공간 레이로 바꿈.
	//  · 투과 물체(이 얼음 자신 · 얼음 덩어리)와 알파 마스크 빈 곳은 지나간다. 지형 재질이 아닌 것(돌 · 캐릭터)에 맞으면
	//    «모름» 으로 뺀다 — 얼음 위를 걷는 캐릭터 둘레가 옅어지지 않게. 레이가 아무것도 안 맞으면 땅 속에서 쏜 것(높은 둔덕)이라 땅.
	//  · 옅어지는 쪽 색은 «레이가 맞은 둘레 물가 지형이 화면에 보이는 색» 의 평균이다. 얼음 바로 밑(6 cm 아래 바닥)은 얼음과
	//    색이 거의 같아 옅게 해도 티가 안 났다 — 눈에 띄는 경계는 «어두운 얼음 ↔ 하얀 물가 둔덕» 이라서다.
	//  · RT 를 끄면 하지 않는다(물가가 예전처럼 뚜렷하다).
	if (mat.edgeFade > 0.0 && gRtEnabled != 0)
	{
		const float3 axisT = normalize(abs(N.y) < 0.99 ? cross(N, float3(0.0, 1.0, 0.0)) : cross(N, float3(1.0, 0.0, 0.0)));
		const float3 axisB = cross(N, axisT);
		static const uint  kFadeRings      = 3;
		static const uint  kFadeDirections = 8;
		static const float kLandHeight     = 0.05;   // 얼음 면보다 5 cm 이상 올라온 지형 = 땅
		static const float kProbeHeight    = 4.0;    // 레이를 얼음 면 위 4 m 에서 쏜다
		static const float kProbeDepth     = 0.5;    //        면 아래 0.5 m 까지
		float  land = 0.0;
		float  taps = 0.0;
		float3 shoreColor  = float3(0.0, 0.0, 0.0);
		float  shoreWeight = 0.0;
		[loop] for (uint ring = 0; ring < kFadeRings; ++ring)
		{
			const float radius = mat.edgeFade * float(ring + 1) / float(kFadeRings);
			[loop] for (uint k = 0; k < kFadeDirections; ++k)
			{
				const float a = (float(k) + float(ring) / float(kFadeRings)) * (2.0 * kPi / float(kFadeDirections));   // 고리마다 엇갈림
				RayDesc probe;
				probe.Origin    = P + (axisT * cos(a) + axisB * sin(a)) * radius + N * kProbeHeight;
				probe.Direction = -N;
				probe.TMin      = 0.0;
				probe.TMax      = kProbeHeight + kProbeDepth;
				RayQuery<RAY_FLAG_NONE> pq;
				pq.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, probe);
				while (pq.Proceed())
				{
					const RtInstance ci = gRtInstances[pq.CandidateInstanceIndex()];
					if (gRtMaterials[ci.material].transmissive == 0 &&
						CandidateIsSolid(gRtInstances, gRtMaterials, pq.CandidateInstanceIndex(), pq.CandidatePrimitiveIndex(),
							pq.CandidateTriangleBarycentrics(), gWrapSampler))
						pq.CommitNonOpaqueTriangleHit();
				}
				if (pq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
				{
					land += 1.0;   // 땅 속에서 쏜 레이 = 4 m 넘게 솟은 둔덕
					taps += 1.0;
					continue;
				}
				if (gRtMaterials[gRtInstances[pq.CommittedInstanceIndex()].material].terrain == 0)
					continue;      // 돌 · 캐릭터 = 모름
				const float w = saturate((kProbeHeight - pq.CommittedRayT()) / kLandHeight);
				land += w;
				taps += 1.0;
				// 맞은 물가 지형이 화면에 보이면 그 색을 모은다(가려져 있으면 빼고)
				[branch] if (w > 0.0)
				{
					const float3 S  = probe.Origin + probe.Direction * pq.CommittedRayT();
					const float4 sc = mul(float4(S - gEyePos, 1.0), gViewProj);
					if (sc.w > 1.0e-4)
					{
						const float2 suv = ClipToUv(sc);
						if (all(suv >= 0.0) && all(suv <= 1.0))
						{
							Texture2D<float> opaqueDepth = ResourceDescriptorHeap[gDepthSrv];
							const uint2 sp = min(uint2(suv * float2(gSize)), gSize - 1);
							const float sd = opaqueDepth[sp];
							if (sd < 1.0 && length(ReconstructWorldPosition(sp, sd, gSize, gInvViewProj, gEyePos) - S) <
								0.25 + 0.002 * length(S - gEyePos))
							{
								shoreColor  += w * sceneColor.SampleLevel(gClampSampler, suv, 0.0).rgb;
								shoreWeight += w;
							}
						}
					}
				}
			}
		}
		const float fade = taps > 0.0 ? saturate(1.0 - 2.0 * land / taps) : 1.0;
		// 옅어지는 쪽 = 둘레 물가 지형이 화면에 보이는 색(물가 선에서 옆 둔덕과 같아진다). 못 모았으면 얼음 없이 보이는 이 자리.
		const float3 shore = shoreWeight > 0.0 ? shoreColor / shoreWeight
		                                       : sceneColor.SampleLevel(gClampSampler, pixelUv, 0.0).rgb;
		color = lerp(shore, color, fade);
	}
	return float4(color, 1.0);
}
