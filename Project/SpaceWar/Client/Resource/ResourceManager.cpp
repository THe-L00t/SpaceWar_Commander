#include "ResourceManager.h"
// ★ 하이트맵 로더는 Shared 로 옮겨졌다(09-18 `1621d64`).
// ★ 모델은 자체 파서(Shared::ModelReader)로 읽는다 — 외부 SDK 를 쓰지 않는다(2026-10-07).
//   파서가 Shared 에 있는 이유: 서버가 같은 코드로 충돌·높이를 읽는다(명세 §18 원칙 4).
#include "Shared/Terrain/HeightmapLoader.h"
#include "Shared/Model/ModelReader.h"
#include "ModelBuilder.h"
#include "TextureLoader.h"
#include <filesystem>
#include <utility>

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

	// ── 로딩 스레드 ────────────────────────────────────────────

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

	// ── 모델 ──────────────────────────────────────────────────

	std::wstring ResourceManager::ModelKey(const wchar_t* path, std::wstring& error)
	{
		if (!path || !*path)
		{
			error = L"모델 경로가 비어 있습니다.";
			return {};
		}

		std::error_code ec;
		const std::filesystem::path fullPath = std::filesystem::absolute(path, ec);
		if (ec)
		{
			error = L"모델 절대 경로를 만들 수 없습니다.";
			return {};
		}
		return fullPath.lexically_normal().wstring();
	}

	bool ResourceManager::ParseModelFile(const std::wstring& key, const ModelRequestOptions& extra,
		ParsedModelFile& out, std::wstring& error)
	{
		// ── 1) 포맷 중립 표현으로 읽는다 ────────────────
		//  클라는 충돌 데이터가 필요 없다. 렌더 삼각형을 한 번 더 복사하지 않게 끈다
		//  (서버는 geometryOnly = true 로 충돌만 읽는다).
		Shared::ReadOptions options;
		options.generateCollision = false;
		options.collectObject = extra.collectObject;   // 이름으로 고른 면을 같은 파싱에서 함께 모은다

		Shared::ModelSource source;
		if (!Shared::ModelReader().Load(key.c_str(), source, error, options))
			return false;

		// 같은 파싱 결과로 렌더 말고 다른 것도 만든다(예: 행성 접지면). 로딩 스레드다.
		if (extra.onParsed && !extra.onParsed(source, error))
			return false;
		// 모은 기하는 렌더 변환에 쓰지 않는다. 메모리를 일찍 돌려준다.
		source.collected = Shared::CollectedGeometry{};

		// ── 2) 메시·재질 ───────────────────────────────
		//  클립만 든 파일이면 메시가 없다 — 그것은 실패가 아니다.
		if (!source.meshes.empty())
		{
			out.model = std::make_unique<ModelData>();
			if (!BuildModelData(source, *out.model, error))
				return false;
		}

		// ── 3) 스켈레톤(본) ────────────────────────────
		if (!source.skeleton.Empty())
		{
			out.skeleton = std::make_unique<SkeletonResource>();
			BuildSkeleton(source, *out.skeleton);
		}

		// ── 4) 애니메이션 클립 ─────────────────────────
		if (!source.animations.empty())
			BuildAnimationClips(source, out.clips);

		return true;
	}

	ModelFileResources ResourceManager::RegisterModelFile(const std::wstring& key, ParsedModelFile&& parsed)
	{
		ModelFileResources result;

		if (parsed.model)
		{
			models.push_back(std::move(parsed.model));
			modelGenerations.push_back(1);
			result.model = { static_cast<uint32_t>(models.size()), 1 };
		}

		if (parsed.skeleton)
		{
			skeletons.push_back(std::move(parsed.skeleton));
			skeletonGenerations.push_back(1);
			result.skeleton = { static_cast<uint32_t>(skeletons.size()), 1 };
		}

		result.animations.reserve(parsed.clips.size());
		for (AnimationClipData& clip : parsed.clips)
		{
			animations.push_back(std::make_unique<AnimationClipData>(std::move(clip)));
			animationGenerations.push_back(1);
			result.animations.push_back({ static_cast<uint32_t>(animations.size()), 1 });
		}

		fileCache.emplace(key, result);
		return result;
	}

	ModelFileResources ResourceManager::LoadModelFile(const wchar_t* path)
	{
		lastError.clear();
		const std::wstring key = ModelKey(path, lastError);
		if (key.empty())
			return {};

		if (auto it = fileCache.find(key); it != fileCache.end())
			return it->second;                       // ★ 파일 하나는 한 번만 읽는다

		ParsedModelFile parsed;
		if (!ParseModelFile(key, ModelRequestOptions{}, parsed, lastError))
			return {};

		return RegisterModelFile(key, std::move(parsed));
	}

	void ResourceManager::RequestModelFile(LoadBatch& batch, const std::wstring& path, ModelReady onReady,
		ModelRequestOptions extra)
	{
		struct Job
		{
			std::wstring        key;
			ModelRequestOptions extra;
			ParsedModelFile     parsed;
			std::wstring        error;
		};
		std::shared_ptr<Job> job = std::make_shared<Job>();
		job->key = ModelKey(path.c_str(), job->error);
		job->extra = std::move(extra);

		// 키를 못 만들었거나 이미 읽은 파일이면 로딩 스레드에 일을 주지 않는다.
		LoadBatch::Work work;
		if (!job->key.empty() && fileCache.find(job->key) == fileCache.end())
		{
			work = [job]()
			{
				return ParseModelFile(job->key, job->extra, job->parsed, job->error);
			};
		}

		batch.Add(L"모델 " + FileName(path), std::move(work),
			[this, job, onReady = std::move(onReady)](bool ok)
			{
				ModelFileResources result;
				if (job->key.empty())
					lastError = job->error;
				else if (auto it = fileCache.find(job->key); it != fileCache.end())
					result = it->second;             // 그사이 같은 파일이 먼저 등록됐다
				else if (ok)
					result = RegisterModelFile(job->key, std::move(job->parsed));
				else
					lastError = job->error;

				if (onReady) return onReady(result);
				return result.model.Valid() || result.skeleton.Valid() || !result.animations.empty();
			});
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

	// ── 단독 텍스처 ───────────────────────────────────────────
	//  캐시 키에 srgb 를 붙인다. 같은 파일을 색/값 두 가지로 읽을 수 있기 때문이다.

	TextureDataHandle ResourceManager::LoadTexture(const wchar_t* path, bool srgb)
	{
		if (!path || !*path) return {};

		const std::wstring key = std::wstring(path) + (srgb ? L"|srgb" : L"|linear");
		if (auto it = texturePathCache.find(key); it != texturePathCache.end())
			return it->second;

		TextureData data;
		if (!LoadTextureImage(path, srgb, data, lastError))
			return {};

		return AddTexture(key, std::move(data));
	}

	void ResourceManager::RequestTexture(LoadBatch& batch, const std::wstring& path, bool srgb, TextureReady onReady)
	{
		struct Job
		{
			std::wstring path;
			std::wstring key;
			bool         srgb = false;
			TextureData  data;
			std::wstring error;
		};
		std::shared_ptr<Job> job = std::make_shared<Job>();
		job->path = path;
		job->key = path + (srgb ? L"|srgb" : L"|linear");
		job->srgb = srgb;

		LoadBatch::Work work;
		if (texturePathCache.find(job->key) == texturePathCache.end())
		{
			work = [job]()
			{
				return LoadTextureImage(job->path.c_str(), job->srgb, job->data, job->error);
			};
		}

		batch.Add(L"텍스처 " + FileName(path), std::move(work),
			[this, job, onReady = std::move(onReady)](bool ok)
			{
				TextureDataHandle handle;
				if (auto it = texturePathCache.find(job->key); it != texturePathCache.end())
					handle = it->second;
				else if (ok)
					handle = AddTexture(job->key, std::move(job->data));
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

	TextureDataHandle ResourceManager::AddTexture(const std::wstring& key, TextureData&& data)
	{
		textures.push_back(std::make_unique<TextureData>(std::move(data)));
		textureGenerations.push_back(1);

		const TextureDataHandle handle{ uint32_t(textures.size()), 1 };
		texturePathCache.emplace(key, handle);
		return handle;
	}
}
