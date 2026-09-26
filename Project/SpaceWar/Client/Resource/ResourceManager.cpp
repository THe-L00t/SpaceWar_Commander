#include "ResourceManager.h"
#include "HeightmapLoader.h"
#include "FbxModelLoader.h"
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
		if (!LoadHeightmapPng(path, data, lastError))
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
		FbxModelLoader loader;
		if (!loader.Load(key.c_str(), *data, lastError))
			return {};

		models.push_back(std::move(data));
		modelGenerations.push_back(1);
		const ModelHandle handle{ static_cast<uint32_t>(models.size()), 1 };
		modelPathCache.emplace(key, handle);
		return handle;
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
