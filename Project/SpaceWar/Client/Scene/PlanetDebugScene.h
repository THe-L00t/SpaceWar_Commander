#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "Client/Scene/Scene.h"
#include "Client/Model.h"
#include "Client/Vec3d.h"
#include "Shared/Terrain/PlanetSurface.h"

// ============================================================
//  Client/Scene/PlanetDebugScene.h — 예시 행성 디버그 씬 (2026-10-28)
//
//  resource 브랜치에서 만든 예시 행성 5개를 하나씩 띄워 보는 임시 디버깅용 씬.
//  사용자 지시(2026-10-28): «행성별 씬 1~5, 임시 디버깅용이니 최대한 가볍게, 숫자키 1~5 로 바로 전환»,
//  이어서 «오브젝트는 json 읽어서 안 가져와? 그것도 작업해줘».
//  숫자키 처리와 맵 바꿔 끼우기는 Engine 이 한다(Engine::HandleDebugPlanetKeys · EnterDebugPlanet).
//
//  ★ 맵 glb + 배치 JSON(Placement.json)의 오브젝트
//    맵 = 지형 · 보행로 · 착륙장 · 유물 자리 · 용암 · 얼어붙은 호수.
//    오브젝트 = JSON 의 objects(종류별 glb)와 instances([id, x, y, z, qx, qy, qz, qw, scale], glTF 좌표 · 행성 중심 기준).
//  ★ 오브젝트는 «화면에 크게 보이는 것부터» 예산만큼만 장면에 올린다 (StreamObjects)
//    행성마다 8만 ~ 37만 개인데 렌더러 상한이 패스당 인스턴스 8192 · TLAS 4096 이다(GRenderer.cpp Impl).
//    전부 넣으면 상한을 넘는 것은 버려지고(RT 에서는 4096 뒤가 사라짐), 노드 수백만 개를 매 프레임 훑게 된다.
//    그래서 점수 = 반지름 × 배율 ÷ 거리(화면 크기 근사)가 큰 순으로 메시 kObjectMeshBudget 개까지만 놓는다.
//    큰 건물 · 바위는 멀리서도, 풀은 가까이서만 보인다. kRestreamDistance 만큼 움직이면 다시 고른다.
//    한 번 만든 노드는 종류별로 숨겨 두었다가 다시 쓴다(LegacyScene 에 노드 삭제가 없다).
//  ★ 게임 씬과 같은 플레이어 · 카메라 · LegacyScene 을 쓴다
//    게임 씬이 아직 Engine 안에 있어서(Engine.h «아직 자리로 옮기지 않은 것») 이 씬은
//    «맵 + 접지면 + 스폰 방향 + 배치» 만 들고, 들어갈 때 Engine 이 맵과 접지면을 바꿔 끼운다.
//  ★ 한 번 올린 행성은 지우지 않고 숨겨 둔다
//    렌더러에 GPU 자원을 지우는 API 가 없다. 다시 고르면 다시 읽지 않고 보이기만 바꾼다(VRAM 은 고른 행성 수만큼 쌓인다).
//  ★ 좌표는 게임 맵과 같은 규약이다 — glb 원점 = 행성 중심, 기준 반경 650 m, 중심 (0,-R,0) 에 놓는다(PlanetConst.h).
//    접지 노드 이름 규칙(COL_Expedition_Planet_Terrain · Walkable_Path_* · Landing_L*_Platform)도 같아서
//    PlanetSurface::IsGroundObject 를 그대로 쓴다. 오브젝트는 접지면에 넣지 않는다(밟을 수 없다).
//  ★ 메인 스레드 소유. 소유자는 SceneManager 다(명세 5절). ReadPlacement 만 로딩 스레드에서 부른다.
//  ★ 지형 레이어 (2026-10-28, 사용자 «렌더러 밖 수정 이번만 허용» — 지형 레이어 작업)
//    glb 의 지형 재질은 대체 레이어 한 장뿐이다(리소스 규격). 배치 JSON 의 terrain_layers 로 레이어 4종 + 마스크의
//    밉맵 DDS 를 읽어(LoadTerrainLayers, 로딩 스레드) 렌더러 지형 재질을 만들고 지형 노드의 재질을 바꿔 끼운다
//    (ApplyTerrainLayers, 메인). 못 읽으면 대체 레이어 한 장 그대로 띄운다.
//  ★ 땅 위 메시 경계 섞기 (2026-10-29, 사용자 «용암 행성에서도 길이 경계가 뚜렷하다 수정» ·
//    «얼음 호수 경계도 뚜렷해. 모든 메시 경계도 흐릿하게 섞어줘»)
//    · 오솔길(Walkable_Path_*) · 착륙 대지(Landing_L*_Platform) 중 glb 재질이 «지형과 같은 땅»(지형 재질 · 레이어 이름 ·
//      행성 표의 길 재질)인 것은 지형 레이어 재질로 바꿔 그린다 — 마스크를 따라 지형과 이어진다.
//      리소스 규격: 빙하 trail_note «렌더러가 지형과 같은 레이어 셰이더로 그리면 띠가 따로 보이지 않는다 … 착륙 대지도 같다»,
//      화산 build_map.py «오솔길 · 착륙 대지 = 주변과 같은 굳은 껍질(티 나지 않게)», 숲 시안은 착륙 원판을 지형 재질로 그린다.
//      네온 · 폐허의 도로 · 포장(따로 만든 재질)은 그대로 둔다.
//    · 얼어붙은 호수(Frozen_Lake*)는 물가 페이드를 켠다 — 바닥에 가까운 가장자리에서 얼음이 옅어진다(렌더러 반투명 패스).
// ============================================================

namespace swc {

	class GRenderer;
	class LegacyScene;

	// 배치 JSON 을 읽은 결과 — 로딩 스레드가 채우고 메인이 PlanetDebugScene::SetPlacement 로 넘겨받는다.
	struct PlanetPlacement
	{
		struct Type
		{
			std::string name;
			std::string file;   // assets\model 기준 상대 경로 (JSON 그대로 — UTF-8, '/')
		};
		struct Instance
		{
			uint32_t type = 0;              // types 번호 (JSON 의 id 가 아니다)
			float    position[3] = {};      // glTF 좌표, 행성 중심 기준 (오브젝트 원점 = 바닥 가운데)
			float    rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // 쿼터니언 x y z w
			float    scale = 1.0f;          // 균일 배율
		};

		std::vector<Type>     types;
		std::vector<Instance> instances;
		size_t                skipped = 0;   // 모르는 id · 잘못된 숫자로 건너뛴 인스턴스 수
		std::wstring          error;         // 실패 원인 (로딩 스레드 → 메인)

		// 지형 레이어 (2026-10-28) — JSON terrain_layers. channels 의 R G B A = 레이어 이름, mask = 마스크 PNG 이름.
		std::array<std::string, 4> terrainLayers;
		std::string                terrainMask;
	};

	// 지형 레이어 텍스처 — 로딩 스레드가 DDS 를 읽어 채우고, 메인이 PlanetDebugScene::ApplyTerrainLayers 로 GPU 에 올린다(2026-10-28).
	struct TerrainLayerSet
	{
		TextureData  baseColor;   // 배열 4장(마스크 R G B A 순) — 아래 셋도 같다
		TextureData  normal;
		TextureData  orm;
		TextureData  height;
		TextureData  mask;        // RGBA 한 장
		std::wstring error;       // 실패 원인 (로딩 스레드 → 메인)
	};

	class PlanetDebugScene : public Scene
	{
	public:
		static constexpr size_t kCount = 5;

		// 장면에 올리는 오브젝트 메시 노드 상한. TLAS 4096 에서 맵(최대 약 90) · 캐릭터 · 예광탄 몫을 뺀 값.
		static constexpr size_t kObjectMeshBudget = 3500;
		// 마지막으로 고른 자리에서 이만큼(m) 움직이면 다시 고른다.
		static constexpr double kRestreamDistance = 15.0;

		// 0 ~ 4 = 숫자키 1 ~ 5. 순서 = 만든 순서(Verdant · Neon · Ruin · Volcanic · Glacier).
		static const wchar_t* Name(size_t index);
		// exe 옆 assets\ 기준. ★ Client.vcxproj 의 CopyModelAssets 에도 적혀 있다 — 바꿀 때 두 곳.
		static const wchar_t* Asset(size_t index);
		static const wchar_t* PlacementAsset(size_t index);

		// 로딩 스레드에서 부른다(멤버를 건드리지 않는다). 실패하면 out.error 에 원인.
		static bool ReadPlacement(const std::wstring& path, PlanetPlacement& out);

		bool Loaded() const { return loaded; }

		// 메인 스레드. 로딩 스레드가 파싱한 맵을 GPU 에 올리고 LegacyScene 에 «숨긴 채로» 놓는다.
		// ground = 같은 파싱에서 로딩 스레드가 만든 접지면(넘겨받는다).
		bool Build(const ModelData& data, Shared::PlanetSurface&& ground, GRenderer& renderer,
			LegacyScene& scene, const Vec3d& planetCenter, std::wstring& error);

		// ── 배치 오브젝트 (메인 스레드) ──
		void SetPlacement(PlanetPlacement&& placement);
		size_t ObjectTypeCount() const { return objectTypes.size(); }
		// exe 옆 assets\ 기준 경로(L"model\\prop\\...\\X.glb")
		std::wstring ObjectAsset(size_t type) const;
		// 종류 하나의 모델을 GPU 에 올린다. 실패한 종류의 인스턴스는 놓지 않는다.
		bool AddObjectModel(size_t type, const ModelData& data, GRenderer& renderer, std::wstring& error);

		// 매 프레임. eye = 행성 중심 기준 위치(glTF 좌표 = 월드 − 행성 중심).
		// 처음 · force · kRestreamDistance 이상 움직였을 때만 다시 고른다. 다시 골랐으면 true.
		bool StreamObjects(LegacyScene& scene, const Vec3d& eye, bool force);

		// ── 지형 레이어 (2026-10-28) ──
		// 읽을 DDS 의 exe 옆 assets\ 기준 경로 17개 — 색 4 · 노멀 4 · ORM 4 · 높이 4(각각 마스크 R G B A 순) + 마스크.
		//  JSON 에 terrain_layers 가 없으면 빈 목록.
		static std::vector<std::wstring> TerrainLayerAssets(size_t index, const PlanetPlacement& placement);
		// 로딩 스레드에서 부른다(멤버를 건드리지 않는다). paths = 위 목록을 실제 경로로 바꾼 것. 실패하면 out.error.
		static bool LoadTerrainLayers(const std::vector<std::wstring>& paths, TerrainLayerSet& out);
		// 메인 스레드. Build 뒤에 부른다 — 텍스처 5개를 올려 지형 재질을 만들고 지형 그리기 노드의 재질을 바꾼다.
		bool ApplyTerrainLayers(size_t index, TerrainLayerSet&& layers, GRenderer& renderer, LegacyScene& scene,
			std::wstring& error);

		// 창 제목용 — «지형 N 삼각형 · 지형 레이어 섞기 / 대체 1장 · 오브젝트 보임 k / 배치 M · 종류 a/b»
		std::wstring Status() const;

		const Shared::PlanetSurface& Ground() const { return ground; }
		NodeHandle Root() const { return root; }

		// 스폰 방향(행성 중심 기준 단위 벡터) = 착륙 지점 L1. 게임 맵과 같은 규칙이다(PlanetConst.h kPlanetSpawnUp).
		const Vec3d& SpawnUp() const { return spawnUp; }

	private:
		struct ObjectType
		{
			std::string             name;
			std::string             file;
			Model                   model;
			float                   radius = 0.0f;     // 모델 바운드의 반지름(m) — 점수에 쓴다
			uint32_t                meshCount = 0;     // 인스턴스 하나가 차지하는 메시 노드 수
			bool                    loaded = false;
			std::vector<NodeHandle> freeSlots;         // 숨겨 둔 인스턴스 루트(다시 쓴다)
		};

		Model                 model;
		Shared::PlanetSurface ground;
		NodeHandle            root = kInvalidNode;
		Vec3d                 spawnUp{ 0.0, 1.0, 0.0 };
		bool                  loaded = false;
		std::vector<NodeHandle> terrainNodes;          // 지형 메시(…_Planet_Terrain)의 그리기 노드 — 지형 레이어 재질로 바꾼다
		bool                  terrainLayered = false;  // 지형 레이어 재질을 끼웠는가(창 제목)
		// ── 땅 위 메시 경계 섞기 (2026-10-29) ──
		//  오솔길 · 착륙 대지 중 «지형과 같은 땅» 재질인 것도 지형 레이어 재질로 그린다(ApplyTerrainLayers).
		Vec3d                 center{ 0.0, 0.0, 0.0 };              // 월드의 행성 중심 — 지형 레이어 좌표의 원점
		std::string           terrainMaterialName;                  // glb 지형 노드의 재질 이름(대체 레이어)
		std::array<std::string, 4> terrainLayerNames;               // 배치 JSON terrain_layers 의 R G B A
		std::vector<std::pair<NodeHandle, std::string>> overlayNodes;   // (그리기 노드, glb 재질 이름) — 오솔길 · 착륙 대지
		size_t                layeredOverlays = 0;                  // 그중 지형 레이어 재질로 바꾼 수(창 제목)
		size_t                fadedLakes = 0;                       // 물가 페이드를 켠 얼음 호수 노드 수(창 제목)

		std::vector<ObjectType>                 objectTypes;
		std::vector<PlanetPlacement::Instance>  instances;
		size_t                                  skippedInstances = 0;
		std::vector<NodeHandle>                 liveSlot;      // 인스턴스 → 보이는 루트(없으면 kInvalidNode)
		std::vector<uint32_t>                   liveList;      // 지금 보이는 인스턴스 번호
		std::vector<uint32_t>                   selectStamp;   // 이번에 골랐는가(stamp 와 같으면)
		uint32_t                                stamp = 0;
		std::vector<std::pair<float, uint32_t>> scratch;       // (점수, 인스턴스) — 고를 때마다 다시 쓴다
		Vec3d                                   lastStreamEye;
		bool                                    streamed = false;
	};

} // namespace swc
