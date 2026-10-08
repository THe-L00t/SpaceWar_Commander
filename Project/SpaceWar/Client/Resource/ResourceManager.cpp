#include "ResourceManager.h"
#include "Shared/Terrain/HeightmapLoader.h"
#include "Client/Resource/TextureLoader.h"

namespace {

	// 로딩 화면에 띄울 짧은 이름 — 경로 끝의 파일 이름만.
	std::wstring FileName(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	}

}

namespace swc {

	ResourceManager::~ResourceManager()
	{
		StopLoader();
	}

	bool ResourceManager::StartLoader()
	{
		return loader.Start();
	}

	void ResourceManager::StopLoader()
	{
		loader.Stop();
	}

	size_t ResourceManager::PumpLoaded(size_t maxCount)
	{
		return loader.PumpCompleted(maxCount);
	}

	// ── 하이트맵 ───────────────────────────────────────────────

	HeightmapHandle ResourceManager::LoadHeightmap(const wchar_t* path)
	{
		if (!path || !*path) return {};

		const std::wstring key = path;
		if (auto it = pathCache.find(key); it != pathCache.end())
			return it->second;                       // ★ 중복 로드 방지

		Shared::HeightmapData data;
		if (!Shared::LoadHeightmapPng(path, data, lastError))
			return {};

		return AddHeightmap(key, std::move(data));
	}

	void ResourceManager::RequestHeightmap(LoadBatch& batch, const std::wstring& path, HeightmapReady onReady)
	{
		// 로딩 스레드와 메인이 함께 보는 상자. work 가 채우고, finish 가 꺼낸다.
		struct Job
		{
			std::wstring          path;
			Shared::HeightmapData data;
			std::wstring          error;
		};
		std::shared_ptr<Job> job = std::make_shared<Job>();
		job->path = path;

		// 이미 읽은 경로면 로딩 스레드에 일을 주지 않는다.
		LoadBatch::Work work;
		if (pathCache.find(path) == pathCache.end())
		{
			work = [job]()
			{
				return Shared::LoadHeightmapPng(job->path.c_str(), job->data, job->error);
			};
		}

		batch.Add(L"하이트맵 " + FileName(path), std::move(work),
			[this, job, onReady = std::move(onReady)](bool ok)
			{
				HeightmapHandle handle;
				if (auto it = pathCache.find(job->path); it != pathCache.end())
					handle = it->second;             // 그사이 같은 경로가 먼저 등록됐다
				else if (ok)
					handle = AddHeightmap(job->path, std::move(job->data));
				else
					lastError = job->error;

				return onReady ? onReady(handle) : handle.Valid();
			});
	}

	const Shared::HeightmapData* ResourceManager::Get(HeightmapHandle handle) const
	{
		if (!handle.Valid() || handle.index > heightmaps.size())
			return nullptr;
		if (generations[handle.index - 1] != handle.generation)
			return nullptr;                          // 이미 해제된 참조
		return heightmaps[handle.index - 1].get();
	}

	HeightmapHandle ResourceManager::AddHeightmap(const std::wstring& path, Shared::HeightmapData&& data)
	{
		heightmaps.push_back(std::make_unique<Shared::HeightmapData>(std::move(data)));
		generations.push_back(1);

		// index 는 1부터 — 0을 무효로 예약
		const HeightmapHandle handle{ uint32_t(heightmaps.size()), 1 };
		pathCache.emplace(path, handle);
		return handle;
	}

	// ── 텍스처 ────────────────────────────────────────────────

	TextureDataHandle ResourceManager::LoadTexture(const wchar_t* path)
	{
		if (!path || !*path) return {};

		const std::wstring key = path;
		if (auto it = texturePathCache.find(key); it != texturePathCache.end())
			return it->second;

		TextureData data;
		if (!LoadTextureFile(path, data, lastError))
			return {};

		return AddTexture(key, std::move(data));
	}

	void ResourceManager::RequestTexture(LoadBatch& batch, const std::wstring& path, TextureReady onReady)
	{
		struct Job
		{
			std::wstring path;
			TextureData  data;
			std::wstring error;
		};
		std::shared_ptr<Job> job = std::make_shared<Job>();
		job->path = path;

		LoadBatch::Work work;
		if (texturePathCache.find(path) == texturePathCache.end())
		{
			work = [job]()
			{
				return LoadTextureFile(job->path.c_str(), job->data, job->error);
			};
		}

		batch.Add(L"텍스처 " + FileName(path), std::move(work),
			[this, job, onReady = std::move(onReady)](bool ok)
			{
				TextureDataHandle handle;
				if (auto it = texturePathCache.find(job->path); it != texturePathCache.end())
					handle = it->second;
				else if (ok)
					handle = AddTexture(job->path, std::move(job->data));
				else
					lastError = job->error;

				return onReady ? onReady(handle) : handle.Valid();
			});
	}

	const TextureData* ResourceManager::Get(TextureDataHandle handle) const
	{
		if (!handle.Valid() || handle.index > textures.size())
			return nullptr;
		if (textureGenerations[handle.index - 1] != handle.generation)
			return nullptr;
		return textures[handle.index - 1].get();
	}

	void ResourceManager::Release(TextureDataHandle handle)
	{
		if (!Get(handle)) return;

		const uint32_t slot = handle.index - 1;
		textures[slot].reset();
		++textureGenerations[slot];

		// 같은 경로를 다시 요청하면 파일을 다시 읽어야 한다. 캐시에서 지운다.
		for (auto it = texturePathCache.begin(); it != texturePathCache.end(); ++it)
		{
			if (it->second.index == handle.index)
			{
				texturePathCache.erase(it);
				break;
			}
		}
	}

	TextureDataHandle ResourceManager::AddTexture(const std::wstring& path, TextureData&& data)
	{
		textures.push_back(std::make_unique<TextureData>(std::move(data)));
		textureGenerations.push_back(1);

		const TextureDataHandle handle{ uint32_t(textures.size()), 1 };
		texturePathCache.emplace(path, handle);
		return handle;
	}
}
