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
#include "Client/Resource/TextureLoader.h"
#include "Shared/Model/Readers/JsonValue.h"

using namespace DirectX;

namespace {

	struct PlanetEntry
	{
		const wchar_t* name;
		const wchar_t* asset;
		const wchar_t* placement;
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
	};

	// resource 브랜치 예시 행성 (fa6777c · 8bafbc6 · 6afb98d · c51ed07 · 5dfe6bd)
	constexpr std::array<PlanetEntry, swc::PlanetDebugScene::kCount> kPlanets{ {
		{ L"Verdant",  L"model\\map\\Verdant_Planet\\Verdant_Planet.glb",
		               L"model\\map\\Verdant_Planet\\Verdant_Planet_Placement.json",   { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f },
		{ L"Neon",     L"model\\map\\Neon_Planet\\Neon_Planet.glb",
		               L"model\\map\\Neon_Planet\\Neon_Planet_Placement.json",         { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f },
		{ L"Ruin",     L"model\\map\\Ruin_Planet\\Ruin_Planet.glb",
		               L"model\\map\\Ruin_Planet\\Ruin_Planet_Placement.json",         { 0.5f, 0.5f, 0.5f, 0.5f }, 1.2f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.06f, 0.45f, 0.8f },
		{ L"Volcanic", L"model\\map\\Volcanic_Planet\\Volcanic_Planet.glb",
		               L"model\\map\\Volcanic_Planet\\Volcanic_Planet_Placement.json", { 0.5f, 0.5f, 0.5f, 0.5f }, 1.6f,
		               2u, 9.0f, 2.7f, false, 0.0f, 0.0f,
		               0.32f, 0.84f, 0.09f, 0.55f, 0.9f },
		// 빙하: A = Snow 만 0.35 (시안 «0.35 if name == 'Snow' else 0.5»)
		{ L"Glacier",  L"model\\map\\Glacier_Planet\\Glacier_Planet.glb",
		               L"model\\map\\Glacier_Planet\\Glacier_Planet_Placement.json",   { 0.5f, 0.5f, 0.5f, 0.35f }, 1.4f,
		               2u | 4u, 8.0f, 8.0f * 0.37f, true, 70.0f, 10.0f,
		               0.22f, 0.89f, 0.07f, 0.45f, 0.7f },
	} };

	// 보이는 지형 메시의 노드 — «<행성>_Planet_Terrain». 같은 메시를 쓰는 접지용 «COL_Expedition_Planet_Terrain» 은 뺀다
	// (다섯 행성 glb 확인, 2026-10-28).
	bool IsTerrainNode(std::string_view name)
	{
		constexpr std::string_view suffix = "_Planet_Terrain";
		return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix &&
			name.substr(0, 4) != "COL_";
	}

	// 스폰 = 착륙 지점 L1 의 플랫폼 노드. 예시 행성은 L1 방향이 게임 맵(kPlanetSpawnUp)과 달라
	// 맵 노드에서 읽는다(resource fa6777c 커밋의 «주의»). 다섯 행성 모두 최상위 노드다.
	constexpr std::string_view kSpawnNode = "Landing_L1_Platform";

	// 배치 JSON 의 인스턴스 배열 키. instances 는 JSON 의 마지막 키다(다섯 행성 모두 확인, 2026-10-28).
	constexpr std::string_view kInstancesKey = "\"instances\"";
	constexpr size_t kInstanceFields = 9;   // [id, x, y, z, qx, qy, qz, qw, scale]

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

	// 인스턴스 변환 — 파서의 노드 변환과 같은 S · R · T (행 벡터, GltfReader.cpp NodeLocal).
	XMMATRIX InstanceMatrix(const swc::PlanetPlacement::Instance& instance)
	{
		XMVECTOR rotation = XMVectorSet(instance.rotation[0], instance.rotation[1],
			instance.rotation[2], instance.rotation[3]);
		rotation = XMVectorGetX(XMVector4LengthSq(rotation)) > 1.0e-12f
			? XMQuaternionNormalize(rotation) : XMQuaternionIdentity();   // JSON 은 소수 셋째 자리로 반올림돼 있다
		return XMMatrixScaling(instance.scale, instance.scale, instance.scale) *
			XMMatrixRotationQuaternion(rotation) *
			XMMatrixTranslation(instance.position[0], instance.position[1], instance.position[2]);
	}

}

namespace swc {

	const wchar_t* PlanetDebugScene::Name(size_t index)
	{
		return index < kPlanets.size() ? kPlanets[index].name : L"";
	}

	const wchar_t* PlanetDebugScene::Asset(size_t index)
	{
		return index < kPlanets.size() ? kPlanets[index].asset : L"";
	}

	const wchar_t* PlanetDebugScene::PlacementAsset(size_t index)
	{
		return index < kPlanets.size() ? kPlanets[index].placement : L"";
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
		std::vector<uint32_t> terrainMeshes;
		for (const ModelNodeData& node : data.nodes)
			if (IsTerrainNode(node.name))
				terrainMeshes.insert(terrainMeshes.end(), node.meshes.begin(), node.meshes.end());
		std::vector<std::pair<uint32_t, NodeHandle>> meshNodes;
		root = model.Instantiate(scene, kInvalidNode, &meshNodes);
		if (root == kInvalidNode)
		{
			error = L"행성 모델을 장면에 등록하지 못했습니다.";
			return false;
		}
		terrainNodes.clear();
		for (const auto& [mesh, node] : meshNodes)
			if (std::find(terrainMeshes.begin(), terrainMeshes.end(), mesh) != terrainMeshes.end())
				terrainNodes.push_back(node);
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
	//  ① 머리말(instances 앞까지)만 최소 JSON 파서(Shared::JsonValue)로 읽는다 — objects 목록.
	//     instances 를 값 트리로 만들면 37만 × 9 개 값이라 수백 MB 가 된다.
	//  ② instances 는 [id, x, y, z, qx, qy, qz, qw, scale] 배열의 배열이라 숫자만 직접 훑는다(from_chars).
	bool PlanetDebugScene::ReadPlacement(const std::wstring& path, PlanetPlacement& out)
	{
		out = PlanetPlacement{};

		std::ifstream file(std::filesystem::path(path), std::ios::binary);
		if (!file)
		{
			out.error = L"배치 JSON 을 열지 못했습니다: " + path;
			return false;
		}
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		const size_t key = text.rfind(kInstancesKey);
		if (key == std::string::npos)
		{
			out.error = L"배치 JSON 에 instances 가 없습니다: " + path;
			return false;
		}

		// ① 머리말 — instances 를 빈 배열로 바꿔 닫는다. (instances 는 마지막 키라 앞에 쉼표가 있다)
		const std::string header = text.substr(0, key) + "\"instances\": []}";
		Shared::JsonValue root;
		std::string jsonError;
		if (!Shared::JsonValue::Parse(header.data(), header.size(), root, jsonError))
		{
			out.error = L"배치 JSON 머리말 해석 실패: " + FromUtf8(jsonError);
			return false;
		}

		std::unordered_map<uint32_t, uint32_t> typeOfId;   // JSON id → types 번호
		const Shared::JsonValue& objects = root["objects"];
		for (size_t i = 0; i < objects.Size(); ++i)
		{
			const Shared::JsonValue& object = objects[i];
			PlanetPlacement::Type type;
			type.name = object["name"].AsString();
			type.file = object["file"].AsString();
			if (type.file.empty() || !object.Has("id")) continue;
			typeOfId.emplace(object["id"].AsUint(), static_cast<uint32_t>(out.types.size()));
			out.types.push_back(std::move(type));
		}
		out.instances.reserve(root["instance_count"].AsUint(0));

		// 지형 레이어 (2026-10-28) — terrain_layers.channels 의 R G B A = 레이어 이름, mask = 마스크 PNG 이름.
		//  없으면 빈 채로 둔다 → 지형은 glb 의 대체 레이어 한 장으로 그린다.
		const Shared::JsonValue& terrainLayers = root["terrain_layers"];
		const Shared::JsonValue& channels = terrainLayers["channels"];
		const char* const channelKeys[4] = { "R", "G", "B", "A" };
		for (size_t c = 0; c < 4; ++c)
			out.terrainLayers[c] = channels[channelKeys[c]].AsString();
		out.terrainMask = terrainLayers["mask"].AsString();

		// ② 인스턴스
		const char* const begin = text.data();
		const char* const end = begin + text.size();
		const char* c = begin + key + kInstancesKey.size();
		const auto skipSpace = [&]()
		{
			while (c < end && (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t')) ++c;
		};
		const auto fail = [&](const wchar_t* what)
		{
			out.error = std::wstring(L"배치 JSON instances 해석 실패(") + what + L", 바이트 " +
				std::to_wstring(size_t(c - begin)) + L"): " + path;
			return false;
		};

		skipSpace();
		if (c >= end || *c != ':') return fail(L"':' 없음");
		++c;
		skipSpace();
		if (c >= end || *c != '[') return fail(L"'[' 없음");
		++c;

		for (;;)
		{
			skipSpace();
			if (c >= end) return fail(L"배열이 닫히지 않음");
			if (*c == ']') break;
			if (*c == ',') { ++c; continue; }
			if (*c != '[') return fail(L"인스턴스가 배열이 아님");
			++c;

			double values[kInstanceFields] = {};
			for (size_t k = 0; k < kInstanceFields; ++k)
			{
				skipSpace();
				const std::from_chars_result parsed = std::from_chars(c, end, values[k]);
				if (parsed.ec != std::errc()) return fail(L"숫자가 아님");
				c = parsed.ptr;
				skipSpace();
				if (k + 1 < kInstanceFields)
				{
					if (c >= end || *c != ',') return fail(L"숫자 9개가 아님");
					++c;
				}
			}
			skipSpace();
			if (c >= end || *c != ']') return fail(L"숫자 9개가 아님");
			++c;

			bool finite = true;
			for (const double value : values) finite = finite && std::isfinite(value);
			const auto found = (finite && values[0] >= 0.0) ? typeOfId.find(static_cast<uint32_t>(values[0])) : typeOfId.end();
			if (found == typeOfId.end())
			{
				++out.skipped;
				continue;
			}

			PlanetPlacement::Instance instance;
			instance.type = found->second;
			for (size_t a = 0; a < 3; ++a) instance.position[a] = static_cast<float>(values[1 + a]);
			for (size_t a = 0; a < 4; ++a) instance.rotation[a] = static_cast<float>(values[4 + a]);
			instance.scale = static_cast<float>(values[8]);
			out.instances.push_back(instance);
		}
		return true;
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
		instances = std::move(placement.instances);
		skippedInstances = placement.skipped;

		liveSlot.assign(instances.size(), kInvalidNode);
		selectStamp.assign(instances.size(), 0);
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
		if (!loaded || root == kInvalidNode || instances.empty()) return false;
		if (!force && streamed && Length(eye - lastStreamEye) < kRestreamDistance) return false;
		lastStreamEye = eye;
		streamed = true;

		// ① 점수 = 반지름 × 배율 ÷ 거리 (화면 크기 근사). 모델을 못 올린 종류는 뺀다.
		scratch.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i)
		{
			const PlanetPlacement::Instance& instance = instances[i];
			const ObjectType& type = objectTypes[instance.type];
			if (!type.loaded) continue;
			const double dx = double(instance.position[0]) - eye.x;
			const double dy = double(instance.position[1]) - eye.y;
			const double dz = double(instance.position[2]) - eye.z;
			const double distance = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0);
			scratch.emplace_back(static_cast<float>(type.radius * std::fabs(instance.scale) / distance), i);
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
			const size_t cost = objectTypes[instances[i].type].meshCount;
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
			scene.SetVisible(liveSlot[i], false);
			objectTypes[instances[i].type].freeSlots.push_back(liveSlot[i]);
			liveSlot[i] = kInvalidNode;
		}
		liveList.resize(kept);

		// ④ 새로 든 것을 놓는다 — 빈 자리가 있으면 다시 쓰고, 없으면 모델을 펼쳐 새로 만든다.
		for (size_t k = 0; k < selected; ++k)
		{
			const uint32_t i = scratch[k].second;
			if (liveSlot[i] != kInvalidNode) continue;

			ObjectType& type = objectTypes[instances[i].type];
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
			scene.SetLocalTransform(slot, InstanceMatrix(instances[i]));
			liveSlot[i] = slot;
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

		const std::wstring folder = std::filesystem::path(kPlanets[index].asset).parent_path().wstring() + L"\\";
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
		return true;
	}

	std::wstring PlanetDebugScene::Status() const
	{
		size_t loadedTypes = 0;
		for (const ObjectType& type : objectTypes)
			if (type.loaded) ++loadedTypes;

		std::wstring status = L"지형 " + std::to_wstring(ground.TriangleCount()) + L" 삼각형";
		status += terrainLayered ? L" · 지형 레이어 섞기" : L" · 지형 대체 1장";   // 2026-10-28
		if (!objectTypes.empty())
		{
			status += L" · 오브젝트 보임 " + std::to_wstring(liveList.size()) + L" / 배치 " +
				std::to_wstring(instances.size()) + L" · 종류 " + std::to_wstring(loadedTypes) + L"/" +
				std::to_wstring(objectTypes.size());
			if (skippedInstances > 0)
				status += L" · 건너뜀 " + std::to_wstring(skippedInstances);
		}
		return status;
	}

} // namespace swc
