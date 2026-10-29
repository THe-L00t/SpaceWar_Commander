// ============================================================
//  PostProcess.hlsl — [8] 후처리 (컴퓨트 cs_6_6 · 바인드리스, 2026-10-27 렌더 10단계)
//
//  구조도 [8] «(안개) → 블룸 → 자동 노출 → 톤맵 → UI» 중 블룸 · 자동 노출. 톤맵은 Tonemap.hlsl PSTonemap.
//  방법은 전부 임시 선택(미정):
//    · 자동 노출 — 휘도 히스토그램(UE 방식). 값도 UE 기본값: log2 휘도 −8 ~ 4, 아래 10 % · 위 10 % 를 빼고 평균,
//      평균 밝기 0.03 ~ 8 로 자름, 밝아질 때 3 스톱/초 · 어두워질 때 1 스톱/초. 노출 = 0.18 / 평균 밝기(18 % 회색).
//    · 블룸 — Jimenez, «Next Generation Post Processing in Call of Duty: Advanced Warfare» (SIGGRAPH 2014):
//      반 해상도부터 6단 내리기(13탭, 첫 단계만 Karis 평균 — 밝은 점 하나가 깜빡이는 것 막기) →
//      작은 쪽부터 올리기(3×3 텐트) 하며 그 단계 내리기 결과에 더한다. 화면에 0.04 비율로 섞는다.
//      탭 위치 · 가중치는 LearnOpenGL «Physically Based Bloom» 해설(같은 발표를 출처로 밝힘)과 같다.
//    진입점:
//      CSHistogram       HDR 픽셀마다 log2 휘도 → 256칸 히스토그램 (그룹 공유 메모리에 모았다가 전역에 더함)
//      CSExposure        히스토그램 → 평균 밝기 → 적응(스톱/초) → 노출 버퍼. 다 읽은 칸은 0 으로(다음 프레임용)
//      CSBloomDownsample 원본(HDR 또는 윗 단계) → 이 단계. gFirst = 1 이면 Karis 평균
//      CSBloomUpsample   아랫 단계 «올린 결과» 텐트 + 이 단계 «내린 결과»
// ============================================================

cbuffer PostCB : register(b0)
{
	uint   gSrcIndex;     uint gDstIndex;     uint gAddIndex;    uint gFirst;      // ↓ 바인드리스 번호
	uint2  gDstSize;      float2 gSrcTexel;                                          // 대상 크기, 원본 텍셀 크기
	uint   gHistogramIndex; uint gExposureUav; float gDeltaTime;  uint gPixelCount;
	float  gLogMin;       float gLogMax;      float gLowPercent; float gHighPercent;
	float  gMinBrightness; float gMaxBrightness; float gSpeedUp;  float gSpeedDown;
	uint2  gHdrSize;      uint gFrameIndex;   uint _pad0;
};
SamplerState gClampSampler : register(s2);

static const uint kBins = 256;

float Luminance(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));   // Rec.709
}

// ── 자동 노출 ① 히스토그램 ──  칸 0 = 아주 어두움(log2 < 최소), 칸 1 ~ 255 = log2 휘도 [최소, 최대]
groupshared uint gsBins[kBins];

[numthreads(16, 16, 1)]
void CSHistogram(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	gsBins[gi] = 0;
	GroupMemoryBarrierWithGroupSync();
	if (all(id.xy < gHdrSize))
	{
		Texture2D<float4> hdr = ResourceDescriptorHeap[gSrcIndex];
		const float lum = Luminance(hdr[id.xy].rgb);
		uint bin = 0;
		if (lum > 1.0e-6)
		{
			const float t = saturate((log2(lum) - gLogMin) / (gLogMax - gLogMin));
			bin = uint(t * 254.0 + 1.0);
		}
		InterlockedAdd(gsBins[bin], 1);
	}
	GroupMemoryBarrierWithGroupSync();
	RWByteAddressBuffer histogram = ResourceDescriptorHeap[gHistogramIndex];
	if (gsBins[gi] != 0)
		histogram.InterlockedAdd(gi * 4, gsBins[gi]);
}

// ── 자동 노출 ② 평균 · 적응 ──  그룹 하나(256 스레드). 노출 버퍼 = (노출, 적응된 log2 밝기, 목표 log2 밝기, 유효)
groupshared uint gsCount[kBins];

[numthreads(256, 1, 1)]
void CSExposure(uint gi : SV_GroupIndex)
{
	RWByteAddressBuffer histogram = ResourceDescriptorHeap[gHistogramIndex];
	gsCount[gi] = histogram.Load(gi * 4);
	histogram.Store(gi * 4, 0);   // 다음 프레임용으로 비운다
	GroupMemoryBarrierWithGroupSync();
	if (gi != 0) return;

	// 칸 0(아주 어두운 픽셀 · 물체 없음)은 빼고, 위아래 퍼센트 밖을 버린 평균 (UE 방식)
	float total = 0.0;
	for (uint b = 1; b < kBins; ++b) total += gsCount[b];
	RWStructuredBuffer<float4> exposure = ResourceDescriptorHeap[gExposureUav];
	float4 state = exposure[0];
	float targetLog2 = log2(0.18);   // 히스토그램이 비면 중간 회색
	if (total > 0.0)
	{
		const float lowCut  = total * gLowPercent;
		const float highCut = total * gHighPercent;
		float below = 0.0, sumLog = 0.0, sumCount = 0.0;
		for (uint b = 1; b < kBins; ++b)
		{
			const float count = gsCount[b];
			// 이 칸에서 [lowCut, highCut] 안에 드는 몫만
			const float take = max(0.0, min(below + count, highCut) - max(below, lowCut));
			const float binLog = gLogMin + (float(b) - 0.5) / 254.0 * (gLogMax - gLogMin);
			sumLog   += take * binLog;
			sumCount += take;
			below    += count;
		}
		if (sumCount > 0.0)
			targetLog2 = sumLog / sumCount;
	}
	targetLog2 = clamp(targetLog2, log2(gMinBrightness), log2(gMaxBrightness));

	// 적응 — 스톱(log2) 단위로 일정 속도. 밝아질 때 빠르게, 어두워질 때 느리게. 첫 프레임은 바로 맞춘다.
	float adapted = targetLog2;
	if (state.w > 0.5)
	{
		const float delta = targetLog2 - state.y;
		const float step  = (delta > 0.0 ? gSpeedUp : gSpeedDown) * gDeltaTime;
		adapted = state.y + clamp(delta, -step, step);
	}
	exposure[0] = float4(0.18 / exp2(adapted), adapted, targetLog2, 1.0);
}

// ── 블룸 ① 내리기 — 13탭 (가운데 0.125, 모서리 0.03125, 변 가운데 0.0625, 안쪽 4개 0.125) ──
float3 Tap(Texture2D<float4> src, float2 uv, float2 offset)
{
	return src.SampleLevel(gClampSampler, uv + offset * gSrcTexel, 0).rgb;
}
float KarisWeight(float3 c)   // 1 / (1 + 휘도) — 아주 밝은 텍셀 하나의 몫을 줄인다
{
	return 1.0 / (1.0 + Luminance(c));
}

[numthreads(8, 8, 1)]
void CSBloomDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDstSize)) return;
	Texture2D<float4>   src = ResourceDescriptorHeap[gSrcIndex];
	RWTexture2D<float4> dst = ResourceDescriptorHeap[gDstIndex];
	const float2 uv = (float2(id.xy) + 0.5) / float2(gDstSize);

	const float3 a = Tap(src, uv, float2(-2.0,  2.0)), b = Tap(src, uv, float2(0.0,  2.0)), c = Tap(src, uv, float2(2.0,  2.0));
	const float3 d = Tap(src, uv, float2(-2.0,  0.0)), e = Tap(src, uv, float2(0.0,  0.0)), f = Tap(src, uv, float2(2.0,  0.0));
	const float3 g = Tap(src, uv, float2(-2.0, -2.0)), h = Tap(src, uv, float2(0.0, -2.0)), i = Tap(src, uv, float2(2.0, -2.0));
	const float3 j = Tap(src, uv, float2(-1.0,  1.0)), k = Tap(src, uv, float2(1.0,  1.0));
	const float3 l = Tap(src, uv, float2(-1.0, -1.0)), m = Tap(src, uv, float2(1.0, -1.0));

	float3 result;
	if (gFirst != 0)
	{
		// 다섯 묶음(안쪽 4개 0.5, 모서리 2×2 묶음 넷 0.125씩)마다 Karis 가중치를 곱해 정규화
		const float3 g0 = (j + k + l + m) * 0.25;
		const float3 g1 = (a + b + d + e) * 0.25;
		const float3 g2 = (b + c + e + f) * 0.25;
		const float3 g3 = (d + e + g + h) * 0.25;
		const float3 g4 = (e + f + h + i) * 0.25;
		const float w0 = KarisWeight(g0) * 0.5;
		const float w1 = KarisWeight(g1) * 0.125, w2 = KarisWeight(g2) * 0.125;
		const float w3 = KarisWeight(g3) * 0.125, w4 = KarisWeight(g4) * 0.125;
		result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
	}
	else
	{
		result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	}
	dst[id.xy] = float4(max(result, 0.0), 1.0);
}

// ── 블룸 ② 올리기 — 3×3 텐트 (가운데 4, 변 2, 모서리 1, ÷16) + 같은 단계 내리기 결과 ──
[numthreads(8, 8, 1)]
void CSBloomUpsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDstSize)) return;
	Texture2D<float4>   lower = ResourceDescriptorHeap[gSrcIndex];   // 아랫 단계 올린 결과 (반 크기)
	Texture2D<float4>   same  = ResourceDescriptorHeap[gAddIndex];   // 이 단계 내리기 결과
	RWTexture2D<float4> dst   = ResourceDescriptorHeap[gDstIndex];
	const float2 uv = (float2(id.xy) + 0.5) / float2(gDstSize);

	float3 tent = Tap(lower, uv, float2(0.0, 0.0)) * 4.0;
	tent += (Tap(lower, uv, float2(0.0, 1.0)) + Tap(lower, uv, float2(-1.0, 0.0)) +
	         Tap(lower, uv, float2(1.0, 0.0)) + Tap(lower, uv, float2(0.0, -1.0))) * 2.0;
	tent += Tap(lower, uv, float2(-1.0, 1.0)) + Tap(lower, uv, float2(1.0, 1.0)) +
	        Tap(lower, uv, float2(-1.0, -1.0)) + Tap(lower, uv, float2(1.0, -1.0));
	tent *= 1.0 / 16.0;
	dst[id.xy] = float4(same[id.xy].rgb + tent, 1.0);
}
