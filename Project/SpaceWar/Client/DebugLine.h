#pragma once
#include <cstdint>
#include <DirectXMath.h>

// ============================================================
//  Client/DebugLine.h — 디버그 선 정점 (2026-10-29)
//
//  GRenderer::DrawDebugLines 가 받는 정점. 두 개가 선 하나다(선 목록).
//  처음 쓰는 곳: C 키 충돌체 와이어프레임(Client/DebugView/ColliderDebugView).
//  사용자 지시(2026-10-29): «저폴리로 충돌체 만들면 c키를 눌러 저폴리 충돌체를 와이어 프레임으로 볼 수있게 해줘».
//
//  ★ position 은 «원점 기준» 좌표다 — DrawDebugLines 의 origin(월드)을 더하면 월드가 된다.
//    행성 중심 같은 큰 값을 정점마다 넣지 않으려는 것(float 정밀도). 렌더러는 origin − eye 를 double 로 구해 넘긴다.
//  ★ color 는 RGBA8(메모리 순서 R, G, B, A — DXGI_FORMAT_R8G8B8A8_UNORM). 0xAABBGGRR 꼴이다.
// ============================================================

namespace swc {

	struct DebugLineVertex
	{
		DirectX::XMFLOAT3 position;
		uint32_t          color = 0xFFFFFFFFu;
	};
	static_assert(sizeof(DebugLineVertex) == 16, "DebugLines.hlsl 입력 배치와 맞춘다");

	constexpr uint32_t DebugColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
	{
		return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
	}

} // namespace swc
