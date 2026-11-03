#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "../Physics/CollisionWorld.h"

// ============================================================
//  Shared/World/Placement.h — 예시 행성 배치 JSON 읽기 (2026-11-01, Client/Scene/PlanetDebugScene 에서 옮겼다)
//
//  사용자 지시(2026-11-01): «맵번호 적용해줘» — 서버도 예시 행성의 충돌체를 같은 배치로 씌워야 한다.
//  근거: 노션 「충돌 처리 ②」 3절 Shared 표 «배치 JSON 읽기 — PlanetDebugScene::ReadPlacement(클라) → Shared 로 옮긴다. 서버도 읽는다».
//
//  ★ 형식 (resource 브랜치 예시 행성 <행성>_Placement.json)
//    objects   = [{ id, name, file, ... }] — 종류별 glb(assets\model 기준 상대 경로, '/').
//    instances = [[id, x, y, z, qx, qy, qz, qw, scale], ...] — glTF 좌표 · 행성 중심 기준(오브젝트 원점 = 바닥 가운데). JSON 의 마지막 키.
//    terrain_layers = { channels: { R, G, B, A }, mask } — 지형 레이어(클라 렌더만 쓴다).
//  ★ 읽는 방법 — 머리말(instances 앞까지)만 최소 JSON 파서(Shared::JsonValue)로 읽고, instances 는 숫자만 직접 훑는다.
//    instances 를 값 트리로 만들면 37만 × 9 개 값이라 수백 MB 가 된다.
//  ★ 스레드 — 멤버가 없는 함수다. 클라 로딩 스레드 · 서버 맵 읽기 스레드에서 부른다.
// ============================================================

namespace Shared {

	struct PlacementData
	{
		struct Type
		{
			std::string name;
			std::string file;   // assets\model 기준 상대 경로 (JSON 그대로 — UTF-8, '/')
		};
		// 인스턴스 하나 — 충돌 월드가 받는 꼴과 같다(type = types 번호. JSON 의 id 가 아니다).
		using Instance = PlacedInstance;

		std::vector<Type>     types;
		std::vector<Instance> instances;
		size_t                skipped = 0;   // 모르는 id · 잘못된 숫자로 건너뛴 인스턴스 수
		std::wstring          error;         // 실패 원인

		// 지형 레이어 (2026-10-28) — JSON terrain_layers. channels 의 R G B A = 레이어 이름, mask = 마스크 PNG 이름.
		std::array<std::string, 4> terrainLayers;
		std::string                terrainMask;
	};

	// 배치 JSON 을 읽는다. 실패하면 false 이고 out.error 에 원인.
	bool ReadPlacement(const std::wstring& path, PlacementData& out);

	// 종류 이름 목록 — CollisionWorld::AddPlacement 의 typeNames.
	std::vector<std::string> PlacementTypeNames(const PlacementData& placement);

} // namespace Shared
