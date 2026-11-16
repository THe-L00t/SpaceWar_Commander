#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <string>

// ============================================================
//  Shared/World/WorldMap.h — 서버가 맵 하나의 접지면 · 스폰 방향을 읽는다 (2026-11-01)
//
//  사용자 지시(2026-11-01): «맵번호 적용해줘» — 서버가 예시 행성도 판정한다(Shared/World/WorldCatalog.h).
//
//  ★ 클라와 같은 것을 얻는다
//    · 접지면 = PlanetSurface::IsGroundObject 로 모은 면(클라는 렌더용 파싱 한 번에 같은 규칙으로 모은다).
//    · 스폰 방향 = spawnNode 노드의 행성 중심 기준 위치의 방향(클라 PlanetDebugScene::Build 와 같은 계산 —
//      노드 월드 = 로컬 · 부모 월드, 이동 성분). 노드가 없거나 원점에서 1 m 안이면 게임 맵의 L1 방향(kPlanetSpawnUp).
//      spawnNode 가 nullptr 이면(게임 맵) 노드를 찾지 않고 kPlanetSpawnUp 을 쓴다 — 클라 게임 맵과 같다.
//  ★ 렌더 정보(재질 · 텍스처 · UV)는 읽지 않는다(ReadOptions::geometryOnly).
//  ★ 스레드 — 멤버가 없는 함수다. 서버 맵 읽기 스레드에서 부른다.
// ============================================================

namespace Shared {

	class PlanetSurface;

	// path = 맵 glb 의 실제 경로. 성공하면 surface 를 채우고 spawnUp(단위 벡터)을 돌려준다.
	//  spawnFromNode = 스폰 방향을 노드에서 찾았는가(못 찾았으면 kPlanetSpawnUp).
	bool LoadWorldGround(const wchar_t* path, const char* spawnNode, PlanetSurface& surface,
		double spawnUp[3], bool& spawnFromNode, std::wstring& error);

} // namespace Shared
