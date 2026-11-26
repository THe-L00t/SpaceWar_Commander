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
#include "Shared/Physics/CollisionWorld.h"
#include "Shared/Terrain/PlanetSurface.h"
#include "Shared/World/Placement.h"
#include "Shared/GameObject/StaticObject.h"

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
//    그래서 점수 = 반지름 × 배율 ÷ 거리(화면 크기 근사)가 큰 순으로 메시 예산(budget)까지만 놓는다.
//    ★ 2026-11-17 render/tlas-select — 고정 3,500(kObjectMeshBudget)을 없앴다(사용자 «레스터 제한 풀어버려»).
//      예산 = 렌더러 래스터 상한(패스당 8,192) − 다른 래스터 몫 − 여유(Engine 이 넘긴다). TLAS 는 SelectTlasObjects 가 따로 고른다.
//    ★ 2026-11-17 — 래스터 순서도 TLAS 와 같은 순위(RankObjects: 60 m 안 전부 + 조준 32 px → 단계 → px)로 바꿨다
//      (사용자 «우선 TLAS 하되, 컬링은 그대로 유지해줘»). 위 «점수» 순서는 더 쓰지 않는다. 공간 격자는 로드 때 만든다.
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
//  ★ 충돌체 (2026-10-29, 사용자 «… 나머진은 저폴리로 충돌 가자. … c키를 눌러 저폴리 충돌체를 와이어 프레임으로»)
//    맵 glb 옆 «<행성>_Collision.glb · .json»(Resource_Work/_tools/collision/col_build.py)을 로딩 스레드에서 읽어
//    배치 인스턴스를 씌운 Shared::CollisionWorld 를 들고 있다(SetCollision). C 키 와이어프레임 · 이동 예측(PlayerController)이 쓴다.
//  ★ 서버 판정 (2026-11-01, 사용자 «맵번호 적용해줘») — 이 행성은 맵 번호 1 ~ 5(Shared/World/WorldCatalog.h).
//    들어갈 때 Engine 이 서버에 JoinWorld 를 보내고, 서버가 같은 맵 glb · 배치 JSON · 충돌체로 판정한다(Server/main.cpp).
//    경로(Asset · PlacementAsset · Collision*)는 Shared::kWorlds 한 곳에서 읽는다.
//  ★ 배치 오브젝트 = StaticObject (2026-11-02, 사용자 «지금 JSON을 기준으로 읽어오는 오브젝트를 스태틱오브젝트 생성해서
//    메쉬를 연결해줘») — 명세 6.5 · 6.10 «Scene 이 StaticObject 를 든다».
//    JSON 인스턴스마다 Shared::StaticObject 하나(SetPlacement). 필드는 이렇게 채운다:
//      position · scale = JSON 값, rotation = JSON 쿼터니언(정규화), direction = 그 회전으로 돌린 glTF 앞쪽 +Z,
//      meshIndex = 배치 종류 번호 → objectTypes[meshIndex].model 이 그 종류의 GPU 메시 묶음이다
//        (리소스 RAM 사본은 GPU 업로드 뒤 버리므로 Resource Manager 핸들 대신 종류 번호로 잇는다 — 명세 수정기록 2026-11-02),
//      renderObjectId = 장면에 올린 노드(LegacyScene — Render World 몫). 장면에 없으면 kInvalidNode.
//    장면 노드의 변환 = StaticObject 의 scale · rotation · position(InstanceMatrix).
//    (처음엔 명세 Object 에 회전이 없어 rotations[] 에 따로 두었다가, 같은 날 명세 6.3 에 Rotation 이 들어가 Object 로 옮겼다.)
// ============================================================

namespace swc {

	class GRenderer;
	class LegacyScene;

	// 배치 JSON 을 읽은 결과 — 로딩 스레드가 채우고 메인이 PlanetDebugScene::SetPlacement 로 넘겨받는다.
	//  2026-11-01 — 서버도 같은 배치로 충돌체를 씌우므로 Shared 로 옮겼다(Shared/World/Placement.h). 필드는 그대로다.
	using PlanetPlacement = Shared::PlacementData;

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

		// (2026-11-17 render/tlas-select — 고정 상한 kObjectMeshBudget = 3500 은 없앴다. StreamObjects 의 budget 인자.)
		// 마지막으로 고른 자리에서 이만큼(m) 움직이면 다시 고른다.
		static constexpr double kRestreamDistance = 15.0;

		// 0 ~ 4 = 숫자키 1 ~ 5. 순서 = 만든 순서(Verdant · Neon · Ruin · Volcanic · Glacier).
		static const wchar_t* Name(size_t index);
		// exe 옆 assets\ 기준 — Shared::kWorlds(WorldCatalog.h). ★ Client · Server.vcxproj 의 복사 목록에도 적혀 있다.
		static const wchar_t* Asset(size_t index);
		static const wchar_t* PlacementAsset(size_t index);
		// 충돌체 묶음 · 설명 JSON — 맵 glb 이름에 «_Collision» 을 붙인 것(Shared::kWorlds).
		static std::wstring CollisionAsset(size_t index);
		static std::wstring CollisionManifestAsset(size_t index);

		// 로딩 스레드에서 부른다(멤버를 건드리지 않는다). 실패하면 out.error 에 원인.
		static bool ReadPlacement(const std::wstring& path, PlanetPlacement& out);

		bool Loaded() const { return loaded; }

		// 메인 스레드. 로딩 스레드가 파싱한 맵을 GPU 에 올리고 LegacyScene 에 «숨긴 채로» 놓는다.
		// ground = 같은 파싱에서 로딩 스레드가 만든 접지면(넘겨받는다).
		bool Build(const ModelData& data, Shared::PlanetSurface&& ground, GRenderer& renderer,
			LegacyScene& scene, const Vec3d& planetCenter, std::wstring& error);

		// ── 행성 내리기 (2026-11-17, 사용자 «이전에 있던 행성만 남기고, 더 오래된 행성은 해제») — 메인 스레드 · 프레임 밖 ──
		//  이 행성의 GPU 자원(맵 · 오브젝트 · 지형 레이어)을 놓고, 장면 노드를 비우고, CPU 쪽(접지면 · 충돌체 · 배치)도 비운다.
		//  끝나면 Loaded() = false — 다시 고르면 처음처럼 로드 묶음으로 읽는다. 지금 보이는 행성에는 부르지 않는다(Engine 이 고른다).
		//  GPU 놓기에 실패하면 false(장면 · CPU 는 그래도 비운다).
		bool Release(GRenderer& renderer, LegacyScene& scene);

		// ── 배치 오브젝트 (메인 스레드) ──
		void SetPlacement(PlanetPlacement&& placement);
		size_t ObjectTypeCount() const { return objectTypes.size(); }
		// exe 옆 assets\ 기준 경로(L"model\\prop\\...\\X.glb")
		std::wstring ObjectAsset(size_t type) const;
		// 종류 하나의 모델을 GPU 에 올린다. 실패한 종류의 인스턴스는 놓지 않는다.
		bool AddObjectModel(size_t type, const ModelData& data, GRenderer& renderer, std::wstring& error);
		// 2026-11-17 — 종류 하나의 로드가 끝났다(올렸든 못 올렸든). 마지막 종류면 공간 격자를 만든다(로드 때 미리).
		void ObjectModelFinished();

		// 매 프레임. eye = 행성 중심 기준 위치(glTF 좌표 = 월드 − 행성 중심).
		// 처음 · force · kRestreamDistance 이상 움직였을 때만 다시 고른다. 다시 골랐으면 true.
		// ★ 2026-11-16 — 눈의 반대편 반구(행성 중심 기준 내적 < 0)에 있는 오브젝트는 고르지 않는다(사용자 지시).
		// ★ 2026-11-17 — budget = 메시 노드 상한(Engine: 래스터 상한 − 다른 몫 − 여유). 줄어 넘치거나 64 이상 늘어도 다시 고른다.
		bool StreamObjects(LegacyScene& scene, const Vec3d& eye, bool force, size_t budget);
		size_t StreamBudget() const { return lastStreamBudget; }

		// ── 래스터 칸 리스트 (2026-11-17 render/tlas-select — StreamObjects 대신, 장면 노드 없이) ──
		//  사용자: «재구성 범위 단위별로 리스트 만들어 두고, 해당 리스트들만 불러오는건» · «플레이어 기준 먼 격자는 px기준으로
		//  조금씩 잘라서» · «임시 리스트 하나 둬서 플레이어가 속한 격자인데 안그려지는거는 임시 리스트에 전부» · «문턱은 4m» ·
		//  «리스트 하나에 최대 8192» + 결정(칸 거리마다 문턱 올리기 · 앞쪽 반구 전체 · 큰 것은 px 로 자른 뒤 8,192 까지).
		//  공간 격자(128 m 칸)로 앞쪽 반구 전체를 본다. 링 = 플레이어 칸과의 칸 거리(0 = 플레이어 칸).
		//   링별 px 문턱(조준 기준): 0 · 8 · 16 · 그 밖 32 — 밑은 그리지 않는다.
		//   지름 4 m 미만 → 링 리스트(링마다 메시 8,192 씩), 4 m 이상 → 큰 리스트(px 큰 순 8,192 까지).
		//   임시 리스트 = 플레이어 칸에서 칸 리스트가 8,192 를 넘어 못 넣은 것 + 큰 리스트에서 잘린 큰 것.
		//  플레이어 칸이 바뀌었거나 force 일 때만 다시 만든다(만들었으면 true). 리스트는 렌더러가 패스 자원 하나씩으로 그린다.
		bool BuildRasterLists(const Vec3d& eye, bool force);
		const std::vector<InstanceList>& RasterLists() const { return rasterLists; }
		size_t RasterSmallMeshes() const { return rasterSmallMeshes; }
		size_t RasterBigMeshes() const { return rasterBigMeshes; }
		size_t RasterTempMeshes() const { return rasterTempMeshes; }
		size_t RasterCutBigMeshes() const { return rasterCutBigMeshes; }
		double LastRasterBuildMs() const { return lastRasterBuildMs; }
		// 2026-11-20 메시 LOD 시험 — 단계를 이은 오브젝트 종류 수 · LOD0 메시 수(측정 로그)
		size_t LodObjectTypes() const { size_t n = 0; for (const ObjectType& t : objectTypes) n += t.lodMeshes > 0 ? 1 : 0; return n; }
		size_t LodObjectMeshes() const { size_t n = 0; for (const ObjectType& t : objectTypes) n += t.lodMeshes; return n; }

		// ── TLAS 목록 (2026-11-17, 사용자 «TLAS 따로 고르는 단계 만들어» · «아예 두 묶음으로 다르게») ──
		//  래스터(StreamObjects · 장면 노드)와 따로, TLAS 에 넣을 배치 오브젝트를 고른다. 장면 노드 없이 메시마다 월드 행렬을
		//  계산해 둔다(TlasObjects). 규칙(11-16 · 11-17 사용자 결정):
		//   앞쪽 반구(행성 중심 기준 내적 ≥ 0) · 60 m 안 전부 + 조준 기준 화면 지름 32 px 이상 →
		//   단계(60 m 안 0 · 120 m 안 1 · 밖 2, 32 px 이상 −1 · 128 px 이상 −2) 낮은 것부터, 같은 단계는 px 큰 것부터.
		//   자리가 남으면 32 px 밑을 px 큰 순으로 마저 채운다. budget = 메시 수 상한(TLAS 4,096 − 다른 몫, Engine 이 넘긴다).
		//  공간 격자(128 m 칸 × 크기 묶음)로 훑는다(11-17 결정). 처음 · force · kRestreamDistance 이상 움직였을 때 ·
		//  budget 이 줄어 넘치거나 64 이상 늘었을 때만 다시 고른다. 다시 골랐으면 true.
		bool SelectTlasObjects(const Vec3d& eye, size_t budget, bool force);
		const std::vector<InstanceData>& TlasObjects() const { return tlasObjects; }
		// 이 행성 배치 오브젝트의 GPU 메시인가 — 래스터 목록(패킷)에서 오브젝트를 빼고 TLAS 기본 목록을 만들 때 쓴다.
		bool IsObjectMesh(MeshHandle mesh) const { return mesh < objectMesh.size() && objectMesh[mesh] != 0; }
		// 마지막 고르기 — 훑은 배치 수 · 후보(60 m 안 + 32 px 이상) 메시 수 · 걸린 시간(ms). 자동 측정 · 창 제목용.
		size_t LastTlasExamined() const { return lastTlasExamined; }
		size_t LastTlasCandidateMeshes() const { return lastTlasCandidateMeshes; }
		double LastTlasSelectMs() const { return lastTlasSelectMs; }

		// ── 지형 레이어 (2026-10-28) ──
		// 읽을 DDS 의 exe 옆 assets\ 기준 경로 17개 — 색 4 · 노멀 4 · ORM 4 · 높이 4(각각 마스크 R G B A 순) + 마스크.
		//  JSON 에 terrain_layers 가 없으면 빈 목록.
		static std::vector<std::wstring> TerrainLayerAssets(size_t index, const PlanetPlacement& placement);
		// 로딩 스레드에서 부른다(멤버를 건드리지 않는다). paths = 위 목록을 실제 경로로 바꾼 것. 실패하면 out.error.
		static bool LoadTerrainLayers(const std::vector<std::wstring>& paths, TerrainLayerSet& out);
		// 메인 스레드. Build 뒤에 부른다 — 텍스처 5개를 올려 지형 재질을 만들고 지형 그리기 노드의 재질을 바꾼다.
		bool ApplyTerrainLayers(size_t index, TerrainLayerSet&& layers, GRenderer& renderer, LegacyScene& scene,
			std::wstring& error);

		// 창 제목용 — «지형 N 삼각형 · 지형 레이어 섞기 / 대체 1장 · 오브젝트 보임 k / 배치 M · 종류 a/b · 메시 n/예산 · 반대편 제외 f»
		std::wstring Status() const;

		const Shared::PlanetSurface& Ground() const { return ground; }

		// 배치 JSON 의 오브젝트 — 인스턴스 하나에 StaticObject 하나(2026-11-02). 순서 = JSON instances 순서(건너뛴 것 제외).
		const std::vector<Shared::StaticObject>& StaticObjects() const { return staticObjects; }

		// ── 충돌체 (2026-10-29) — 로딩 스레드가 만든 것을 메인이 넘겨받는다. 못 읽었으면 비어 있다(Loaded() false) ──
		void SetCollision(Shared::CollisionWorld&& world) { collision = std::move(world); }
		const Shared::CollisionWorld& Collision() const { return collision; }
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
			uint32_t                meshCount = 0;     // 인스턴스 하나가 차지하는 메시 노드 수(2026-11-20 — LOD0 노드만)
			uint32_t                lodMeshes = 0;     // 2026-11-20 메시 LOD — 렌더러에 단계를 이은 LOD0 메시 수(0 = LOD 없음)
			bool                    loaded = false;
			std::vector<NodeHandle> freeSlots;         // 숨겨 둔 인스턴스 루트(다시 쓴다)
		};

		Model                 model;
		Shared::PlanetSurface ground;
		Shared::CollisionWorld collision;              // 배치 오브젝트 충돌체(2026-10-29)
		NodeHandle            root = kInvalidNode;
		Vec3d                 spawnUp{ 0.0, 1.0, 0.0 };
		bool                  loaded = false;
		std::vector<NodeHandle> terrainNodes;          // 지형 메시(…_Planet_Terrain)의 그리기 노드 — 지형 레이어 재질로 바꾼다
		bool                  terrainLayered = false;  // 지형 레이어 재질을 끼웠는가(창 제목)
		std::vector<TextureHandle> terrainTextures;    // 2026-11-17 — 지형 레이어 텍스처(ApplyTerrainLayers 가 올린 것, Release 가 놓는다)
		MaterialHandle        terrainMaterial = kInvalidMaterial;   // 2026-11-17 — 지형 레이어 재질(Release 가 놓는다)
		// ── 땅 위 메시 경계 섞기 (2026-10-29) ──
		//  오솔길 · 착륙 대지 중 «지형과 같은 땅» 재질인 것도 지형 레이어 재질로 그린다(ApplyTerrainLayers).
		Vec3d                 center{ 0.0, 0.0, 0.0 };              // 월드의 행성 중심 — 지형 레이어 좌표의 원점
		std::string           terrainMaterialName;                  // glb 지형 노드의 재질 이름(대체 레이어)
		std::array<std::string, 4> terrainLayerNames;               // 배치 JSON terrain_layers 의 R G B A
		std::vector<std::pair<NodeHandle, std::string>> overlayNodes;   // (그리기 노드, glb 재질 이름) — 오솔길 · 착륙 대지
		size_t                layeredOverlays = 0;                  // 그중 지형 레이어 재질로 바꾼 수(창 제목)
		size_t                fadedLakes = 0;                       // 물가 페이드를 켠 얼음 호수 노드 수(창 제목)

		std::vector<ObjectType>                 objectTypes;
		// 배치 오브젝트 (2026-11-02) — meshIndex = objectTypes 번호, renderObjectId = 보이는 루트(없으면 kInvalidNode)
		std::vector<Shared::StaticObject>       staticObjects;
		size_t                                  skippedInstances = 0;
		std::vector<uint32_t>                   liveList;      // 지금 보이는 오브젝트 번호
		std::vector<uint32_t>                   selectStamp;   // 이번에 골랐는가(stamp 와 같으면)
		uint32_t                                stamp = 0;
		std::vector<std::pair<float, uint32_t>> scratch;       // (점수, 인스턴스) — 고를 때마다 다시 쓴다
		Vec3d                                   lastStreamEye;
		bool                                    streamed = false;
		size_t                                  selectedMeshes = 0;        // 2026-11-16 마지막으로 고른 메시 노드 수(≤ 예산, 창 제목)
		size_t                                  lastStreamBudget = 0;      // 2026-11-17 마지막 고르기의 메시 예산(StreamObjects budget)
		size_t                                  farHemisphereSkipped = 0;  // 2026-11-16 마지막 고르기에서 반대편 반구라 뺀 오브젝트 수(창 제목)

		// ── TLAS 목록 (2026-11-17) ──
		struct GridBucket                                      // 공간 격자 한 칸의 크기 묶음
		{
			Vec3d    center{};                                 // 칸 가운데(행성 중심 기준)
			float    maxDiameter = 0.0f;                       // 묶음 안 가장 큰 지름(m) — 화면 지름 상한
			uint32_t first = 0;                                // gridItems 안 시작
			uint32_t count = 0;
		};
		void BuildObjectGrid();
		// 2026-11-17 — 순위(TLAS · 래스터 공용): tlasPrimary · tlasFill 을 채운다. 눈 1 m 안 · 예산 이하면 앞 순위를 다시 쓴다.
		void RankObjects(const Vec3d& eye, size_t budget);
		Vec3d                                   rankEye;
		size_t                                  rankBudget = 0;
		bool                                    rankValid = false;
		size_t                                  objectModelsFinished = 0;   // ObjectModelFinished 센 수(마지막 종류 뒤 격자)
		// 래스터 칸 리스트 (2026-11-17)
		std::vector<InstanceList>               rasterLists;
		bool                                    rasterBuilt = false;
		int64_t                                 rasterCell[3] = { 0, 0, 0 };   // 마지막으로 만든 때의 플레이어 칸
		uint64_t                                rasterVersion = 0;
		size_t                                  rasterSmallMeshes = 0;
		size_t                                  rasterBigMeshes = 0;
		size_t                                  rasterTempMeshes = 0;
		size_t                                  rasterCutBigMeshes = 0;    // 큰 리스트에서 잘린 것(플레이어 칸 밖 — 그리지 않음)
		double                                  lastRasterBuildMs = 0.0;
		std::vector<GridBucket>                 gridBuckets;
		std::vector<uint32_t>                   gridItems;     // 묶음별로 모은 배치 번호
		bool                                    gridBuilt = false;
		std::vector<uint8_t>                    objectMesh;    // MeshHandle → 배치 오브젝트 메시면 1
		std::vector<InstanceData>               tlasObjects;   // 고른 오브젝트의 메시마다 한 줄(월드 행렬)
		std::vector<std::pair<float, uint32_t>> tlasPrimary;   // (정렬 키 = 단계 × 1e6 − px, 배치) — 후보
		std::vector<std::pair<float, uint32_t>> tlasFill;      // (px, 배치) — 32 px 밑 채움 후보
		std::vector<uint8_t>                    bucketExamined;
		Vec3d                                   lastTlasEye;
		size_t                                  lastTlasBudget = 0;
		bool                                    tlasSelected = false;
		size_t                                  lastTlasExamined = 0;
		size_t                                  lastTlasCandidateMeshes = 0;
		double                                  lastTlasSelectMs = 0.0;
	};

} // namespace swc
