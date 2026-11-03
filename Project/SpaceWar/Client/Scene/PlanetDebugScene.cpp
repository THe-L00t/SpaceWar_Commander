#include "PlanetDebugScene.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
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
	}

	std::wstring PlanetDebugScene::ObjectAsset(size_t type) const
	{
		if (type >= objectTypes.size()) return std::wstring();
		std::wstring relative = FromUtf8(objectTypes[type].file);
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

		target.meshCount = 0;
		for (const ModelNodeData& node : data.nodes)
			target.meshCount += static_cast<uint32_t>(node.meshes.size());
		target.loaded = target.meshCount > 0;
		if (!target.loaded)
			error = L"그릴 메시가 없습니다.";
		return target.loaded;
	}

	// ── 오브젝트 고르기 (메인 · 매 프레임) ───────────────────────────
	bool PlanetDebugScene::StreamObjects(LegacyScene& scene, const Vec3d& eye, bool force)
	{
		if (!loaded || root == kInvalidNode || staticObjects.empty()) return false;
		if (!force && streamed && Length(eye - lastStreamEye) < kRestreamDistance) return false;
		lastStreamEye = eye;
		streamed = true;

		// ① 점수 = 반지름 × 배율 ÷ 거리 (화면 크기 근사). 모델을 못 올린 종류는 뺀다.
		scratch.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(staticObjects.size()); ++i)
		{
			const Shared::StaticObject& object = staticObjects[i];
			const ObjectType& type = objectTypes[object.meshIndex];
			if (!type.loaded) continue;
			const double dx = double(object.position.x) - eye.x;
			const double dy = double(object.position.y) - eye.y;
			const double dz = double(object.position.z) - eye.z;
			const double distance = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0);
			scratch.emplace_back(static_cast<float>(type.radius * std::fabs(object.scale.x) / distance), i);
		}

		// ② 점수 높은 순으로 메시 노드 예산까지. 인스턴스 하나는 메시 1개 이상이라 앞쪽 kObjectMeshBudget 개면 충분하다.
		const auto byScore = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b)
		{
			return a.first > b.first;
		};
		const size_t candidates = (std::min)(scratch.size(), kObjectMeshBudget);
		if (candidates < scratch.size())
			std::nth_element(scratch.begin(), scratch.begin() + candidates, scratch.end(), byScore);
		std::sort(scratch.begin(), scratch.begin() + candidates, byScore);

		if (++stamp == 0)   // 한 바퀴 돌면 표시를 지우고 1 부터
		{
			std::fill(selectStamp.begin(), selectStamp.end(), 0u);
			stamp = 1;
		}
		size_t selected = 0;
		size_t meshes = 0;
		for (; selected < candidates; ++selected)
		{
			const uint32_t i = scratch[selected].second;
			const size_t cost = objectTypes[staticObjects[i].meshIndex].meshCount;
			if (meshes + cost > kObjectMeshBudget) break;
			meshes += cost;
			selectStamp[i] = stamp;
		}

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
			if (skippedInstances > 0)
				status += L" · 건너뜀 " + std::to_wstring(skippedInstances);
		}
		return status;
	}

} // namespace swc
