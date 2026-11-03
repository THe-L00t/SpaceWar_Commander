// ============================================================
//  ReflectionTiles.hlsli — [3] 분류 · [4b] 반사가 같이 쓰는 «반사 레이가 필요한가» 와 타일 번호 규칙
//  (2026-10-27 렌더 11단계)
//
//  ReflectionClassify.hlsl 이 8×8 타일마다 «레이가 필요한 픽셀이 하나라도 있나» 를 보고 타일 목록을 만들고,
//  RtReflection.hlsl RayGen 이 그 목록으로 픽셀을 찾는다. 두 쪽의 판정 · 번호 규칙이 어긋나면
//  «레이를 쏴야 할 픽셀이 빠지거나 0 이 안 써진 픽셀이 남는» 화면 차이가 생기므로 여기 한 곳에만 둔다.
// ============================================================

#ifndef REFLECTION_TILES_HLSLI
#define REFLECTION_TILES_HLSLI

#include "Shading.hlsli"   // kMinRoughness

// 타일 한 변(픽셀). ReflectionClassify.hlsl 의 numthreads 와 GRenderer.cpp 의 Impl::kReflectionTileSize 가 같아야 한다.
#define REFLECTION_TILE_SIZE 8
static const uint kReflectionTilePixels = REFLECTION_TILE_SIZE * REFLECTION_TILE_SIZE;   // 타일 하나 = 레이 64개 자리
static const uint kNoTileList = 0xFFFFFFFF;   // ReflectionCB.gTileListIndex 가 이 값이면 예전처럼 화면 전체로 돈다

// 이 픽셀에 반사 레이가 필요한가 — 메인 패스 물체가 있고(깊이 < 1) 거칠기가 상한보다 낮다.
//  rawRoughness 는 G-buffer GB2.r 그대로. 라이팅 · RayGen 과 같은 최소 거칠기로 자른 뒤 비교한다.
bool NeedsReflectionRay(float depth, float rawRoughness, float maxRoughness)
{
	return depth < 1.0 && max(rawRoughness, kMinRoughness) < maxRoughness;
}

// 타일 좌표 ↔ 목록 한 칸(uint). 아래 16비트 x, 위 16비트 y (화면 65536 타일까지).
uint PackReflectionTile(uint2 tile)
{
	return tile.x | (tile.y << 16);
}

// 목록 한 칸 + 타일 안 순번(0 ~ 63, 줄 순서) → 픽셀 좌표
uint2 ReflectionTilePixel(uint packedTile, uint lane)
{
	const uint2 tile = uint2(packedTile & 0xFFFF, packedTile >> 16);
	return tile * REFLECTION_TILE_SIZE + uint2(lane % REFLECTION_TILE_SIZE, lane / REFLECTION_TILE_SIZE);
}

#endif
