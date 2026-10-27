// ============================================================
//  ColorPyramid.hlsl — [7] 반투명 앞 «화면 색 피라미드» (컴퓨트 cs_6_6 · 바인드리스, 2026-10-27 렌더 9단계)
//
//  불투명 라이팅이 끝난 HDR 컬러를 복사하고 밉을 점점 흐리게 만든다. 얼음(투과 재질) 패스가 굴절된 자리의
//  색을 거칠기에 맞는 단계에서 읽는다 — HDRP 의 색 피라미드(_ColorPyramidTexture)와 같은 자리.
//    CSCopy        HDR → 밉 0 (같은 크기)
//    CSDownsample  밉 i−1 → 밉 i. 출력 텍셀마다 원본 4×4 영역을 쌍선형 4탭으로 덮는다(텐트 모양 흐림).
//  ★ 피라미드는 한 번만 뜬다 — 얼음 뒤의 얼음은 이 복사본에 없어서 안 보인다(10-27 선택지 (a), 노션 9단계).
// ============================================================

cbuffer PyramidCB : register(b0)
{
	uint   gSrcIndex;   uint gDstIndex;   // 바인드리스 번호 — 원본 SRV · 대상 UAV
	uint2  gDstSize;
	float2 gSrcTexel;   uint2 _pad0;      // 원본 텍셀 크기(1/폭, 1/높이)
};
SamplerState gClampSampler : register(s2);

[numthreads(8, 8, 1)]
void CSCopy(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDstSize)) return;
	Texture2D<float4>   src = ResourceDescriptorHeap[gSrcIndex];
	RWTexture2D<float4> dst = ResourceDescriptorHeap[gDstIndex];
	dst[id.xy] = float4(src[id.xy].rgb, 1.0);
}

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gDstSize)) return;
	Texture2D<float4>   src = ResourceDescriptorHeap[gSrcIndex];
	RWTexture2D<float4> dst = ResourceDescriptorHeap[gDstIndex];
	const float2 uv = (float2(id.xy) + 0.5) / float2(gDstSize);   // 출력 텍셀 중심 = 원본 2×2 의 가운데
	float3 sum = src.SampleLevel(gClampSampler, uv + gSrcTexel * float2(-1.0, -1.0), 0).rgb;
	sum += src.SampleLevel(gClampSampler, uv + gSrcTexel * float2( 1.0, -1.0), 0).rgb;
	sum += src.SampleLevel(gClampSampler, uv + gSrcTexel * float2(-1.0,  1.0), 0).rgb;
	sum += src.SampleLevel(gClampSampler, uv + gSrcTexel * float2( 1.0,  1.0), 0).rgb;
	dst[id.xy] = float4(sum * 0.25, 1.0);
}
