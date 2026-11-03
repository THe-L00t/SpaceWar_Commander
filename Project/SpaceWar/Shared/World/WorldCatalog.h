#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstddef>
#include <cstdint>
#include "../PlanetConst.h"

// ============================================================
//  Shared/World/WorldCatalog.h — 맵 번호 · 맵 파일 목록 (2026-11-01)
//
//  사용자 지시(2026-11-01): «맵번호 적용해줘».
//  근거: 노션 「충돌 처리 ②」 0절 사용자 결정 1번 «예시행성돠 게임맵 둘 다» · 4번 «B안으로 전면수정»,
//        같은 페이지 3절 계획(서버 맵 = 게임 맵 + 예시 행성 5개, 세션마다 맵 번호, JoinWorld(worldId)).
//
//  ★ 맵 번호 — 0 = 게임 맵, 1 ~ 5 = 예시 행성(클라 숫자키 1 ~ 5 = PlanetDebugScene 0 ~ 4)
//    서버는 세션마다 맵 번호를 들고, 같은 맵에 있는 사람끼리만 판정 · 송신한다(Server/main.cpp).
//    클라는 행성을 바꿀 때 JoinWorld(맵 번호)를 보내고, 서버의 WorldJoined 를 받은 뒤부터 입력을 보낸다.
//  ★ 파일 경로는 exe 옆 assets\ 기준이다. 클라(PlanetDebugScene)와 서버가 이 한 곳을 본다.
//    ★ Client.vcxproj CopyModelAssets · Server.vcxproj CopyPlanetSurfaceAsset 에도 적혀 있다 — 바꿀 때 세 곳.
//  ★ 모든 맵의 좌표 규약이 같다 — glb 원점 = 행성 중심, 기준 반경 650 m, 중심 (0, −R, 0)(PlanetConst.h).
//    그래서 맵마다 다른 것은 접지면 · 충돌체 · 스폰 방향뿐이다.
// ============================================================

namespace Shared {

	inline constexpr uint32_t kWorldGameMap = 0;
	inline constexpr uint32_t kWorldCount = 6;
	inline constexpr uint32_t kNoWorld = 0xFFFFFFFFu;   // 아직 어느 맵에도 들어가지 않았다(들어가는 중 · 실패)

	// 예시 행성의 스폰 = 착륙 지점 L1 플랫폼 노드의 행성 중심 기준 방향(게임 맵은 kPlanetSpawnUp 상수).
	//  예시 행성은 L1 방향이 게임 맵과 달라 맵 노드에서 읽는다(resource fa6777c 커밋의 «주의»).
	inline constexpr const char* kWorldSpawnNode = "Landing_L1_Platform";

	struct WorldInfo
	{
		const char*    name;                // 로그용 이름
		const wchar_t* map;                 // 맵 glb (접지면 · 스폰 노드)
		const wchar_t* placement;           // 배치 JSON — 예시 행성만. 게임 맵은 nullptr(오브젝트가 맵 glb 안에 있다)
		const wchar_t* collision;           // 정적 오브젝트 충돌체 묶음 glb (Resource_Work/_tools/collision/col_build.py)
		const wchar_t* collisionManifest;   // 그 설명 JSON
	};

	inline constexpr WorldInfo kWorlds[kWorldCount] = {
		{ "GameMap",  kPlanetModelAsset, nullptr, kPlanetCollisionAsset, kPlanetCollisionManifestAsset },
		{ "Verdant",  L"model\\map\\Verdant_Planet\\Verdant_Planet.glb",   L"model\\map\\Verdant_Planet\\Verdant_Planet_Placement.json",
		              L"model\\map\\Verdant_Planet\\Verdant_Planet_Collision.glb",   L"model\\map\\Verdant_Planet\\Verdant_Planet_Collision.json" },
		{ "Neon",     L"model\\map\\Neon_Planet\\Neon_Planet.glb",         L"model\\map\\Neon_Planet\\Neon_Planet_Placement.json",
		              L"model\\map\\Neon_Planet\\Neon_Planet_Collision.glb",         L"model\\map\\Neon_Planet\\Neon_Planet_Collision.json" },
		{ "Ruin",     L"model\\map\\Ruin_Planet\\Ruin_Planet.glb",         L"model\\map\\Ruin_Planet\\Ruin_Planet_Placement.json",
		              L"model\\map\\Ruin_Planet\\Ruin_Planet_Collision.glb",         L"model\\map\\Ruin_Planet\\Ruin_Planet_Collision.json" },
		{ "Volcanic", L"model\\map\\Volcanic_Planet\\Volcanic_Planet.glb", L"model\\map\\Volcanic_Planet\\Volcanic_Planet_Placement.json",
		              L"model\\map\\Volcanic_Planet\\Volcanic_Planet_Collision.glb", L"model\\map\\Volcanic_Planet\\Volcanic_Planet_Collision.json" },
		{ "Glacier",  L"model\\map\\Glacier_Planet\\Glacier_Planet.glb",   L"model\\map\\Glacier_Planet\\Glacier_Planet_Placement.json",
		              L"model\\map\\Glacier_Planet\\Glacier_Planet_Collision.glb",   L"model\\map\\Glacier_Planet\\Glacier_Planet_Collision.json" },
	};

	// 예시 행성 디버그 씬 번호(0 ~ 4) → 맵 번호(1 ~ 5). 범위 밖이면 kNoWorld.
	inline constexpr uint32_t WorldOfDebugPlanet(size_t index)
	{
		return index + 1 < kWorldCount ? static_cast<uint32_t>(index + 1) : kNoWorld;
	}

	inline constexpr const char* WorldName(uint32_t world)
	{
		return world < kWorldCount ? kWorlds[world].name : "None";
	}

} // namespace Shared
