#include "PlanetDebugScene.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <DirectXMath.h>
#include "Client/GRenderer.h"
#include "Client/LegacyScene.h"
#include "Client/Log.h"
#include "Client/Resource/TextureLoader.h"
#include "Shared/Model/Readers/JsonValue.h"
#include "Shared/World/WorldCatalog.h"

using namespace DirectX;

namespace {

	// 맵 glb · 배치 JSON · 충돌체 경로는 Shared/World/WorldCatalog.h 의 kWorlds(맵 번호 1 ~ 5)에 있다(2026-11-01 — 서버도 같은 파일을 읽는다).
	struct PlanetEntry
	{
		const wchar_t* name;
		// 지형 레이어 (2026-10-28) — 블렌더 시안 재질 값(Resource_Work/<행성>/scripts/build_map.py draft_terrain_material).
		//  heightBlend = 레이어(마스크 R G B A)마다 Height 에 곱하는 값, normalStrength = Normal Map 노드 Strength.
		//  경계 폭(0.2)은 다섯 행성 모두 같아 TerrainMaterialData 기본값을 쓴다.
		std::array<float, 4> heightBlend;
		float normalStrength;
		// 3단계 (2026-10-28) — 시안의 세 방향 투영: 레이어 비트(R 1 · G 2 · B 4 · A 8), 크기 A · B(m), B 좌표 오프셋,
		//  좌표 비틀기(규모 · 최대 거리, m). 숲 · 네온 · 폐허 · 화산은 바위 레이어(G) 9 m + 2.7 m · 비틀기 없음,
		//  빙하는 화강암(G) · 빙하 얼음(B) 8 m + 2.96 m(8 × 0.37) · B 오프셋 (3.7, 1.9, 5.3) · 70 m 규모 ±10 m.
		uint32_t triplanarLayers;
		float triplanarTileA;
		float triplanarTileB;
		bool  triplanarOffsetB;
		float warpScale;
		float warpAmount;
		// 4단계 (2026-10-28) — 시안의 큰 규모 색 흔들림(노이즈 × amount + bias) · 범프 1 Distance · 범프 2 Distance · Strength.
		//  숲 · 네온 · 폐허 0.32 / 0.84 · 0.06 · 0.45 · 0.8, 화산 범프 0.09 · 0.55 · 0.9, 빙하 0.22 / 0.89 · 0.07 · 0.45 · 0.7.
		float macroAmount;
		float macroBias;
		float bumpDistance;
		float reliefDistance;
		float reliefStrength;
		// 땅 위 메시 경계 섞기 (2026-10-29) — 이름이 레이어와 달라도 «지형과 같은 땅» 으로 볼 오솔길 재질(없으면 nullptr).
		//  숲: Verdant_Trail = Dirt_Trail 레이어 텍스처(Verdant build_map.py 250줄), 마스크 A 가 길 자리에 흙길을 부드럽게
		//  칠해 둔다(mask_gen.py 60 ~ 72줄: 가운데 0.9 m 까지 흙 → 2.3 m 에서 풀, 노이즈로 흔들림).
		const char* trailMaterial;
	};

	// resource 브랜치 예시 행성 (fa6777c · 8bafbc6 · 6afb98d · c51ed07 · 5dfe6bd)
	constexpr std::array<PlanetEntry, swc::PlanetDebugScene::kCount> kPlanets{ {
		{ L"Verdant",  { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f, "Verdant_Trail" },
		{ L"Neon",     { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f, nullptr },
		{ L"Ruin",     { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f, nullptr },
		{ L"Volcanic", { 0.5f, 0.5f, 0.5f, 0.5f }, 1.6f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.09f, 0.55f, 0.9f, nullptr },
		// 빙하: A = Snow 만 0.35 (시안 «0.35 if name == 'Snow' else 0.5»)
		{ L"Glacier",  { 0.5f, 0.5f, 0.5f, 0.35f }, 1.4f,
		               2u | 4u, 8.0f, 8.0f * 0.37f, true, 70.0f, 10.0f,
		               0.22f, 0.89f, 0.07f, 0.45f, 0.7f, nullptr },
	} };

	// 보이는 지형 메시의 노드 — «<행성>_Planet_Terrain». 같은 메시를 쓰는 접지용 «COL_Expedition_Planet_Terrain» 은 뺀다
	// (다섯 행성 glb 확인, 2026-10-28).
	bool IsTerrainNode(std::string_view name)
	{
		constexpr std::string_view suffix = "_Planet_Terrain";
		return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix &&
			name.substr(0, 4) != "COL_";
	}

	// 땅 위 메시 (2026-10-29) — 오솔길 · 착륙 대지. 다섯 행성 glb 확인: Walkable_Path_* · Landing_L1~4_Platform(노드 변환 있음).
	//  착륙 표시(Landing_L*_Marking, 네온 · 폐허)는 표시 재질이라 넣지 않는다.
	bool IsGroundOverlayNode(std::string_view name)
	{
		constexpr std::string_view path = "Walkable_Path_";
		constexpr std::string_view padPrefix = "Landing_L";
		constexpr std::string_view padSuffix = "_Platform";
		if (name.substr(0, path.size()) == path) return true;
		return name.size() > padPrefix.size() + padSuffix.size() && name.substr(0, padPrefix.size()) == padPrefix &&
			name.substr(name.size() - padSuffix.size()) == padSuffix;
	}

	// 얼어붙은 호수 면(빙하 Frozen_Lake 15개, 재질 Lake_Ice — 투과 0.45 · 굴절률 1.31 · 두께 0)
	bool IsFrozenLakeNode(std::string_view name)
	{
		constexpr std::string_view lake = "Frozen_Lake";
		return name.substr(0, lake.size()) == lake;
	}

	// 얼음 호수 물가 페이드 폭(m) — 물가 선에서 얼음 0 → 이만큼 안쪽에서 얼음 그대로(Transparent.hlsl PSIce ⑥).
	//  호수 15번 확인: 얼음 면 아래 지형이 가운데 ~55 m 까지 6 cm 로 평평, 물가에서 면 위로 올라온다.
	constexpr float kLakeEdgeFade = 2.0f;

	// 스폰 = 착륙 지점 L1 의 플랫폼 노드. 예시 행성은 L1 방향이 게임 맵(kPlanetSpawnUp)과 달라
	// 맵 노드에서 읽는다(resource fa6777c 커밋의 «주의»). 다섯 행성 모두 최상위 노드다.
	//  ★ 2026-11-01 — 서버도 같은 노드로 스폰 방향을 잡는다(Shared/World/WorldMap.cpp LoadWorldGround). 이름은 Shared 한 곳.
	constexpr std::string_view kSpawnNode = Shared::kWorldSpawnNode;

	// 디버그 씬 번호(0 ~ 4)의 맵 정보 — Shared/World/WorldCatalog.h 맵 번호 1 ~ 5. 범위 밖이면 nullptr.
	const Shared::WorldInfo* WorldOf(size_t index)
	{
		const uint32_t world = Shared::WorldOfDebugPlanet(index);
		return world < Shared::kWorldCount ? &Shared::kWorlds[world] : nullptr;
	}

	// JSON 문자열(UTF-8) → 와이드. 경로 · 오류 메시지용. 잘못된 UTF-8 이면 바이트를 그대로 넓힌다.
	std::wstring FromUtf8(const std::string& text)
	{
		try
		{
			return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
		}
		catch (...)
		{
			return std::wstring(text.begin(), text.end());
		}
	}

	// JSON 회전(쿼터니언 x y z w) → 명세 6.3 Rotation. 소수 셋째 자리로 반올림돼 있어 정규화한다. 길이가 0 이면 회전 없음.
	Shared::Vec4 PlacementRotation(const float q[4])
	{
		XMVECTOR rotation = XMVectorSet(q[0], q[1], q[2], q[3]);
		rotation = XMVectorGetX(XMVector4LengthSq(rotation)) > 1.0e-12f
			? XMQuaternionNormalize(rotation) : XMQuaternionIdentity();
		XMFLOAT4 out;
		XMStoreFloat4(&out, rotation);
		return Shared::Vec4{ out.x, out.y, out.z, out.w };
	}

	XMVECTOR LoadRotation(const Shared::Vec4& q)
	{
		return XMVectorSet(q.x, q.y, q.z, q.w);
	}

	// 배치 오브젝트의 변환 — 파서의 노드 변환과 같은 S · R · T (행 벡터, GltfReader.cpp NodeLocal).
	//  StaticObject 의 Scale · Rotation · Position (명세 6.3).
	XMMATRIX InstanceMatrix(const Shared::StaticObject& object)
	{
		return XMMatrixScaling(object.scale.x, object.scale.y, object.scale.z) *
			XMMatrixRotationQuaternion(LoadRotation(object.rotation)) *
			XMMatrixTranslation(object.position.x, object.position.y, object.position.z);
	}

	// 명세 6.3 Direction — 오브젝트가 바라보는 쪽. glTF 앞쪽(+Z)을 Rotation 으로 돌린 단위 벡터.
	Shared::Vec3 PlacementDirection(const Shared::Vec4& rotation)
	{
		XMFLOAT3 forward;
		XMStoreFloat3(&forward, XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), LoadRotation(rotation)));
		return Shared::Vec3{ forward.x, forward.y, forward.z };
	}

}

namespace swc {

	const wchar_t* PlanetDebugScene::Name(size_t index)
	{
		return index < kPlanets.size() ? kPlanets[index].name : L"";
	}

	// 경로 네 가지는 Shared::kWorlds 를 읽는다(2026-11-01 — 서버와 한 곳).
	const wchar_t* PlanetDebugScene::Asset(size_t index)
	{
		const Shared::WorldInfo* world = WorldOf(index);
		return world ? world->map : L"";
	}

	const wchar_t* PlanetDebugScene::PlacementAsset(size_t index)
	{
		const Shared::WorldInfo* world = WorldOf(index);
		return world && world->placement ? world->placement : L"";
	}

	// 충돌체 묶음 (2026-10-29) — «model\map\X\X_Collision.glb» · «…\X_Collision.json»
	std::wstring PlanetDebugScene::CollisionAsset(size_t index)
	{
		const Shared::WorldInfo* world = WorldOf(index);
		return world ? std::wstring(world->collision) : std::wstring();
	}

	std::wstring PlanetDebugScene::CollisionManifestAsset(size_t index)
	{
		const Shared::WorldInfo* world = WorldOf(index);
		return world ? std::wstring(world->collisionManifest) : std::wstring();
	}

	bool PlanetDebugScene::Build(const ModelData& data, Shared::PlanetSurface&& parsedGround, GRenderer& renderer,
		LegacyScene& scene, const Vec3d& planetCenter, std::wstring& error)
	{
		if (loaded)
		{
			error = L"이미 올린 행성입니다.";
			return false;
		}
		if (!parsedGround.Valid())
		{
			error = L"행성 지표면(COL_Expedition_Planet_Terrain)을 만들지 못했습니다.";
			return false;
		}

		// 예시 행성은 게임 맵과 같은 기준 반경(650 m)으로 만들어 배율 1 로 올린다.
		XMFLOAT4X4 visual;
		XMStoreFloat4x4(&visual, XMMatrixIdentity());
		if (!model.Initialize(data, renderer, visual))
		{
			error = L"맵 모델 초기화 실패: " + model.LastError();
			return false;
		}

		// 지형 메시의 그리기 노드를 기억한다 — 지형 레이어 재질로 바꿔 끼울 자리(ApplyTerrainLayers, 2026-10-28).
		//  2026-10-29 — 오솔길 · 착륙 대지(재질이 지형과 같은 땅이면 같이 바꾼다)와 얼어붙은 호수(물가 페이드)도 기억한다.
		auto materialName = [&data](uint32_t mesh) -> std::string
		{
			if (mesh >= data.meshes.size()) return std::string();
			const uint32_t material = data.meshes[mesh].material;
			return material < data.materials.size() ? data.materials[material].name : std::string();
		};
		std::vector<uint32_t> terrainMeshes;
		std::vector<uint32_t> overlayMeshes;
		std::vector<uint32_t> lakeMeshes;
		terrainMaterialName.clear();
		for (const ModelNodeData& node : data.nodes)
		{
			if (IsTerrainNode(node.name))
			{
				terrainMeshes.insert(terrainMeshes.end(), node.meshes.begin(), node.meshes.end());
				if (terrainMaterialName.empty() && !node.meshes.empty())
					terrainMaterialName = materialName(node.meshes.front());
			}
			else if (IsGroundOverlayNode(node.name))
				overlayMeshes.insert(overlayMeshes.end(), node.meshes.begin(), node.meshes.end());
			else if (IsFrozenLakeNode(node.name))
				lakeMeshes.insert(lakeMeshes.end(), node.meshes.begin(), node.meshes.end());
		}
		std::vector<std::pair<uint32_t, NodeHandle>> meshNodes;
		root = model.Instantiate(scene, kInvalidNode, &meshNodes);
		if (root == kInvalidNode)
		{
			error = L"행성 모델을 장면에 등록하지 못했습니다.";
			return false;
		}
		auto contains = [](const std::vector<uint32_t>& meshes, uint32_t mesh)
		{
			return std::find(meshes.begin(), meshes.end(), mesh) != meshes.end();
		};
		terrainNodes.clear();
		overlayNodes.clear();
		for (const auto& [mesh, node] : meshNodes)
		{
			if (contains(terrainMeshes, mesh))
				terrainNodes.push_back(node);
			else if (contains(overlayMeshes, mesh))
				overlayNodes.emplace_back(node, materialName(mesh));
		}

		// 얼어붙은 호수 — 물가 페이드를 켠다(2026-10-29). 호수들이 재질 하나(Lake_Ice)를 같이 쓰므로 재질마다 한 번.
		fadedLakes = 0;
		std::vector<MaterialHandle> fadedMaterials;
		for (uint32_t mesh : lakeMeshes)
		{
			const MaterialHandle material = model.MeshMaterial(mesh);
			if (material == kInvalidMaterial) continue;
			if (std::find(fadedMaterials.begin(), fadedMaterials.end(), material) == fadedMaterials.end())
			{
				if (!renderer.SetMaterialEdgeFade(material, kLakeEdgeFade))
				{
					Log(L"[디버그 행성] 얼음 호수 물가 페이드 실패 — %s", renderer.StatusText().c_str());
					continue;
				}
				fadedMaterials.push_back(material);
			}
			++fadedLakes;
		}
		center = planetCenter;
		scene.SetLocalTransform(root,
			XMMatrixTranslation(float(planetCenter.x), float(planetCenter.y), float(planetCenter.z)));
		scene.SetVisible(root, false);   // 들어갈 때 Engine 이 켠다

		// 스폰 방향 — L1 플랫폼 노드의 행성 중심 기준 위치. 부모가 있으면 부모 쪽으로 거슬러 곱한다
		// (노드는 부모가 먼저 온다 — Model::Initialize 가 검사한다). 못 찾으면 게임 맵의 L1 방향을 쓴다.
		spawnUp = Normalize({ Shared::kPlanetSpawnUpX, Shared::kPlanetSpawnUpY, Shared::kPlanetSpawnUpZ });
		for (const ModelNodeData& node : data.nodes)
		{
			if (node.name != kSpawnNode) continue;
			XMMATRIX world = XMLoadFloat4x4(&node.local);
			for (uint32_t p = node.parent; p != kInvalidModelIndex && p < data.nodes.size(); p = data.nodes[p].parent)
				world = world * XMLoadFloat4x4(&data.nodes[p].local);
			XMFLOAT3 offset;
			XMStoreFloat3(&offset, world.r[3]);
			const Vec3d fromCenter{ offset.x, offset.y, offset.z };
			if (Length(fromCenter) > 1.0)
				spawnUp = Normalize(fromCenter);
			break;
		}

		ground = std::move(parsedGround);
		loaded = true;
		return true;
	}

	// ── 행성 내리기 (2026-11-17, 사용자 «이전에 있던 행성만 남기고, 더 오래된 행성은 해제») ──
	//  GPU: 맵 모델 · 오브젝트 종류 모델 · 지형 레이어 텍스처 5장 · 재질을 놓는다(GRenderer::Release — GPU 를 기다린 뒤).
	//  장면: 루트 아래 노드를 비운다(LegacyScene::ClearSubtree — 칸은 빈 노드로 남는다).
	//  CPU: 접지면 · 충돌체 · 배치(StaticObject) · 고르기 상태를 비우고 Loaded() 를 false 로 — 다시 고르면 처음처럼 읽는다.
	//  GPU 놓기가 하나라도 실패해도 장면 · CPU 는 비운다(다시 읽을 수 있게). 그때는 false — 못 놓은 GPU 자원은 남는다.
	bool PlanetDebugScene::Release(GRenderer& renderer, LegacyScene& scene)
	{
		bool released = true;
		if (!model.Release(renderer))
			released = false;
		for (ObjectType& type : objectTypes)
			if (!type.model.Release(renderer))
				released = false;
		std::vector<MaterialHandle> layerMaterials;
		if (terrainMaterial != kInvalidMaterial)
			layerMaterials.push_back(terrainMaterial);
		if (!renderer.Release({}, terrainTextures, layerMaterials))
			released = false;

		if (root != kInvalidNode)
			scene.ClearSubtree(root);

		// 빈 벡터와 맞바꿔 용량까지 돌려준다(clear 는 용량을 남긴다 — 배치 · 접지면은 행성당 수십 ~ 100 MiB).
		auto freeVector = [](auto& values) { std::remove_reference_t<decltype(values)>().swap(values); };
		ground = Shared::PlanetSurface{};
		collision = Shared::CollisionWorld{};
		root = kInvalidNode;
		spawnUp = Vec3d{ 0.0, 1.0, 0.0 };
		loaded = false;
		freeVector(terrainNodes);
		terrainLayered = false;
		freeVector(terrainTextures);
		terrainMaterial = kInvalidMaterial;
		center = Vec3d{ 0.0, 0.0, 0.0 };
		terrainMaterialName.clear();
		terrainLayerNames = {};
		freeVector(overlayNodes);
		layeredOverlays = 0;
		fadedLakes = 0;
		freeVector(objectTypes);
		freeVector(staticObjects);
		skippedInstances = 0;
		freeVector(liveList);
		freeVector(selectStamp);
		stamp = 0;
		freeVector(scratch);
		lastStreamEye = Vec3d{};
		streamed = false;
		selectedMeshes = 0;
		lastStreamBudget = 0;
		farHemisphereSkipped = 0;
		freeVector(gridBuckets);   // 2026-11-17 TLAS 목록
		freeVector(gridItems);
		gridBuilt = false;
		freeVector(objectMesh);
		freeVector(tlasObjects);
		freeVector(tlasPrimary);
		freeVector(tlasFill);
		freeVector(bucketExamined);
		lastTlasEye = Vec3d{};
		lastTlasBudget = 0;
		tlasSelected = false;
		rankValid = false;
		rankBudget = 0;
		objectModelsFinished = 0;
		freeVector(rasterLists);   // 2026-11-17 래스터 칸 리스트
		rasterBuilt = false;
		return released;
	}

	// ── 배치 JSON 읽기 (로딩 스레드) ─────────────────────────────
	//  2026-11-01 — 본문을 Shared/World/Placement.cpp 로 옮겼다(서버도 같은 배치로 충돌체를 씌운다 — 맵 번호).
	bool PlanetDebugScene::ReadPlacement(const std::wstring& path, PlanetPlacement& out)
	{
		return Shared::ReadPlacement(path, out);
	}

	// ── 배치 오브젝트 (메인) ──────────────────────────────────────
	void PlanetDebugScene::SetPlacement(PlanetPlacement&& placement)
	{
		objectTypes.clear();
		objectTypes.resize(placement.types.size());
		for (size_t t = 0; t < placement.types.size(); ++t)
		{
			objectTypes[t].name = std::move(placement.types[t].name);
			objectTypes[t].file = std::move(placement.types[t].file);
		}
		skippedInstances = placement.skipped;
		terrainLayerNames = placement.terrainLayers;   // 2026-10-29 — 오솔길 · 착륙 대지 재질이 «지형과 같은 땅» 인지 볼 때

		// 인스턴스마다 StaticObject 하나 (2026-11-02, 명세 6.5 · 6.10) — 필드 규칙은 PlanetDebugScene.h 머리 주석.
		staticObjects.clear();
		staticObjects.reserve(placement.instances.size());
		for (const PlanetPlacement::Instance& instance : placement.instances)
		{
			Shared::StaticObject& object = staticObjects.emplace_back();
			object.position = Shared::Vec3{ instance.position[0], instance.position[1], instance.position[2] };
			object.rotation = PlacementRotation(instance.rotation);
			object.direction = PlacementDirection(object.rotation);
			object.scale = Shared::Vec3{ instance.scale, instance.scale, instance.scale };
			object.meshIndex = instance.type;          // → objectTypes[meshIndex].model (그 종류의 GPU 메시 묶음)
			object.renderObjectId = kInvalidNode;      // 장면에 올리면 루트 노드(StreamObjects)
		}
		placement.instances.clear();
		placement.instances.shrink_to_fit();

		selectStamp.assign(staticObjects.size(), 0);
		liveList.clear();
		stamp = 0;
		streamed = false;
		gridBuilt = false;        // 2026-11-17 TLAS 목록 — 배치가 바뀌면 격자 · 고른 목록을 새로
		tlasSelected = false;
		tlasObjects.clear();
		rankValid = false;
		objectModelsFinished = 0;
		rasterLists.clear();      // 2026-11-17 래스터 칸 리스트
		rasterBuilt = false;
	}

	// ── 식생 LOD 시험 (2026-11-20 render/tlas-select, 사용자 «테스트 브랜치 이동해서 LOD 적용시키고 Gbuffer 측정해줘») ──
	//  켜면 아래 종류는 같은 폴더의 «이름_LOD.glb»(Resource_Work/LOD_Vegetation/out 결과 — LOD0 = 원본 + 노드 _LOD1 ~ _LOD3)를 읽는다.
	//  LOD0 메시는 원본 glb 와 바이트까지 같다(Resource_Work/LOD_Vegetation/렌더_20261120/목록.md). 끄면 예전 glb.
	constexpr bool kVegetationLodAssets = true;
	constexpr const char* kLodAssetNames[] = {
		"Tree_Oak_A", "Tree_Oak_B", "Tree_Beech_A", "Tree_Beech_B", "Tree_Pine_A", "Tree_Pine_B",
		"Grass_Patch_A", "Grass_Patch_B", "Weed_Patch_A" };

	std::wstring PlanetDebugScene::ObjectAsset(size_t type) const
	{
		if (type >= objectTypes.size()) return std::wstring();
		std::string file = objectTypes[type].file;
		if (kVegetationLodAssets)
		{
			for (const char* name : kLodAssetNames)
			{
				const std::string suffix = std::string("/") + name + ".glb";
				if (file.size() > suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0)
				{
					file = file.substr(0, file.size() - 4) + "_LOD.glb";
					break;
				}
			}
		}
		std::wstring relative = FromUtf8(file);
		std::replace(relative.begin(), relative.end(), L'/', L'\\');
		return L"model\\" + relative;
	}

	bool PlanetDebugScene::AddObjectModel(size_t type, const ModelData& data, GRenderer& renderer, std::wstring& error)
	{
		if (type >= objectTypes.size())
		{
			error = L"배치 JSON 에 없는 오브젝트 종류입니다.";
			return false;
		}
		ObjectType& target = objectTypes[type];
		if (target.loaded) return true;

		// 오브젝트 원점(바닥 가운데)을 그대로 쓴다 — 캐릭터처럼 높이 맞춤을 하지 않는다.
		XMFLOAT4X4 visual;
		XMStoreFloat4x4(&visual, XMMatrixIdentity());
		if (!target.model.Initialize(data, renderer, visual))
		{
			error = target.model.LastError();
			return false;
		}

		const float dx = data.boundsMax.x - data.boundsMin.x;
		const float dy = data.boundsMax.y - data.boundsMin.y;
		const float dz = data.boundsMax.z - data.boundsMin.z;
		target.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

		// 2026-11-20 메시 LOD — 인스턴스 하나가 래스터 목록에서 차지하는 칸 = LOD0 노드 메시 수(단계 노드는 목록에 안 든다)
		target.meshCount = target.model.BaseMeshCount();
		target.loaded = target.meshCount > 0;
		{
			const XMFLOAT3 center((data.boundsMin.x + data.boundsMax.x) * 0.5f, (data.boundsMin.y + data.boundsMax.y) * 0.5f,
				(data.boundsMin.z + data.boundsMax.z) * 0.5f);
			const XMFLOAT3 extents(dx * 0.5f, dy * 0.5f, dz * 0.5f);
			target.lodMeshes = target.model.RegisterLods(renderer, center, extents);   // 단계 노드가 없으면 0
		}
		// 2026-11-17 TLAS 목록 분리 — 이 종류의 GPU 메시를 표시해 둔다(래스터 목록에서 오브젝트를 가려낼 때)
		for (const MeshHandle mesh : target.model.Meshes())
		{
			if (mesh == kInvalidMesh) continue;
			if (mesh >= objectMesh.size()) objectMesh.resize(size_t(mesh) + 1, 0);
			objectMesh[mesh] = 1;
		}
		gridBuilt = false;
		if (!target.loaded)
			error = L"그릴 메시가 없습니다.";
		return target.loaded;
	}

	// ── 오브젝트 고르기 (메인 · 매 프레임) ───────────────────────────
	bool PlanetDebugScene::StreamObjects(LegacyScene& scene, const Vec3d& eye, bool force, size_t budget)
	{
		if (!loaded || root == kInvalidNode || staticObjects.empty()) return false;
		// 2026-11-17 — 예산이 줄어 고른 것이 넘치거나 64 이상 늘었으면 움직이지 않아도 다시 고른다(SelectTlasObjects 와 같은 규칙)
		const bool budgetChanged = streamed && (budget < selectedMeshes || budget >= lastStreamBudget + 64);
		if (!force && streamed && !budgetChanged && Length(eye - lastStreamEye) < kRestreamDistance) return false;
		lastStreamEye = eye;
		lastStreamBudget = budget;
		streamed = true;

		// ①② 2026-11-17 render/tlas-select — TLAS 와 같은 순위(RankObjects: 앞쪽 반구 · 60 m 안 전부 + 조준 32 px →
		//  단계 → px, 남는 자리는 32 px 밑 px 순)로 메시 예산까지(사용자 «우선 TLAS 하되, 컬링은 그대로 유지해줘»).
		//  전에는 점수(반지름 × 배율 ÷ 거리) 순이라 먼 큰 나무 · 건물이 가까운 작은 풀보다 먼저 자리를 차지했다.
		//  반대편 반구 제외(2026-11-16 사용자 지시)는 순위 안에 그대로 있다.
		RankObjects(eye, budget);
		if (++stamp == 0)   // 한 바퀴 돌면 표시를 지우고 1 부터
		{
			std::fill(selectStamp.begin(), selectStamp.end(), 0u);
			stamp = 1;
		}
		scratch.clear();    // 고른 배치(second) — ④ 에서 놓는다
		size_t meshes = 0;
		auto pick = [&](const std::vector<std::pair<float, uint32_t>>& list)
		{
			for (const auto& entry : list)
			{
				const size_t cost = objectTypes[staticObjects[entry.second].meshIndex].meshCount;
				if (meshes + cost > budget) return false;
				meshes += cost;
				selectStamp[entry.second] = stamp;
				scratch.emplace_back(0.0f, entry.second);
			}
			return true;
		};
		if (pick(tlasPrimary)) pick(tlasFill);
		const size_t selected = scratch.size();
		selectedMeshes = meshes;

		// ③ 이번에 빠진 것을 숨기고 종류별 빈 자리로 돌린다.
		size_t kept = 0;
		for (const uint32_t i : liveList)
		{
			if (selectStamp[i] == stamp)
			{
				liveList[kept++] = i;
				continue;
			}
			Shared::StaticObject& object = staticObjects[i];
			scene.SetVisible(object.renderObjectId, false);
			objectTypes[object.meshIndex].freeSlots.push_back(object.renderObjectId);
			object.renderObjectId = kInvalidNode;
		}
		liveList.resize(kept);

		// ④ 새로 든 것을 놓는다 — 빈 자리가 있으면 다시 쓰고, 없으면 모델을 펼쳐 새로 만든다.
		for (size_t k = 0; k < selected; ++k)
		{
			const uint32_t i = scratch[k].second;
			Shared::StaticObject& object = staticObjects[i];
			if (object.renderObjectId != kInvalidNode) continue;

			ObjectType& type = objectTypes[object.meshIndex];
			NodeHandle slot = kInvalidNode;
			if (!type.freeSlots.empty())
			{
				slot = type.freeSlots.back();
				type.freeSlots.pop_back();
				scene.SetVisible(slot, true);
			}
			else
			{
				slot = type.model.Instantiate(scene, root);   // 맵 루트 아래 — 행성을 숨기면 같이 숨는다
				if (slot == kInvalidNode) continue;
			}
			scene.SetLocalTransform(slot, InstanceMatrix(object));
			object.renderObjectId = slot;   // StaticObject → 렌더 데이터(명세 6.11 RenderObjectID)
			liveList.push_back(i);
		}
		return true;
	}

	// ── TLAS 목록 (2026-11-17) ─────────────────────────────────────
	//  공간 격자 = 행성 중심 기준 128 m 칸 × 크기 묶음(지름 0.25 m 부터 2 배 간격). 묶음마다 «가장 큰 지름» 을 들고 있어,
	//  칸이 통째로 반대편이거나 60 m 밖인데 가장 큰 것도 32 px 가 안 되는 묶음은 통째로 건너뛴다(11-17 계산: 훑는 양 4 ~ 17 %).
	namespace
	{
		constexpr double kTlasCell        = 128.0;   // m
		constexpr double kTlasNear        = 60.0;    // m — R0: 이 안은 전부 후보 · 단계 0
		constexpr double kTlasMid         = 120.0;   // m — R1: 단계 1
		constexpr double kTlasMinPx       = 32.0;    // 60 m 밖 후보의 최소 화면 지름(px, 조준 기준) — 단계 한 칸 올림 문턱과 같다
		constexpr double kTlasPromote2Px  = 128.0;   // 이 이상이면 단계 두 칸 올림
		// 조준 기준 초점 거리(px) = (화면 높이 720 / 2) ÷ tan(조준 FOV 35° / 2) — Camera.cpp kAimFov, 11-16 결정
		const double kTlasAimFocal = 360.0 / std::tan(17.5 * 3.14159265358979 / 180.0);
	}

	void PlanetDebugScene::BuildObjectGrid()
	{
		gridBuckets.clear();
		gridItems.clear();
		std::vector<std::pair<uint64_t, uint32_t>> keys;
		keys.reserve(staticObjects.size());
		for (uint32_t i = 0; i < static_cast<uint32_t>(staticObjects.size()); ++i)
		{
			const Shared::StaticObject& object = staticObjects[i];
			const ObjectType& type = objectTypes[object.meshIndex];
			if (!type.loaded) continue;
			const double diameter = 2.0 * double(type.radius) * std::fabs(double(object.scale.x));
			const int64_t cx = int64_t(std::floor(double(object.position.x) / kTlasCell)) + 4096;
			const int64_t cy = int64_t(std::floor(double(object.position.y) / kTlasCell)) + 4096;
			const int64_t cz = int64_t(std::floor(double(object.position.z) / kTlasCell)) + 4096;
			const int64_t size = std::clamp<int64_t>(int64_t(std::floor(std::log2((std::max)(diameter, 1.0e-3) / 0.25))), 0, 15);
			const uint64_t key = (uint64_t(cx) << 44) | (uint64_t(cy) << 28) | (uint64_t(cz) << 12) | uint64_t(size);
			keys.emplace_back(key, i);
		}
		std::sort(keys.begin(), keys.end());
		gridItems.reserve(keys.size());
		for (size_t k = 0; k < keys.size(); ++k)
		{
			if (k == 0 || keys[k].first != keys[k - 1].first)
			{
				const uint64_t key = keys[k].first;
				GridBucket bucket;
				bucket.center = Vec3d{ (double(int64_t((key >> 44) & 0xFFFF)) - 4096.0 + 0.5) * kTlasCell,
					(double(int64_t((key >> 28) & 0xFFFF)) - 4096.0 + 0.5) * kTlasCell,
					(double(int64_t((key >> 12) & 0xFFFF)) - 4096.0 + 0.5) * kTlasCell };
				bucket.first = uint32_t(gridItems.size());
				gridBuckets.push_back(bucket);
			}
			const uint32_t i = keys[k].second;
			const Shared::StaticObject& object = staticObjects[i];
			GridBucket& bucket = gridBuckets.back();
			bucket.maxDiameter = (std::max)(bucket.maxDiameter,
				float(2.0 * double(objectTypes[object.meshIndex].radius) * std::fabs(double(object.scale.x))));
			++bucket.count;
			gridItems.push_back(i);
		}
		gridBuilt = true;
		rankValid = false;
	}

	// 오브젝트 모델 하나가 끝났다(올렸든 못 올렸든) — 마지막 종류면 공간 격자를 만든다(로드 때 미리, 2026-11-17
	//  사용자 «격자를 로드 때 미리» — 실행 5 에서 들어간 직후 첫 고르기가 18.6 ~ 72.4 ms 끊겼다).
	void PlanetDebugScene::ObjectModelFinished()
	{
		if (++objectModelsFinished < objectTypes.size()) return;
		const auto start = std::chrono::steady_clock::now();
		BuildObjectGrid();
		Log(L"[디버그 행성] 오브젝트 공간 격자 — 배치 %zu · 묶음 %zu · %.1f ms (로드 때)", gridItems.size(), gridBuckets.size(),
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}

	bool PlanetDebugScene::SelectTlasObjects(const Vec3d& eye, size_t budget, bool force)
	{
		if (!loaded || staticObjects.empty())
		{
			tlasObjects.clear();
			return false;
		}
		const bool moved = !tlasSelected || Length(eye - lastTlasEye) >= kRestreamDistance;
		const bool budgetChanged = budget < tlasObjects.size() || budget >= lastTlasBudget + 64;
		if (!force && !moved && !budgetChanged) return false;

		const auto start = std::chrono::steady_clock::now();
		lastTlasEye = eye;
		lastTlasBudget = budget;
		tlasSelected = true;

		RankObjects(eye, budget);   // 같은 프레임에 래스터(StreamObjects)가 더 큰 예산으로 이미 순위를 냈으면 그것을 쓴다

		// 순서대로 메시 상한까지 — 후보(단계 · px) 다음 채움(px)
		tlasObjects.clear();
		tlasObjects.reserve(budget);
		size_t used = 0;
		auto take = [&](const std::vector<std::pair<float, uint32_t>>& list)
		{
			for (const auto& entry : list)
			{
				const Shared::StaticObject& object = staticObjects[entry.second];
				const ObjectType& type = objectTypes[object.meshIndex];
				if (used + type.meshCount > budget) return false;
				used += type.meshCount;
				XMFLOAT4X4 rootWorld;
				XMStoreFloat4x4(&rootWorld, InstanceMatrix(object) *
					XMMatrixTranslation(float(center.x), float(center.y), float(center.z)));   // 맵 루트(행성 중심) 아래
				type.model.AppendInstances(rootWorld, tlasObjects);
			}
			return true;
		};
		if (take(tlasPrimary)) take(tlasFill);

		lastTlasSelectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		return true;
	}

	// ── 오브젝트 순위 (2026-11-17) — TLAS · 래스터가 같이 쓴다 ──
	//  tlasPrimary = 후보(앞쪽 반구 · 60 m 안이거나 조준 32 px 이상)를 단계 → px 순, tlasFill = 32 px 밑을 px 큰 순.
	//  budget 메시를 채울 만큼만 격자를 훑는다. 눈이 1 m 안이고 budget 이 앞 순위 이하면 앞 순위를 그대로 쓴다
	//  (더 큰 예산의 순위 앞부분 = 작은 예산의 순위 — 채움은 px 문턱 위만 세고 멈추므로).
	void PlanetDebugScene::RankObjects(const Vec3d& eye, size_t budget)
	{
		if (rankValid && Length(eye - rankEye) < 1.0 && budget <= rankBudget) return;
		if (!gridBuilt) BuildObjectGrid();
		rankEye = eye;
		rankBudget = budget;
		rankValid = true;

		const double eyeLength = Length(eye);
		const Vec3d up = eyeLength > 1.0e-9 ? eye * (1.0 / eyeLength) : Vec3d{ 0.0, 1.0, 0.0 };
		const double half = kTlasCell * std::sqrt(3.0) * 0.5;
		tlasPrimary.clear();
		tlasFill.clear();
		bucketExamined.assign(gridBuckets.size(), 0);
		size_t examined = 0;
		size_t farSkipped = 0;

		// 배치 하나 — 후보면 tlasPrimary(정렬 키), 아니면 minPx 이상일 때 tlasFill(px)
		auto visit = [&](uint32_t i, double minPx)
		{
			const Shared::StaticObject& object = staticObjects[i];
			const Vec3d position{ double(object.position.x), double(object.position.y), double(object.position.z) };
			if (Dot(position, eye) < 0.0) { ++farSkipped; return; }   // 반대편 반구(2026-11-16 사용자 지시)
			const ObjectType& type = objectTypes[object.meshIndex];
			const double distance = (std::max)(Length(position - eye), 1.0);
			const double px = 2.0 * double(type.radius) * std::fabs(double(object.scale.x)) * kTlasAimFocal / distance;
			if (distance <= kTlasNear || px >= kTlasMinPx)
			{
				int tier = distance <= kTlasNear ? 0 : (distance <= kTlasMid ? 1 : 2);
				tier -= px >= kTlasPromote2Px ? 2 : (px >= kTlasMinPx ? 1 : 0);
				tier = (std::max)(tier, 0);
				tlasPrimary.emplace_back(float(double(tier) * 1.0e6 - px), i);
			}
			else if (px >= minPx)
				tlasFill.emplace_back(float(px), i);
		};

		// ① 후보 — 60 m 안이거나 32 px 이상이 될 수 있는 묶음만 훑는다
		for (size_t b = 0; b < gridBuckets.size(); ++b)
		{
			const GridBucket& bucket = gridBuckets[b];
			if (Dot(bucket.center, up) + half < 0.0) { bucketExamined[b] = 1; farSkipped += bucket.count; continue; }   // 칸 통째로 반대편
			const double nearest = (std::max)(Length(bucket.center - eye) - half, 1.0);
			if (nearest > kTlasNear && double(bucket.maxDiameter) * kTlasAimFocal / nearest < kTlasMinPx) continue;
			bucketExamined[b] = 1;
			for (uint32_t k = 0; k < bucket.count; ++k)
				visit(gridItems[bucket.first + k], 0.0);   // 같은 묶음의 32 px 밑은 채움 후보로 모은다(이미 훑었으니)
			examined += bucket.count;
		}
		size_t candidateMeshes = 0;
		for (const auto& entry : tlasPrimary)
			candidateMeshes += objectTypes[staticObjects[entry.second].meshIndex].meshCount;
		lastTlasCandidateMeshes = candidateMeshes;

		// ② 자리가 남으면 32 px 밑을 마저 채운다 — 문턱을 반씩 낮추며 안 훑은 묶음을 더 훑는다(모자라지 않을 때까지)
		//  모은 채움 후보 중 minPx 이상의 메시 수 — 안 훑은 묶음의 것은 모두 문턱 밑이라, 이 수가 남는 자리를 채우면 멈춰도 순서가 맞다.
		auto fillMeshes = [&](double minPx)
		{
			size_t meshes = 0;
			for (const auto& entry : tlasFill)
				if (double(entry.first) >= minPx) meshes += objectTypes[staticObjects[entry.second].meshIndex].meshCount;
			return meshes;
		};
		for (double threshold = kTlasMinPx * 0.5; candidateMeshes < budget; threshold *= 0.5)
		{
			const double minPx = threshold < 1.0 ? 0.0 : threshold;
			for (size_t b = 0; b < gridBuckets.size(); ++b)
			{
				if (bucketExamined[b]) continue;
				const GridBucket& bucket = gridBuckets[b];
				const double nearest = (std::max)(Length(bucket.center - eye) - half, 1.0);
				if (minPx > 0.0 && double(bucket.maxDiameter) * kTlasAimFocal / nearest < minPx) continue;
				bucketExamined[b] = 1;
				for (uint32_t k = 0; k < bucket.count; ++k)
					visit(gridItems[bucket.first + k], 0.0);
				examined += bucket.count;
			}
			if (minPx == 0.0 || candidateMeshes + fillMeshes(minPx) >= budget) break;
		}
		lastTlasExamined = examined;
		farHemisphereSkipped = farSkipped;   // 창 제목 «반대편 제외» — 통째로 건너뛴 칸 + 훑은 칸에서 뺀 것

		// ③ 정렬 — 후보(단계 · px), 채움(px 큰 순)
		std::sort(tlasPrimary.begin(), tlasPrimary.end(),
			[](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) { return a.first < b.first; });
		std::sort(tlasFill.begin(), tlasFill.end(),
			[](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) { return a.first > b.first; });
	}

	// ── 래스터 칸 리스트 (2026-11-17) — PlanetDebugScene.h BuildRasterLists 주석 ──
	namespace
	{
		constexpr double kRasterBigDiameter = 4.0;     // m — 이 이상은 큰 리스트(사용자 «문턱은 4m»)
		constexpr size_t kRasterListMax     = 8192;    // 리스트 하나 최대 메시(사용자 «리스트 하나에 최대 8192») = GRenderer MaxDrawInstances
		constexpr double kRasterRingPx[4]   = { 0.0, 8.0, 16.0, 32.0 };   // 링 0 · 1 · 2 · 3 이상의 px 문턱(조준 기준)
	}

	bool PlanetDebugScene::BuildRasterLists(const Vec3d& eye, bool force)
	{
		if (!loaded || staticObjects.empty())
		{
			rasterLists.clear();
			return false;
		}
		const int64_t cell[3] = { int64_t(std::floor(eye.x / kTlasCell)), int64_t(std::floor(eye.y / kTlasCell)),
			int64_t(std::floor(eye.z / kTlasCell)) };
		if (!force && rasterBuilt && cell[0] == rasterCell[0] && cell[1] == rasterCell[1] && cell[2] == rasterCell[2])
			return false;
		const auto start = std::chrono::steady_clock::now();
		if (!gridBuilt) BuildObjectGrid();
		rasterBuilt = true;
		rasterCell[0] = cell[0]; rasterCell[1] = cell[1]; rasterCell[2] = cell[2];

		const double eyeLength = Length(eye);
		const Vec3d up = eyeLength > 1.0e-9 ? eye * (1.0 / eyeLength) : Vec3d{ 0.0, 1.0, 0.0 };
		const double half = kTlasCell * std::sqrt(3.0) * 0.5;

		// ① 고르기 — 링별 작은 것 · 큰 것 후보(px · 링)
		struct BigPick { float px; uint32_t index; int64_t ring; };
		std::vector<std::vector<uint32_t>> ringSmall;
		std::vector<BigPick> big;
		for (const GridBucket& bucket : gridBuckets)
		{
			if (Dot(bucket.center, up) + half < 0.0) continue;   // 칸 통째로 반대편 반구
			const int64_t bc[3] = { int64_t(std::floor(bucket.center.x / kTlasCell)), int64_t(std::floor(bucket.center.y / kTlasCell)),
				int64_t(std::floor(bucket.center.z / kTlasCell)) };
			const int64_t ring = (std::max)({ std::llabs(bc[0] - cell[0]), std::llabs(bc[1] - cell[1]), std::llabs(bc[2] - cell[2]) });
			const double minPx = kRasterRingPx[(std::min)(ring, int64_t(3))];
			if (minPx > 0.0)
			{
				const double nearest = (std::max)(Length(bucket.center - eye) - half, 1.0);
				if (double(bucket.maxDiameter) * kTlasAimFocal / nearest < minPx) continue;   // 묶음에서 가장 큰 것도 문턱 밑
			}
			for (uint32_t k = 0; k < bucket.count; ++k)
			{
				const uint32_t i = gridItems[bucket.first + k];
				const Shared::StaticObject& object = staticObjects[i];
				const Vec3d position{ double(object.position.x), double(object.position.y), double(object.position.z) };
				if (Dot(position, eye) < 0.0) continue;   // 반대편 반구(2026-11-16 사용자 지시)
				const double diameter = 2.0 * double(objectTypes[object.meshIndex].radius) * std::fabs(double(object.scale.x));
				const double px = diameter * kTlasAimFocal / (std::max)(Length(position - eye), 1.0);
				if (px < minPx) continue;
				if (diameter >= kRasterBigDiameter)
					big.push_back({ float(px), i, ring });
				else
				{
					if (ringSmall.size() <= size_t(ring)) ringSmall.resize(size_t(ring) + 1);
					ringSmall[size_t(ring)].push_back(i);
				}
			}
		}

		// ② 리스트 — 링 0(플레이어 칸) 하나 · 링 1 ~ 마다 8,192 씩 · 큰 것 하나 · 임시 하나
		std::vector<std::vector<InstanceData>> built;
		std::vector<InstanceData> temp;
		rasterSmallMeshes = rasterBigMeshes = rasterTempMeshes = rasterCutBigMeshes = 0;
		const XMMATRIX toWorld = XMMatrixTranslation(float(center.x), float(center.y), float(center.z));   // 맵 루트(행성 중심) 아래
		auto append = [&](std::vector<InstanceData>& out, uint32_t i)
		{
			const Shared::StaticObject& object = staticObjects[i];
			XMFLOAT4X4 rootWorld;
			XMStoreFloat4x4(&rootWorld, InstanceMatrix(object) * toWorld);
			objectTypes[object.meshIndex].model.AppendInstances(rootWorld, out);
		};
		auto cost = [&](uint32_t i) { return size_t(objectTypes[staticObjects[i].meshIndex].meshCount); };
		for (size_t ring = 0; ring < ringSmall.size(); ++ring)
		{
			if (ringSmall[ring].empty()) continue;
			built.emplace_back();
			for (const uint32_t i : ringSmall[ring])
			{
				if (built.back().size() + cost(i) > kRasterListMax)
				{
					if (ring == 0)   // 플레이어 칸이 넘치면 임시 리스트로(사용자 «플레이어가 속한 격자인데 안그려지는거는 임시 리스트에 전부»)
					{
						append(temp, i);
						continue;
					}
					built.emplace_back();   // 링 1 ~ 은 리스트를 하나 더
				}
				append(built.back(), i);
			}
		}
		for (const std::vector<InstanceData>& list : built) rasterSmallMeshes += list.size();
		std::sort(big.begin(), big.end(), [](const BigPick& a, const BigPick& b) { return a.px > b.px; });
		std::vector<InstanceData> bigList;
		for (const BigPick& pick : big)
		{
			if (bigList.size() + cost(pick.index) <= kRasterListMax)
				append(bigList, pick.index);
			else if (pick.ring == 0)   // 큰 리스트에서 잘렸는데 플레이어 칸이면 임시 리스트로
				append(temp, pick.index);
			else
				rasterCutBigMeshes += cost(pick.index);
		}
		rasterBigMeshes = bigList.size();
		if (!bigList.empty()) built.push_back(std::move(bigList));
		if (temp.size() > kRasterListMax) temp.resize(kRasterListMax);   // 임시 리스트도 하나 최대 8,192
		rasterTempMeshes = temp.size();
		if (!temp.empty()) built.push_back(std::move(temp));

		rasterLists.clear();
		rasterLists.reserve(built.size());
		for (std::vector<InstanceData>& list : built)
		{
			if (list.empty()) continue;
			InstanceList entry;
			entry.items = std::make_shared<const std::vector<InstanceData>>(std::move(list));
			entry.version = ++rasterVersion;
			rasterLists.push_back(std::move(entry));
		}
		lastRasterBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		return true;
	}

	// ── 지형 레이어 (2026-10-28) ────────────────────────────────────
	//  레이어 텍스처는 resource dfa4fe4 의 밉맵 DDS(원본 PNG 옆, 같은 이름)를 읽는다 — glb 에는 대체 레이어 한 장뿐이다.
	std::vector<std::wstring> PlanetDebugScene::TerrainLayerAssets(size_t index, const PlanetPlacement& placement)
	{
		std::vector<std::wstring> assets;
		if (index >= kPlanets.size() || placement.terrainMask.empty()) return assets;
		for (const std::string& layer : placement.terrainLayers)
			if (layer.empty()) return assets;

		const std::wstring folder = std::filesystem::path(Asset(index)).parent_path().wstring() + L"\\";
		const wchar_t* const kinds[4] = { L"_BaseColor.dds", L"_Normal.dds", L"_ORM.dds", L"_Height.dds" };
		for (const wchar_t* kind : kinds)
			for (const std::string& layer : placement.terrainLayers)
				assets.push_back(folder + FromUtf8(layer) + kind);
		// 마스크 — JSON 에는 PNG 이름(…_LayerMask.png)이 적혀 있다. 같은 이름의 DDS 를 읽는다.
		std::filesystem::path mask(FromUtf8(placement.terrainMask));
		mask.replace_extension(L".dds");
		assets.push_back(folder + mask.wstring());
		return assets;
	}

	bool PlanetDebugScene::LoadTerrainLayers(const std::vector<std::wstring>& paths, TerrainLayerSet& out)
	{
		if (paths.size() != 17)
		{
			out.error = L"지형 레이어 파일 목록이 17개가 아닙니다.";
			return false;
		}
		TextureData* const arrays[4] = { &out.baseColor, &out.normal, &out.orm, &out.height };
		for (size_t kind = 0; kind < 4; ++kind)
		{
			std::vector<TextureData> slices(4);
			for (size_t layer = 0; layer < 4; ++layer)
				if (!LoadDdsTexture(paths[kind * 4 + layer].c_str(), slices[layer], out.error)) return false;
			if (!CombineTextureArray(slices, *arrays[kind], out.error)) return false;
		}
		return LoadDdsTexture(paths[16].c_str(), out.mask, out.error);
	}

	bool PlanetDebugScene::ApplyTerrainLayers(size_t index, TerrainLayerSet&& layers, GRenderer& renderer,
		LegacyScene& scene, std::wstring& error)
	{
		if (!loaded || index >= kPlanets.size())
		{
			error = L"행성 맵이 아직 올라가지 않았습니다.";
			return false;
		}
		if (terrainNodes.empty())
		{
			error = L"지형 노드(…_Planet_Terrain)를 맵에서 찾지 못했습니다.";
			return false;
		}

		// 텍스처 5개 — 올린 뒤 바로 픽셀을 놓는다(무압축 + 밉이라 행성 하나에 약 360 MiB).
		TextureData* const sources[5] = { &layers.baseColor, &layers.normal, &layers.orm, &layers.height, &layers.mask };
		TextureHandle handles[5] = {};
		for (size_t i = 0; i < 5; ++i)
		{
			handles[i] = renderer.CreateTexture(*sources[i]);
			std::vector<uint8_t>().swap(sources[i]->pixels);
			if (handles[i] == kInvalidTexture)
			{
				error = L"지형 레이어 텍스처 업로드 실패: " + renderer.StatusText();
				return false;
			}
			terrainTextures.push_back(handles[i]);   // 2026-11-17 — 행성을 내릴 때(Release) 같이 놓는다
		}

		TerrainMaterialData material;
		material.name = "Terrain_Layers";
		material.heightBlend = kPlanets[index].heightBlend;
		material.normalStrength = kPlanets[index].normalStrength;
		material.triplanarLayers = kPlanets[index].triplanarLayers;   // 3단계 (2026-10-28)
		material.triplanarTileA = kPlanets[index].triplanarTileA;
		material.triplanarTileB = kPlanets[index].triplanarTileB;
		material.triplanarOffsetB = kPlanets[index].triplanarOffsetB;
		material.warpScale = kPlanets[index].warpScale;
		material.warpAmount = kPlanets[index].warpAmount;
		material.macroAmount = kPlanets[index].macroAmount;           // 4단계 (2026-10-28)
		material.macroBias = kPlanets[index].macroBias;
		material.bumpDistance = kPlanets[index].bumpDistance;
		material.reliefDistance = kPlanets[index].reliefDistance;
		material.reliefStrength = kPlanets[index].reliefStrength;
		// 2026-10-29 — 레이어 좌표의 원점 = 월드의 행성 중심(셰이더가 «월드 위치 − 이 값» 으로 마스크 · 무늬를 읽는다)
		material.origin = XMFLOAT3(float(center.x), float(center.y), float(center.z));
		const MaterialHandle handle = renderer.CreateTerrainMaterial(material,
			handles[0], handles[1], handles[2], handles[3], handles[4]);
		if (handle == kInvalidMaterial)
		{
			error = renderer.StatusText();
			return false;
		}
		terrainMaterial = handle;   // 2026-11-17 — 행성을 내릴 때(Release) 같이 놓는다
		for (const NodeHandle node : terrainNodes)
			scene.SetMaterial(node, handle);
		terrainLayered = true;

		// 땅 위 메시 (2026-10-29) — 오솔길 · 착륙 대지 중 재질이 «지형과 같은 땅» 인 것은 같은 지형 레이어 재질로 그린다:
		//  glb 지형 재질과 이름이 같거나(빙하 Moraine_Gravel · 숲 착륙 대지 Verdant_Ground), 레이어 이름이거나(화산 Lava_Crust),
		//  행성 표의 길 재질(숲 Verdant_Trail = Dirt_Trail). 나머지(네온 · 폐허 도로 · 포장)는 glb 재질 그대로 둔다.
		layeredOverlays = 0;
		const char* const trail = kPlanets[index].trailMaterial;
		for (const auto& [node, name] : overlayNodes)
		{
			const bool ground = !name.empty() && (name == terrainMaterialName ||
				std::find(terrainLayerNames.begin(), terrainLayerNames.end(), name) != terrainLayerNames.end() ||
				(trail != nullptr && name == trail));
			if (!ground) continue;
			scene.SetMaterial(node, handle);
			++layeredOverlays;
		}
		Log(L"[디버그 행성] %s 땅 위 메시 — 오솔길 · 착륙 대지 %zu / %zu 개를 지형 레이어로, 얼음 호수 물가 페이드 %zu 개",
			kPlanets[index].name, layeredOverlays, overlayNodes.size(), fadedLakes);
		return true;
	}

	std::wstring PlanetDebugScene::Status() const
	{
		size_t loadedTypes = 0;
		for (const ObjectType& type : objectTypes)
			if (type.loaded) ++loadedTypes;

		std::wstring status = L"지형 " + std::to_wstring(ground.TriangleCount()) + L" 삼각형";
		status += terrainLayered ? L" · 지형 레이어 섞기" : L" · 지형 대체 1장";   // 2026-10-28
		if (layeredOverlays > 0)   // 2026-10-29 — 지형 레이어로 그리는 오솔길 · 착륙 대지
			status += L" (길 · 대지 " + std::to_wstring(layeredOverlays) + L")";
		if (fadedLakes > 0)        // 2026-10-29 — 물가 페이드를 켠 얼음 호수
			status += L" · 호수 물가 " + std::to_wstring(fadedLakes);
		if (!objectTypes.empty())
		{
			status += L" · 오브젝트 보임 " + std::to_wstring(liveList.size()) + L" / 배치 " +
				std::to_wstring(staticObjects.size()) + L" · 종류 " + std::to_wstring(loadedTypes) + L"/" +
				std::to_wstring(objectTypes.size());
			// 2026-11-16 — 고른 메시 수 / 예산 · 반대편 반구라 뺀 오브젝트 수(실행 중 확인용)
			// 2026-11-17 render/tlas-select — 래스터 칸 리스트(BuildRasterLists): 리스트 수 · 작은 것 · 큰 것 · 임시 메시
			status += L" · 래스터 리스트 " + std::to_wstring(rasterLists.size()) + L"개(작은 " + std::to_wstring(rasterSmallMeshes) +
				L" · 큰 " + std::to_wstring(rasterBigMeshes) + L" · 임시 " + std::to_wstring(rasterTempMeshes) + L")";
			if (skippedInstances > 0)
				status += L" · 건너뜀 " + std::to_wstring(skippedInstances);
		}
		return status;
	}

} // namespace swc
