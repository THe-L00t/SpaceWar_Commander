#include "ResourceManager.h"
// 하이트맵 로더는 서버와 공유하고, OBJ/GLB 렌더링 어댑터는 클라이언트에서만 사용한다.
#include "Shared/Terrain/HeightmapLoader.h"
#include "ObjModelLoader.h"
#include "GlbModelLoader.h"
#include <cwctype>
#include <filesystem>
#include <utility>

namespace swc {

	HeightmapHandle ResourceManager::LoadHeightmap(const wchar_t* path)
	{
		if (!path || !*path) return {};

		const std::wstring key = path;
		if (auto it = pathCache.find(key); it != pathCache.end())
			return it->second;                       // ★ 중복 로드 방지

		Shared::HeightmapData data;
		if (!Shared::LoadHeightmapPng(path, data, lastError))
			return {};

		heightmaps.push_back(std::move(data));
		generations.push_back(1);

		// index 는 1부터 — 0을 무효로 예약
		const HeightmapHandle handle{ uint32_t(heightmaps.size()), 1 };
		pathCache.emplace(key, handle);
		return handle;
	}

	const Shared::HeightmapData* ResourceManager::Get(HeightmapHandle handle) const
	{
		if (!handle.Valid() || handle.index > heightmaps.size())
			return nullptr;
		if (generations[handle.index - 1] != handle.generation)
			return nullptr;                          // 이미 해제된 참조
		return &heightmaps[handle.index - 1];
	}

	ModelHandle ResourceManager::LoadModel(const wchar_t* path)
	{
		lastError.clear();
		if (!path || !*path)
		{
			lastError = L"모델 경로가 비어 있습니다.";
			return {};
		}

		std::error_code ec;
		const std::filesystem::path fullPath = std::filesystem::absolute(path, ec);
		if (ec)
		{
			lastError = L"모델 절대 경로를 만들 수 없습니다.";
			return {};
		}
		const std::wstring key = fullPath.lexically_normal().wstring();
		if (auto it = modelPathCache.find(key); it != modelPathCache.end())
			return it->second;

		auto data = std::make_unique<ModelData>();
		std::wstring extension = fullPath.extension().wstring();
		for (wchar_t& value : extension) value = wchar_t(std::towlower(value));
		if (extension == L".glb")
		{
			GlbModelLoader loader;
			if (!loader.Load(key.c_str(), *data, lastError)) return {};
		}
		else
		{
			ObjModelLoader loader;
			if (!loader.Load(key.c_str(), *data, lastError)) return {};
		}

		// 해제한 슬롯을 재사용하고 generation으로 이전 핸들을 구분한다.
		size_t index = 0;
		while (index < models.size() && models[index]) ++index;
		if (index == models.size())
		{
			models.push_back(std::move(data));
			modelGenerations.push_back(1);
		}
		else
			models[index] = std::move(data);
		const ModelHandle handle{ static_cast<uint32_t>(index + 1), modelGenerations[index] };
		modelPathCache.emplace(key, handle);
		return handle;
	}

	void ResourceManager::ReleaseModel(ModelHandle handle)
	{
		if (!Get(handle)) return;
		for (auto it = modelPathCache.begin(); it != modelPathCache.end(); ++it)
		{
			if (it->second.index == handle.index && it->second.generation == handle.generation)
			{
				modelPathCache.erase(it);
				break;
			}
		}
		const size_t index = handle.index - 1;
		models[index].reset();
		// 0은 기본 생성 핸들의 generation이므로 재사용하지 않는다.
		if (++modelGenerations[index] == 0) ++modelGenerations[index];
	}

	const ModelData* ResourceManager::Get(ModelHandle handle) const
	{
		if (!handle.Valid() || handle.index > models.size())
			return nullptr;
		if (modelGenerations[handle.index - 1] != handle.generation)
			return nullptr;
		return models[handle.index - 1].get();
	}
}
