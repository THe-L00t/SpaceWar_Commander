// ============================================================
//  AO.hlsl — [2] AO (컴퓨트 cs_6_6 · 바인드리스 · 인라인 RayQuery, 2026-10-27 렌더 8단계)
//
//  ★ 방식 — 사용자 생각(10-26 «기본은 SSAO, 화면에 그려지지 않지만 표현돼야 할 부분만 RTAO»)을 레이 단위로 옮긴 것.
//    SSAO 결과만 보고는 «화면에 없어서 놓친 픽셀» 을 고를 수 없으므로(10-26 답변), 레이 하나하나에서 판정한다.
//    픽셀마다 코사인 분포 방향 1개 (프레임마다 다름):
//      ① 화면 공간으로 따라간다(8칸, 반경 kAoRadius).
//          깊이 표면 앞으로만 지나감 → «안 가려짐» 확정 (카메라에 보이는 공간은 비어 있다)
//          깊이 표면 바로 뒤(두께 안)    → «가려짐»
//          화면 밖 · 표면 한참 뒤        → «모름»
//      ② «모름» 인 레이만 RayQuery 로 TLAS 에 쏜다(반경까지). RT 를 못 쓰면 «안 가려짐» 으로 본다(= 순수 SSAO).
//    세 경우 나누기는 Lumen 화면 추적의 규칙(«hit 이 없거나 표면 뒤로 지나가면 더 믿을 만한 방법»)과 같고,
//    화면 레이 + 월드 레이를 섞는 것은 NVIDIA HPG 2018 «Hybrid Ray-Traced Ambient Occlusion» 포스터와 같은 계열이다.
//  ★ 노이즈 — 레이 1개라 점박이. 5단계 반사 노이즈 제거와 같은 구조로 지운다:
//      CSAoTemporal  모션 벡터로 이전 프레임 자리를 찾아(쌍선형 4탭, 같은 표면 검사) 최대 kAoMaxHistory 프레임 누적
//      CSAoSpatial   5×5 쌍방향 필터(같은 평면 · 비슷한 법선)
//  ★ 라이팅은 간접 확산(GI 프로브 · 하늘 조도)에만 곱한다 — 직접광 · 반사에는 곱하지 않는다(HDRP 와 같은 자리, 10-26 답변).
// ============================================================

#include "GBuffer.hlsli"
#include "RtCommon.hlsli"   // RayOffsetBias · Random2 · BuildOrthonormalBasis

// GRenderer.cpp 의 AoConstants 와 같은 배치 (288바이트)
cbuffer AoCB : register(b0)
{
	float4x4 gViewProj;          // 월드 → 클립 (화면 공간 추적)
	float4x4 gInvViewProj;
	float4x4 gPrevInvViewProj;   // 이전 프레임 — 이전 깊이에서 이전 위치를 되살린다
	float3   gEyePos;        float gRadius;          // AO 반경(m)
	uint2    gSize;          uint  gFrameIndex;      uint gRtEnabled;   // 1 = «모름» 레이를 RayQuery 로
	uint     gDepthIndex;    uint  gNormalIndex;     uint gMotionIndex;     uint gPrevDepthIndex;    // ↓ 바인드리스 번호
	uint     gPrevNormalIndex; uint gRawUav;         uint gRawSrv;          uint gHistoryInSrv;
	uint     gHistoryOutUav; uint  gHistoryOutSrv;   uint gOutputUav;       uint gHistoryValid;
	float    gThickness;     uint3 _pad0;
};

RaytracingAccelerationStructure gScene : register(t0);   // TLAS (루트 SRV) — gRtEnabled 일 때만 읽는다
// 2026-10-28 알파 마스크 — «모름» 레이의 후보 판정에 재질 표 · 인스턴스 표 · 재질 텍스처 샘플러를 쓴다(컴퓨트 공용 루트 t1 · t2 · s0)
StructuredBuffer<RtMaterial>    gRtMaterials : register(t1);
StructuredBuffer<RtInstance>    gRtInstances : register(t2);
SamplerState                    gWrapSampler : register(s0);

// RT(DXR 1.1)가 없는 장치는 AO_RAYQUERY=0 으로 컴파일한다 — RayQuery 명령이 들어 있으면 PSO 를 못 만든다.
#ifndef AO_RAYQUERY
#define AO_RAYQUERY 1
#endif

static const uint  kAoSteps         = 8;      // 화면 공간 추적 칸 수
static const float kAoMaxHistory    = 24.0;   // 시간 누적 최대 프레임
static const float kNormalThreshold = 0.9;    // 이전 법선과 cos 이 이보다 작으면 다른 표면

float PlaneTolerance(float viewDistance)   // RtReflectionDenoise.hlsl 과 같은 값
{
	return 0.05 + viewDistance * 0.01;
}

bool LoadSurface(uint2 q, out float3 P, out float3 N)
{
	Texture2D<float>  depthTex  = ResourceDescriptorHeap[gDepthIndex];
	Texture2D<float2> normalTex = ResourceDescriptorHeap[gNormalIndex];
	const float depth = depthTex[q];
	P = float3(0.0, 0.0, 0.0);
	N = float3(0.0, 0.0, 1.0);
	if (depth >= 1.0)
		return false;
	P = ReconstructWorldPosition(q, depth, gSize, gInvViewProj, gEyePos);
	N = DecodeNormalOct(normalTex[q]);
	return true;
}

// 코사인 분포 반구 방향 (Malley — 원판 위 고른 점을 반구로 올린다)
float3 CosineDirection(float3 N, float2 u)
{
	float3 T, B;
	BuildOrthonormalBasis(N, T, B);
	const float r   = sqrt(u.x);
	const float phi = 6.28318531 * u.y;
	return normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(saturate(1.0 - u.x)));
}

static const uint kAoVisible  = 0;
static const uint kAoOccluded = 1;
static const uint kAoUnknown  = 2;

// ── ① 레이 (화면 공간 → 모르면 RT) ──  출력 raw = (가시성 0/1, RT 를 썼나 0/1)
[numthreads(8, 8, 1)]
void CSAoTrace(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;
	RWTexture2D<float2> raw = ResourceDescriptorHeap[gRawUav];

	float3 P, N;
	if (!LoadSurface(pixel, P, N))
	{
		raw[pixel] = float2(1.0, 0.0);
		return;
	}
	Texture2D<float> depthTex = ResourceDescriptorHeap[gDepthIndex];

	const float  viewDistance = length(P - gEyePos);
	const float3 origin = P + N * RayOffsetBias(viewDistance);
	const float2 u      = Random2(pixel, gFrameIndex * 3u + 1u);   // 반사(같은 씨앗)와 다른 난수 줄
	const float3 dir    = CosineDirection(N, u);

	// 화면 공간 추적 — 칸마다 그 점을 화면에 찍어 깊이 버퍼의 표면과 «카메라에서 거리» 로 비교한다
	uint verdict = kAoVisible;
	const float jitter = Random2(pixel.yx, gFrameIndex).x;   // 칸 위치를 프레임마다 흔든다(띠 무늬 방지)
	for (uint s = 0; s < kAoSteps; ++s)
	{
		const float  t    = gRadius * (s + jitter) / kAoSteps;
		const float3 p    = origin + dir * max(t, 0.01);
		const float4 clip = mul(float4(p - gEyePos, 1.0), gViewProj);   // 2026-10-29 — gViewProj 는 카메라 기준(GRenderer CameraRelativeViewProj)
		if (clip.w <= 1.0e-4) { verdict = kAoUnknown; break; }
		const float2 uv = ClipToUv(clip);
		if (any(uv < 0.0) || any(uv >= 1.0)) { verdict = kAoUnknown; break; }   // 화면 밖
		const uint2  q  = uint2(uv * float2(gSize));
		const float  sd = depthTex[q];
		if (sd >= 1.0) continue;                                            // 그 자리에 물체 없음(하늘) — 비어 있다
		const float3 Ps   = ReconstructWorldPosition(q, sd, gSize, gInvViewProj, gEyePos);
		const float  dRay = length(p - gEyePos);
		const float  dSrf = length(Ps - gEyePos);
		const float  bias = 0.02 + dSrf * 0.002;                            // 깊이에서 되살린 위치의 오차 몫
		if (dRay > dSrf + bias)                                             // 보이는 표면 뒤로 들어갔다
		{
			verdict = (dRay - dSrf < gThickness) ? kAoOccluded : kAoUnknown;
			break;
		}
	}

	bool usedRt = false;
	if (verdict == kAoUnknown)
	{
		verdict = kAoVisible;   // RT 를 못 쓰면 «안 가려짐» (= 순수 SSAO)
#if AO_RAYQUERY
		if (gRtEnabled != 0)
		{
			usedRt = true;
			RayDesc ray;
			ray.Origin    = origin;
			ray.Direction = dir;
			ray.TMin      = 0.0;
			ray.TMax      = gRadius;
			// 2026-10-28 알파 마스크 — 전에는 FORCE_OPAQUE. 빈 곳은 지나가고, 나머지 비불투명 후보(마스크의 있는 곳 · 얼음)는 가림으로 확정
			RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
			q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
			while (q.Proceed())
			{
				if (CandidateIsSolid(gRtInstances, gRtMaterials, q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(),
						q.CandidateTriangleBarycentrics(), gWrapSampler))
					q.CommitNonOpaqueTriangleHit();
			}
			if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
				verdict = kAoOccluded;
		}
#endif
	}
	raw[pixel] = float2(verdict == kAoOccluded ? 0.0 : 1.0, usedRt ? 1.0 : 0.0);
}

// ── ② 시간 누적 ──  이력 = (AO, 누적 수)
[numthreads(8, 8, 1)]
void CSAoTemporal(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;
	RWTexture2D<float2> historyOut = ResourceDescriptorHeap[gHistoryOutUav];

	float3 P, N;
	if (!LoadSurface(pixel, P, N))
	{
		historyOut[pixel] = float2(1.0, 0.0);
		return;
	}
	Texture2D<float2> rawTex = ResourceDescriptorHeap[gRawSrv];
	const float rawAo = rawTex[pixel].x;

	float history = 1.0;
	float count   = 0.0;
	if (gHistoryValid != 0)
	{
		Texture2D<float2> motionTex     = ResourceDescriptorHeap[gMotionIndex];
		Texture2D<float>  prevDepthTex  = ResourceDescriptorHeap[gPrevDepthIndex];
		Texture2D<float2> prevNormalTex = ResourceDescriptorHeap[gPrevNormalIndex];
		Texture2D<float2> historyIn     = ResourceDescriptorHeap[gHistoryInSrv];

		const float2 uv      = (float2(pixel) + 0.5) / float2(gSize);
		const float2 prevPos = (uv + motionTex[pixel]) * float2(gSize) - 0.5;
		const int2   base    = int2(floor(prevPos));
		const float2 f       = prevPos - float2(base);
		const float  tol     = PlaneTolerance(length(P - gEyePos));

		float sumA = 0.0, sumN = 0.0, sumW = 0.0;
		[unroll]
		for (uint k = 0; k < 4; ++k)
		{
			const int2 o = int2(k & 1u, k >> 1u);
			const int2 q = base + o;
			if (q.x < 0 || q.y < 0 || q.x >= int(gSize.x) || q.y >= int(gSize.y)) continue;
			const float pd = prevDepthTex[q];
			if (pd >= 1.0) continue;
			const float3 Pp = ReconstructWorldPosition(uint2(q), pd, gSize, gPrevInvViewProj, gEyePos);
			const float3 Np = DecodeNormalOct(prevNormalTex[q]);
			if (abs(dot(N, Pp - P)) > tol || dot(N, Np) < kNormalThreshold) continue;
			const float2 h = historyIn[q];
			if (h.y <= 0.0) continue;
			const float w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
			sumA += h.x * w;
			sumN += h.y * w;
			sumW += w;
		}
		if (sumW > 1.0e-3)
		{
			history = sumA / sumW;
			count   = sumN / sumW;
		}
	}
	count = min(count + 1.0, kAoMaxHistory);
	historyOut[pixel] = float2(lerp(history, rawAo, 1.0 / count), count);
}

// ── ③ 공간 필터 ──  5×5 탭(2 픽셀 간격), 같은 평면 · 비슷한 법선만 섞는다
[numthreads(8, 8, 1)]
void CSAoSpatial(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;
	RWTexture2D<float> output = ResourceDescriptorHeap[gOutputUav];

	float3 P, N;
	if (!LoadSurface(pixel, P, N))
	{
		output[pixel] = 1.0;
		return;
	}
	Texture2D<float2> temporal = ResourceDescriptorHeap[gHistoryOutSrv];
	const float tol = PlaneTolerance(length(P - gEyePos));
	float sum = 0.0, sumW = 0.0;
	// [loop] — 펼치면(unroll) Debug(-Od) 에서 DXIL 이 209 KB 가 되고 컴파일 2.5 초 + 드라이버 PSO 생성 3.6 초가 걸렸다.
	[loop]
	for (int j = -2; j <= 2; ++j)
	{
		[loop]
		for (int i = -2; i <= 2; ++i)
		{
			const int2 q = int2(pixel) + int2(i, j) * 2;
			if (q.x < 0 || q.y < 0 || q.x >= int(gSize.x) || q.y >= int(gSize.y)) continue;
			float3 Pq, Nq;
			if (!LoadSurface(uint2(q), Pq, Nq)) continue;
			const float wS = exp(-float(i * i + j * j) / (2.0 * 1.5 * 1.5));
			const float wP = exp(-abs(dot(N, Pq - P)) / tol);
			const float wN = pow(saturate(dot(N, Nq)), 16.0);
			const float w  = wS * wP * wN;
			sum  += temporal[q].x * w;
			sumW += w;
		}
	}
	output[pixel] = sum / max(sumW, 1.0e-6);
}
