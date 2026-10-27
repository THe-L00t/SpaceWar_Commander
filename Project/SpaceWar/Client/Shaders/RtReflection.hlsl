// ============================================================
//  RtReflection.hlsl — [4b] RT 반사 (DispatchRays 셰이더 라이브러리, lib_6_6, 2026-10-27 렌더 4단계)
//
//  DispatchRays(화면 크기) 한 번:
//    RayGen      G-buffer 픽셀마다 반사 레이 1개 → 반사 텍스처(바인드리스 UAV)에 결과 색
//    Miss        아무것도 안 맞으면 하늘색
//    ClosestHit  맞은 물체를 그 자리에서 셰이딩 — 정점 · 재질 · 텍스처를 SM 6.6 바인드리스로 읽는다
//                (태양 직접광 × 태양 가시도(RayQuery) + 앰비언트 + 자체발광, 재귀 반사 없음)
//  [4d] RtReflectionDenoise.hlsl 이 노이즈를 지우고, [5] Lighting.hlsl 이 F_env × 반사 색으로 더한다.
//
//  ★ 2026-10-27 렌더 5단계 — 반사 레이 방향을 B 방식으로 (사용자 결정 10-27)
//    거울 방향 하나가 아니라, 거칠기 분포(GGX 의 «보이는 법선», Heitz 2018)에서 픽셀 · 프레임마다 방향을 하나 뽑는다.
//    한 프레임만 보면 점박이 노이즈 → [4d] 가 이웃 픽셀 · 이전 프레임과 섞어 거칠기만큼 흐린 반사로 만든다.
//    출력 a 채널 = 레이가 맞은 거리(미스면 gMaxDistance) — 나중에 NRD · DLSS RR 이 요구하는 «반사 히트 거리» 자리.
//    원문 추정식의 가중치 F·G2/G1 중 F 는 라이팅의 F_env 가 맡고, G2/G1(≤ 1, 매끈할수록 1)은 지금 생략한다.
//  ★ 반사 레이는 RAY_FLAG_FORCE_OPAQUE — 투과 재질 물체도 반사에서는 불투명 표면으로 보인다(얼음 패스 [7] 전까지).
//  ★ 바인드리스: 루트 시그니처에 CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, 힙은 GRenderer 의 bindlessHeap.
//    히트 셰이더 안 번호는 레이마다 다를 수 있어 NonUniformResourceIndex 로 감싼다(사양 요구).
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"
#include "RtCommon.hlsli"

// GRenderer.cpp 의 ReflectionConstants 와 같은 배치 (144바이트)
cbuffer ReflectionCB : register(b0)
{
	float4x4 gInvViewProj;
	float3   gEyePos;        float gMaxDistance;     // 레이 길이 상한(m)
	float3   gSunDir;        float gMaxRoughness;    // 이 거칠기 이상은 반사 레이를 쏘지 않는다
	uint2    gSize;          uint  gNormalIndex;     uint gMaterialIndex;   // ↓ 바인드리스 힙 번호
	uint     gDepthIndex;    uint  gOutputIndex;     uint  gFrameIndex;   uint _pad0;   // 5단계 — 난수 씨앗용 프레임 번호
	float3   gSkyColor;      float _pad1;            // 미스 색 — 지금 하늘([6] 미정)은 지우기 색이라 같은 값
};

RaytracingAccelerationStructure gScene       : register(t0);   // TLAS (루트 SRV)
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);   // 재질 표 (루트 SRV)
StructuredBuffer<RtInstance>    gRtInstances : register(t2);   // TLAS 인스턴스 표 (루트 SRV, 프레임마다)
SamplerState                    gSampler     : register(s0);   // 정적 샘플러 (선형 · WRAP)

struct ReflectionPayload
{
	float3 color;
	float  hitT;   // 맞은 거리 (미스면 -1) — 나중에 디노이즈 · 흐림 정도에 쓸 자리
};

// 반사 레이 방향 — B 방식 (5단계).
//  ① 법선 N 기준 접선 공간을 만들고 ② 시선을 그 공간으로 옮겨 ③ GGX 보이는 법선(미세면) H 를 하나 뽑고
//  ④ 시선을 H 에 대해 반사한다(원문 식 16: L = reflect(V, N_i)).
//  뽑힌 방향이 표면 아래로 가면(노멀맵 · 비스듬한 시선에서 생긴다) 거울 방향으로 대신한다.
float3 ReflectionDirection(float3 N, float3 V, float rough, float2 u)
{
	float3 T, B;
	BuildOrthonormalBasis(N, T, B);
	const float3 Ve = float3(dot(V, T), dot(V, B), dot(V, N));
	const float3 He = SampleGgxVndf(Ve, rough * rough, u);
	const float3 H  = He.x * T + He.y * B + He.z * N;
	const float3 L  = reflect(-V, H);
	return (dot(L, N) > 0.0) ? L : reflect(-V, N);
}

[shader("raygeneration")]
void RayGen()
{
	const uint2 pixel = DispatchRaysIndex().xy;
	RWTexture2D<float4> output      = ResourceDescriptorHeap[gOutputIndex];
	Texture2D<float>    depthTex    = ResourceDescriptorHeap[gDepthIndex];
	Texture2D<float2>   normalTex   = ResourceDescriptorHeap[gNormalIndex];
	Texture2D<float4>   materialTex = ResourceDescriptorHeap[gMaterialIndex];

	const float depth = depthTex[pixel];
	if (depth >= 1.0)
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 메인 패스 물체 없음
		return;
	}
	const float rough = max(materialTex[pixel].r, kMinRoughness);
	if (rough >= gMaxRoughness)
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 거친 면 — 레이 없음(라이팅은 반사 0)
		return;
	}

	const float3 P   = ReconstructWorldPosition(pixel, depth, gSize, gInvViewProj);
	const float3 N   = DecodeNormalOct(normalTex[pixel]);
	const float3 V   = normalize(gEyePos - P);
	const float3 dir = ReflectionDirection(N, V, rough, Random2(pixel, gFrameIndex));
	if (dot(dir, N) <= 0.0)
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 노멀맵 때문에 반사 방향이 표면 아래로 — 쏘지 않는다
		return;
	}

	RayDesc ray;
	ray.Origin    = P + N * RayOffsetBias(length(P - gEyePos));
	ray.Direction = dir;
	ray.TMin      = 0.0;
	ray.TMax      = gMaxDistance;

	ReflectionPayload payload;
	payload.color = float3(0.0, 0.0, 0.0);
	payload.hitT  = -1.0;
	// 히트 그룹 0 번 하나, 미스 0 번 하나. 기하 기여 배수 1(BLAS 마다 기하 1개).
	TraceRay(gScene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);

	// a = 맞은 거리 (5단계 — 미스는 레이 길이 상한으로)
	output[pixel] = float4(payload.color, payload.hitT >= 0.0 ? payload.hitT : gMaxDistance);
}

[shader("miss")]
void Miss(inout ReflectionPayload payload)
{
	payload.color = gSkyColor;
	payload.hitT  = -1.0;
}

[shader("closesthit")]
void ClosestHit(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
	const RtInstance inst = gRtInstances[InstanceIndex()];
	const RtMaterial mat  = gRtMaterials[inst.material];

	// ── 맞은 삼각형의 정점 세 개를 바인드리스 버퍼에서 읽어 무게중심 좌표로 섞는다 ──
	ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(inst.vertexBuffer)];
	ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(inst.indexBuffer)];
	const uint3  tri = ib.Load3(PrimitiveIndex() * 12);
	const float3 w   = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);

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

	// 법선은 월드 → 물체 행렬로 옮긴다(역전치와 같다 — 비균일 배율에도 맞다).
	float3       N = normalize(mul(n, (float3x3)WorldToObject3x4()));
	const float3 V = -WorldRayDirection();
	if (dot(N, V) < 0.0) N = -N;   // 뒷면을 맞았으면 뒤집는다(양면처럼)

	// ── 재질 — 래스터(Forward.hlsl SampleSurface)와 같은 식. 노멀맵은 아직 읽지 않는다 ──
	Texture2D baseTex     = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.x)];
	Texture2D roughTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.z)];
	Texture2D metalTex    = ResourceDescriptorHeap[NonUniformResourceIndex(mat.textures.w)];
	Texture2D emissiveTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.emissiveTexture)];
	const float3 albedo   = baseTex.SampleLevel(gSampler, uv, 0).rgb * mat.baseColor.rgb * c;
	const float  rough    = clamp(mat.roughness * roughTex.SampleLevel(gSampler, uv, 0).r, kMinRoughness, 1.0);
	const float  metallic = saturate(mat.metallic * metalTex.SampleLevel(gSampler, uv, 0).r);
	const float3 emissive = mat.emissive * emissiveTex.SampleLevel(gSampler, uv, 0).rgb;

	// ── 맞은 점의 조명 — 메인 라이팅과 같은 식(Shading.hlsli) + 같은 그림자 규칙(RtCommon.hlsli) ──
	const float3 P   = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
	const DirectLighting lit = EvaluateSunAndAmbient(albedo, N, V, rough, metallic, gSunDir);
	const float3 L   = normalize(-gSunDir);
	const float3 sun = (dot(N, L) > 0.0) ? TraceSunVisibility(gScene, gRtMaterials, P + N * 0.02, L, gMaxDistance)
	                                     : float3(0.0, 0.0, 0.0);

	payload.color = (lit.diffuse + lit.specular) * sun + lit.ambient + emissive;
	payload.hitT  = RayTCurrent();
}
