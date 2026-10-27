// ============================================================
//  RtReflection.hlsl — [4b] RT 반사 (DispatchRays 셰이더 라이브러리, lib_6_6, 2026-10-27 렌더 4단계)
//
//  DispatchRays(화면 크기) 한 번:
//    RayGen      G-buffer 픽셀마다 반사 레이 1개 → 반사 텍스처(바인드리스 UAV)에 결과 색
//    Miss        아무것도 안 맞으면 하늘색
//    ClosestHit  맞은 물체를 그 자리에서 셰이딩 — 정점 · 재질 · 텍스처를 SM 6.6 바인드리스로 읽는다
//                (태양 직접광 × 태양 가시도(RayQuery) + 앰비언트 + 자체발광, 재귀 반사 없음)
//  [5] Lighting.hlsl 이 F_env × 반사 색으로 더한다.
//
//  ★ 반사 레이 방향(ReflectionDirection)은 «첫 판 범위» 결정을 기다린다.
//    지금은 두 방식에 공통인 경우(거울 방향)만 넣어 기반을 검증한다 — 노션 4단계 기록 참고.
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
	uint     gDepthIndex;    uint  gOutputIndex;     uint2 _pad0;
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

// 반사 레이 방향 — «첫 판 범위» 결정 전이라 거울 방향(두 방식에 공통인 경우)만 둔다.
float3 ReflectionDirection(float3 N, float3 V, float rough)
{
	return reflect(-V, N);
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
	const float3 dir = ReflectionDirection(N, V, rough);
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

	output[pixel] = float4(payload.color, 1.0);
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
