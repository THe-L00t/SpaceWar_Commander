#include "Placement.h"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include "../Model/Readers/JsonValue.h"

// 머리 주석은 Placement.h. 본문은 Client/Scene/PlanetDebugScene.cpp ReadPlacement(2026-10-28)를 그대로 옮겼다(2026-11-01).

namespace {

	// 배치 JSON 의 인스턴스 배열 키. instances 는 JSON 의 마지막 키다(다섯 행성 모두 확인, 2026-10-28).
	constexpr std::string_view kInstancesKey = "\"instances\"";
	constexpr size_t kInstanceFields = 9;   // [id, x, y, z, qx, qy, qz, qw, scale]

	// JSON 문자열(UTF-8) → 와이드. 오류 메시지용. 잘못된 UTF-8 이면 바이트를 그대로 넓힌다.
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

} // namespace

namespace Shared {

	// ① 머리말(instances 앞까지)만 최소 JSON 파서(Shared::JsonValue)로 읽는다 — objects 목록.
	// ② instances 는 [id, x, y, z, qx, qy, qz, qw, scale] 배열의 배열이라 숫자만 직접 훑는다(from_chars).
	bool ReadPlacement(const std::wstring& path, PlacementData& out)
	{
		out = PlacementData{};

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
		JsonValue root;
		std::string jsonError;
		if (!JsonValue::Parse(header.data(), header.size(), root, jsonError))
		{
			out.error = L"배치 JSON 머리말 해석 실패: " + FromUtf8(jsonError);
			return false;
		}

		std::unordered_map<uint32_t, uint32_t> typeOfId;   // JSON id → types 번호
		const JsonValue& objects = root["objects"];
		for (size_t i = 0; i < objects.Size(); ++i)
		{
			const JsonValue& object = objects[i];
			PlacementData::Type type;
			type.name = object["name"].AsString();
			type.file = object["file"].AsString();
			if (type.file.empty() || !object.Has("id")) continue;
			typeOfId.emplace(object["id"].AsUint(), static_cast<uint32_t>(out.types.size()));
			out.types.push_back(std::move(type));
		}
		out.instances.reserve(root["instance_count"].AsUint(0));

		// 지형 레이어 (2026-10-28) — terrain_layers.channels 의 R G B A = 레이어 이름, mask = 마스크 PNG 이름.
		//  없으면 빈 채로 둔다 → 지형은 glb 의 대체 레이어 한 장으로 그린다.
		const JsonValue& terrainLayers = root["terrain_layers"];
		const JsonValue& channels = terrainLayers["channels"];
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

			PlacementData::Instance instance;
			instance.type = found->second;
			for (size_t a = 0; a < 3; ++a) instance.position[a] = static_cast<float>(values[1 + a]);
			for (size_t a = 0; a < 4; ++a) instance.rotation[a] = static_cast<float>(values[4 + a]);
			instance.scale = static_cast<float>(values[8]);
			out.instances.push_back(instance);
		}
		return true;
	}

	std::vector<std::string> PlacementTypeNames(const PlacementData& placement)
	{
		std::vector<std::string> names;
		names.reserve(placement.types.size());
		for (const PlacementData::Type& type : placement.types)
			names.push_back(type.name);
		return names;
	}

} // namespace Shared
