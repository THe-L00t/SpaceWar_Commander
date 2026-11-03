// ============================================================
//  RtReflectionDenoise.hlsl — [4d] 반사 노이즈 제거 (컴퓨트 cs_6_6, SM 6.6 바인드리스, 2026-10-27 렌더 5단계)
//
//  ★ 임시 구현이다. 사용자 결정(10-27): «NRD 랑 DLSS 는 나중에 고민». 그때까지 B 방식 반사(점박이 노이즈)를 쓸 수 있게 하는 자리다.
//    NRD 또는 DLSS RR 이 들어오면 이 파일 자리를 바꾼다. 입력(반사 색 + 맞은 거리, 모션 벡터, 깊이, 법선, 거칠기)은 그대로 쓸 수 있다.
//
//  두 패스 (GRenderer 가 같은 상수 · 루트 시그니처로 연달아 디스패치한다)
//    CSTemporal  시간 누적: 모션 벡터로 이전 프레임 이력의 자리를 찾고(쌍선형 4탭),
//                이전 프레임 깊이 · 법선과 비교해 «같은 표면» 인 탭만 쓴다. 아니면 이력을 버리고 새로 쌓는다.
//                이력 = lerp(이전 이력, 이번 레이 결과, 1/누적 수). 누적 수 상한은 거칠기로 정한다
//                (매끈할수록 작게 — 매끈한 반사는 시선에 따라 바뀌어 오래 쌓으면 잔상이 남는다).
//    CSSpatial   공간 필터: 5×5 탭, 같은 평면 · 비슷한 법선 · 비슷한 거칠기인 이웃만 섞는다.
//                반경 = 최대 반경 × (거칠기 / 기준) ÷ √누적 수 — 거칠수록 넓게, 이력이 쌓일수록 좁게.
//
//  알려진 한계 (임시 구현이라 둔다 — 노션 5단계 기록)
//    · 반사는 표면 모션 벡터로 옮긴다. 카메라가 움직이면 반사 속 물체는 실제로는 다르게 움직이므로 잔상이 남을 수 있다
//      (NRD 는 맞은 거리로 «반사 속 움직임» 을 따로 구한다).
//    · 모션 벡터가 2D 라, 물체가 법선 방향으로 움직이면 «같은 표면» 검사에 걸려 이력을 버린다(NRD README 도 2D MV 의 한계로 적는다).
//    · 첫 번째 한계(잔상)를 줄이려고 이력 클램프를 넣었다 — 이번 레이 결과 5×5 평균 ± 1.5σ 로 자른다.
//      노이즈가 큰 곳은 σ 가 커서 덜 듣는다.
// ============================================================

#include "Shading.hlsli"
#include "GBuffer.hlsli"

// GRenderer.cpp 의 DenoiseConstants 와 같은 배치 (208바이트). 번호는 전부 바인드리스 힙 번호.
cbuffer DenoiseCB : register(b0)
{
	float4x4 gInvViewProj;        // 이번 프레임 (행벡터 규약 mul(v, M))
	float4x4 gPrevInvViewProj;    // 이전 프레임 — 이전 깊이에서 이전 위치를 되살린다
	float3   gEyePos;             float gMaxRoughness;
	uint2    gSize;               uint  gHistoryValid;     uint _pad0;   // gHistoryValid = 0 이면 이력 없이 시작
	uint     gRawIndex;           uint  gMotionIndex;      uint gDepthIndex;       uint gNormalIndex;
	uint     gMaterialIndex;      uint  gPrevDepthIndex;   uint gPrevNormalIndex;  uint gHistoryInIndex;
	uint     gHistoryOutUav;      uint  gHistoryOutSrv;    uint gOutputUav;        uint _pad1;
};

static const float kMinHistoryFrames = 2.0;    // 거칠기 ≈ 0 (거울) — 레이 방향이 거의 안 흔들려 노이즈가 적다
static const float kMaxHistoryFrames = 32.0;   // 거칠기 = 기준(0.4)
static const float kMaxBlurRadius    = 8.0;    // 픽셀
static const float kNormalThreshold  = 0.9;    // 이전 법선과 cos 이 이보다 작으면 다른 표면
static const float kClampSigma       = 1.5;    // 이력 클램프 폭 — 이웃 평균 ± 1.5σ

// «같은 평면» 허용 거리(m) — 깊이에서 되살린 위치 오차가 거리에 따라 커지므로 거리에 비례해 늘린다.
float PlaneTolerance(float viewDistance)
{
	return 0.05 + viewDistance * 0.01;
}

// 이번 프레임 픽셀 q 의 표면. 메인 패스 물체가 없거나(깊이 1) 반사 레이를 안 쏜 거친 면이면 false.
bool LoadSurface(uint2 q, out float3 P, out float3 N, out float rough)
{
	Texture2D<float>  depthTex    = ResourceDescriptorHeap[gDepthIndex];
	Texture2D<float2> normalTex   = ResourceDescriptorHeap[gNormalIndex];
	Texture2D<float4> materialTex = ResourceDescriptorHeap[gMaterialIndex];
	const float depth = depthTex[q];
	rough = max(materialTex[q].r, kMinRoughness);   // 라이팅 · 반사 셰이더와 같은 바닥값
	P = float3(0.0, 0.0, 0.0);
	N = float3(0.0, 0.0, 1.0);
	if (depth >= 1.0 || rough >= gMaxRoughness)
		return false;
	P = ReconstructWorldPosition(q, depth, gSize, gInvViewProj, gEyePos);
	N = DecodeNormalOct(normalTex[q]);
	return true;
}

[numthreads(8, 8, 1)]
void CSTemporal(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;
	RWTexture2D<float4> historyOut = ResourceDescriptorHeap[gHistoryOutUav];

	float3 P, N;
	float  rough;
	if (!LoadSurface(pixel, P, N, rough))
	{
		historyOut[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 누적 수 0 = 이력 없음
		return;
	}

	Texture2D<float4> rawTex = ResourceDescriptorHeap[gRawIndex];
	const float3 raw = rawTex[pixel].rgb;

	float3 history = float3(0.0, 0.0, 0.0);
	float  count   = 0.0;
	if (gHistoryValid != 0)
	{
		Texture2D<float2> motionTex     = ResourceDescriptorHeap[gMotionIndex];
		Texture2D<float>  prevDepthTex  = ResourceDescriptorHeap[gPrevDepthIndex];
		Texture2D<float2> prevNormalTex = ResourceDescriptorHeap[gPrevNormalIndex];
		Texture2D<float4> historyIn     = ResourceDescriptorHeap[gHistoryInIndex];

		// 이전 프레임에서 이 표면이 있던 자리 (GB4: 이전 UV = 지금 UV + MV)
		const float2 uv      = (float2(pixel) + 0.5) / float2(gSize);
		const float2 prevPos = (uv + motionTex[pixel]) * float2(gSize) - 0.5;   // 픽셀 중심 기준 좌표
		const int2   base    = int2(floor(prevPos));
		const float2 f       = prevPos - float2(base);
		const float  tol     = PlaneTolerance(length(P - gEyePos));

		float3 sumC = float3(0.0, 0.0, 0.0);
		float  sumN = 0.0;
		float  sumW = 0.0;
		[unroll]
		for (uint k = 0; k < 4; ++k)
		{
			const int2 o = int2(k & 1u, k >> 1u);
			const int2 q = base + o;
			if (q.x < 0 || q.y < 0 || q.x >= int(gSize.x) || q.y >= int(gSize.y)) continue;
			const float pd = prevDepthTex[q];
			if (pd >= 1.0) continue;
			// 같은 표면인가 — 이전 위치가 지금 표면의 평면 위에 있고, 법선이 비슷해야 한다
			const float3 Pp = ReconstructWorldPosition(uint2(q), pd, gSize, gPrevInvViewProj, gEyePos);
			const float3 Np = DecodeNormalOct(prevNormalTex[q]);
			if (abs(dot(N, Pp - P)) > tol || dot(N, Np) < kNormalThreshold) continue;
			const float4 h = historyIn[q];
			if (h.a <= 0.0) continue;   // 그 자리는 지난 프레임에 반사가 없었다(거친 면 등)
			const float w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
			sumC += h.rgb * w;
			sumN += h.a * w;
			sumW += w;
		}
		if (sumW > 1.0e-3)
		{
			history = sumC / sumW;
			count   = sumN / sumW;

			// ── 이력 클램프 (잔상 줄이기) ──
			//  카메라 · 물체가 움직이면 표면 모션 벡터로 옮긴 «반사» 이력이 실제와 어긋나 끌린 자국이 남는다(실제로 보였다).
			//  이번 프레임 레이 결과 5×5 의 평균 ± kσ 범위 밖의 이력은 그 범위로 자른다.
			//  노이즈가 큰 곳은 σ 가 커서 거의 안 자르고, 고른 곳에서 크게 바뀐 이력(잔상)만 자른다.
			float3 m1 = float3(0.0, 0.0, 0.0);
			float3 m2 = float3(0.0, 0.0, 0.0);
			float  n  = 0.0;
			[unroll]
			for (int j = -2; j <= 2; ++j)
			{
				[unroll]
				for (int i = -2; i <= 2; ++i)
				{
					const int2 q = int2(pixel) + int2(i, j);
					if (q.x < 0 || q.y < 0 || q.x >= int(gSize.x) || q.y >= int(gSize.y)) continue;
					const float4 s = rawTex[q];
					if (s.a <= 0.0) continue;   // 레이를 안 쏜 픽셀(물체 없음 · 거친 면) — a = 맞은 거리라 0 이면 안 쏜 것
					m1 += s.rgb;
					m2 += s.rgb * s.rgb;
					n  += 1.0;
				}
			}
			const float3 mean  = m1 / n;   // 가운데 픽셀은 늘 레이를 쏜 픽셀이라 n ≥ 1
			const float3 sigma = sqrt(max(m2 / n - mean * mean, 0.0));
			history = clamp(history, mean - kClampSigma * sigma, mean + kClampSigma * sigma);
		}
	}

	const float maxFrames = lerp(kMinHistoryFrames, kMaxHistoryFrames, saturate(rough / gMaxRoughness));
	count = min(count + 1.0, maxFrames);
	historyOut[pixel] = float4(lerp(history, raw, 1.0 / count), count);
}

[numthreads(8, 8, 1)]
void CSSpatial(uint3 id : SV_DispatchThreadID)
{
	const uint2 pixel = id.xy;
	if (pixel.x >= gSize.x || pixel.y >= gSize.y) return;
	RWTexture2D<float4> output = ResourceDescriptorHeap[gOutputUav];

	float3 P, N;
	float  rough;
	if (!LoadSurface(pixel, P, N, rough))
	{
		output[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // 반사 없음 — 라이팅이 0 을 더한다
		return;
	}

	Texture2D<float4> temporal = ResourceDescriptorHeap[gHistoryOutSrv];
	const float4 center = temporal[pixel];
	const float  radius = kMaxBlurRadius * saturate(rough / gMaxRoughness) * rsqrt(max(center.a, 1.0));
	if (radius < 0.5)
	{
		output[pixel] = float4(center.rgb, 1.0);   // 거울에 가까운 면 — 흐릴 것이 없다
		return;
	}

	const float tol  = PlaneTolerance(length(P - gEyePos));
	const float step = radius * 0.5;   // 5×5 탭 사이 간격(픽셀)
	float3 sum  = float3(0.0, 0.0, 0.0);
	float  sumW = 0.0;
	[unroll]
	for (int j = -2; j <= 2; ++j)
	{
		[unroll]
		for (int i = -2; i <= 2; ++i)
		{
			const int2 q = int2(pixel) + int2(round(float2(i, j) * step));
			if (q.x < 0 || q.y < 0 || q.x >= int(gSize.x) || q.y >= int(gSize.y)) continue;
			float3 Pq, Nq;
			float  rq;
			if (!LoadSurface(uint2(q), Pq, Nq, rq)) continue;
			const float wS = exp(-float(i * i + j * j) / (2.0 * 1.5 * 1.5));          // 가우스 (탭 단위 σ 1.5)
			const float wP = exp(-abs(dot(N, Pq - P)) / tol);                         // 같은 평면
			const float wN = pow(saturate(dot(N, Nq)), 32.0);                         // 비슷한 법선
			const float wR = exp(-abs(rough - rq) / (0.1 * gMaxRoughness));           // 비슷한 거칠기
			const float w  = wS * wP * wN * wR;
			sum  += temporal[q].rgb * w;
			sumW += w;
		}
	}
	output[pixel] = float4(sum / max(sumW, 1.0e-6), 1.0);   // 가운데 탭 가중치가 1 이라 sumW ≥ 1
}
