#include "ResourceManager.h"
// ★ 하이트맵 로더는 Shared 로 옮겨졌다(09-18 `1621d64`).
// ★ 모델은 자체 파서(Shared::ModelReader)로 읽는다 — 외부 SDK 를 쓰지 않는다(2026-10-07).
//   파서가 Shared 에 있는 이유: 서버가 같은 코드로 충돌·높이를 읽는다(명세 §18 원칙 4).
#include "Shared/Terrain/HeightmapLoader.h"
#include "Shared/Model/ModelReader.h"
#include "ModelBuilder.h"
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

	ModelFileResources ResourceManager::LoadModelFile(const wchar_t* path)
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
		if (auto it = fileCache.find(key); it != fileCache.end())
			return it->second;                       // ★ 파일 하나는 한 번만 읽는다

		// ── 1) 포맷 중립 표현으로 읽는다 ────────────────
		//  클라는 충돌 데이터가 필요 없다. 렌더 삼각형을 한 번 더 복사하지 않게 끈다
		//  (서버는 geometryOnly = true 로 충돌만 읽는다).
		Shared::ReadOptions options;
		options.generateCollision = false;

		Shared::ModelSource source;
		if (!Shared::ModelReader().Load(key.c_str(), source, lastError, options))
			return {};

		ModelFileResources result;

		// ── 2) 메시·재질 ───────────────────────────────
		//  클립만 든 파일이면 메시가 없다 — 그것은 실패가 아니다.
		if (!source.meshes.empty())
		{
			auto data = std::make_unique<ModelData>();
			if (!BuildModelData(source, *data, lastError))
				return {};

			models.push_back(std::move(data));
			modelGenerations.push_back(1);
			result.model = { static_cast<uint32_t>(models.size()), 1 };
		}

		// ── 3) 스켈레톤(본) ────────────────────────────
		if (!source.skeleton.Empty())
		{
			auto skeleton = std::make_unique<SkeletonResource>();
			BuildSkeleton(source, *skeleton);

			skeletons.push_back(std::move(skeleton));
			skeletonGenerations.push_back(1);
			result.skeleton = { static_cast<uint32_t>(skeletons.size()), 1 };
		}

		// ── 4) 애니메이션 클립 ─────────────────────────
		if (!source.animations.empty())
		{
			std::vector<AnimationClipData> clips;
			BuildAnimationClips(source, clips);
			result.animations.reserve(clips.size());

			for (AnimationClipData& clip : clips)
			{
				animations.push_back(std::make_unique<AnimationClipData>(std::move(clip)));
				animationGenerations.push_back(1);
				result.animations.push_back({ static_cast<uint32_t>(animations.size()), 1 });
			}
		}

		fileCache.emplace(key, result);
		return result;
	}

	ModelHandle ResourceManager::LoadModel(const wchar_t* path)
	{
		const ModelFileResources resources = LoadModelFile(path);
		if (!resources.model.Valid() && lastError.empty())
			lastError = L"이 파일에는 표시할 메시가 없습니다(애니메이션 전용 파일입니다).";
		return resources.model;
	}

	void ResourceManager::ReleaseModel(ModelHandle handle)
	{
		if (!handle.Valid() || handle.index > models.size()) return;
		const uint32_t slot = handle.index - 1;
		if (modelGenerations[slot] != handle.generation) return;   // 이미 해제된 핸들

		// CPU 사본을 버리고 세대를 올린다 → 남아 있는 핸들의 Get() 이 nullptr 가 된다.
		models[slot].reset();
		++modelGenerations[slot];

		// 같은 경로를 다시 요청하면 파일을 다시 읽어야 한다. 캐시에서 지운다.
		// ★ 스켈레톤·클립 핸들은 손대지 않는다 — Animator 가 이미 들고 돌리는 중이다.
		for (auto it = fileCache.begin(); it != fileCache.end(); ++it)
		{
			if (it->second.model.index == handle.index)
			{
				fileCache.erase(it);
				break;
			}
		}
	}

	const ModelData* ResourceManager::Get(ModelHandle handle) const
	{
		if (!handle.Valid() || handle.index > models.size())
			return nullptr;
		if (modelGenerations[handle.index - 1] != handle.generation)
			return nullptr;
		return models[handle.index - 1].get();
	}

	const SkeletonResource* ResourceManager::Get(SkeletonHandle handle) const
	{
		if (!handle.Valid() || handle.index > skeletons.size())
			return nullptr;
		if (skeletonGenerations[handle.index - 1] != handle.generation)
			return nullptr;
		return skeletons[handle.index - 1].get();
	}

	const AnimationClipData* ResourceManager::Get(AnimationHandle handle) const
	{
		if (!handle.Valid() || handle.index > animations.size())
			return nullptr;
		if (animationGenerations[handle.index - 1] != handle.generation)
			return nullptr;
		return animations[handle.index - 1].get();
	}

	AnimationHandle ResourceManager::FindAnimation(const ModelFileResources& resources,
		const std::string& name) const
	{
		for (AnimationHandle handle : resources.animations)
		{
			const AnimationClipData* clip = Get(handle);
			if (clip && clip->name == name) return handle;
		}
		return {};
	}
}
