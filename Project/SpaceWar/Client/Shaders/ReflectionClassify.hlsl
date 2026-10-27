// ============================================================
//  ReflectionClassify.hlsl — [3] 분류 (컴퓨트 cs_6_6 · 바인드리스, 2026-10-27 렌더 11단계)
//
//  [4b] 반사 레이가 필요한 8×8 타일 목록을 GPU 가 만들고, 그 수로 DispatchRays 인자를 채운다.
//  GRenderer 가 이 인자로 ExecuteIndirect(DISPATCH_RAYS) 를 부른다 → 화면은 같고 띄우는 RayGen 스레드 수만 준다.
//  (TraceRay 는 예전에도 레이가 필요한 픽셀만 쐈다 — 줄어드는 것은 하늘 · 거친 면에서 0 만 쓰고 바로 끝나던 스레드다)
//  방법은 임시 선택(미정): AMD FidelityFX 의 타일 분류(8×8 타일, 거칠기 문턱 위는 레이 없음) 중
//  «타일 목록 + 간접 인자» 부분만. 픽셀 단위 레이 목록 · 사분면 레이 재사용 · 분산 기반 판정은 넣지 않았다.
//
//  그룹 하나 = 타일 하나 (8×8 스레드 = 64 픽셀):
//    ① 픽셀마다 NeedsReflectionRay (ReflectionTiles.hlsli — RayGen 과 같은 판정)
//    ② 하나도 없으면 → 그 타일 64 픽셀의 반사 텍스처에 0 을 쓴다(예전 RayGen 이 이 픽셀들에 쓰던 값과 같다)
//    ③ 하나라도 있으면 → 인자 버퍼의 Width 에 64 를 원자적으로 더하고, 더하기 전 값 / 64 번째 칸에 타일을 적는다
//       (Width 가 곧 «지금까지 모인 타일 수 × 64». 프레임 시작에 GRenderer 가 Width 0 인 원본을 복사해 둔다)
//  ③ 타일 안에서 레이가 필요 없는 픽셀(하늘 · 거친 면)은 RayGen 이 예전처럼 직접 0 을 쓴다.
// ============================================================

#include "ReflectionTiles.hlsli"

cbuffer ClassifyCB : register(b0)
{
	uint2 gSize;            float gMaxRoughness;   uint gDepthIndex;      // ↓ 바인드리스 번호
	uint  gMaterialIndex;   uint  gReflectionUav;  uint gTileListUav;     uint gArgsUav;
	uint  gArgsWidthOffset; uint3 _pad0;                                  // D3D12_DISPATCH_RAYS_DESC::Width 의 바이트 위치
};

groupshared uint gsNeedsRay;

[numthreads(REFLECTION_TILE_SIZE, REFLECTION_TILE_SIZE, 1)]
void CSClassifyTiles(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint gi : SV_GroupIndex)
{
	if (gi == 0) gsNeedsRay = 0;
	GroupMemoryBarrierWithGroupSync();

	// 오른쪽 · 아래 끝 타일은 화면 밖 스레드가 있다 — 판정 · 쓰기에서 뺀다
	const bool inside = all(id.xy < gSize);
	if (inside)
	{
		Texture2D<float>  depthTex    = ResourceDescriptorHeap[gDepthIndex];
		Texture2D<float4> materialTex = ResourceDescriptorHeap[gMaterialIndex];
		if (NeedsReflectionRay(depthTex[id.xy], materialTex[id.xy].r, gMaxRoughness))
			InterlockedOr(gsNeedsRay, 1u);
	}
	GroupMemoryBarrierWithGroupSync();

	if (gsNeedsRay == 0)
	{
		if (inside)
		{
			RWTexture2D<float4> output = ResourceDescriptorHeap[gReflectionUav];
			output[id.xy] = float4(0.0, 0.0, 0.0, 0.0);   // 레이 없음 — 라이팅 · 노이즈 제거가 읽는 값은 예전과 같다
		}
		return;
	}

	if (gi == 0)
	{
		RWByteAddressBuffer      args  = ResourceDescriptorHeap[gArgsUav];
		RWStructuredBuffer<uint> tiles = ResourceDescriptorHeap[gTileListUav];
		uint before;
		args.InterlockedAdd(gArgsWidthOffset, kReflectionTilePixels, before);
		tiles[before / kReflectionTilePixels] = PackReflectionTile(group.xy);
	}
}
