#include "SceneManager.h"
#include "Client/Resource/ResourceManager.h"

namespace swc {

	SceneManager::SceneManager() = default;
	SceneManager::~SceneManager() = default;

	bool SceneManager::Initialize(GRenderer& renderer, ResourceManager& resources,
		const std::wstring& loadingBackgroundPath, const std::wstring& loadingSpinnerPath,
		std::wstring& error)
	{
		return loading.Initialize(renderer, resources,
			loadingBackgroundPath, loadingSpinnerPath, error);
	}

	bool SceneManager::ChangeScene(SceneId next, std::unique_ptr<LoadBatch> nextBatch, ResourceManager& resources)
	{
		// ★ 앞 묶음이 아직 돌고 있으면 버리지 않는다
		//   큐에 남은 finish 가 그 묶음을 가리킨다. 지우면 끝난 뒤 빈 주소를 부른다.
		if (batch && !batch->Done())
			return false;

		loadFailed = false;
		loadError.clear();

		if (!nextBatch)
		{
			batch.reset();
			Enter(next);
			return true;
		}

		batch = std::move(nextBatch);
		batch->Start(resources.Loader());
		if (batch->Done())
		{
			batch.reset();
			Enter(next);
			return true;
		}

		pending = next;
		loading.SetProgress(0.0f);
		Enter(SceneId::Loading);
		return true;
	}

	void SceneManager::Update(float dt, ResourceManager& resources)
	{
		if (active == SceneId::Loading)
		{
			if (batch)
			{
				batch->Update(resources.Loader());
				loading.SetProgress(batch->Progress());

				if (batch->Done())
				{
					if (batch->Failed())
					{
						loadFailed = true;
						loadError = batch->Error() + L" 실패: " +
							(batch->FailureDetail().empty() ? resources.LastError() : batch->FailureDetail());
					}
					else
					{
						const SceneId next = pending;
						pending = SceneId::None;
						batch.reset();
						Enter(next);
						return;
					}
				}
			}
			loading.Update(dt);
		}
	}

	SceneId SceneManager::TakeEntered()
	{
		const SceneId id = entered;
		entered = SceneId::None;
		return id;
	}

	void SceneManager::ExtractUI(std::vector<UISprite>& out) const
	{
		if (active == SceneId::Loading)
			loading.GetUI().Extract(out);
	}

	void SceneManager::Enter(SceneId id)
	{
		active = id;
		entered = id;
	}

	// ── 예시 행성 남기기 (2026-11-17) — SceneManager.h 설명 참고 ──
	void SceneManager::NoteDebugPlanetEntered(size_t index)
	{
		if (index < debugPlanetEnteredAt.size())
			debugPlanetEnteredAt[index] = ++debugPlanetEnterCount;
	}

	size_t SceneManager::DebugPlanetToRelease(size_t next, size_t keep) const
	{
		if (next >= debugPlanets.size() || debugPlanets[next].Loaded())
			return PlanetDebugScene::kCount;   // 이미 올라간 행성으로 가면 아무것도 내리지 않는다
		size_t loadedCount = 0;
		size_t oldest = PlanetDebugScene::kCount;
		for (size_t i = 0; i < debugPlanets.size(); ++i)
		{
			if (!debugPlanets[i].Loaded()) continue;
			++loadedCount;
			if (i == keep) continue;
			if (oldest == PlanetDebugScene::kCount || debugPlanetEnteredAt[i] < debugPlanetEnteredAt[oldest])
				oldest = i;
		}
		// next 가 들어오면 kKeptDebugPlanets + 1 개가 된다 → 하나를 먼저 내린다(읽기 전에 내려 GPU 메모리가 셋으로 겹치지 않게).
		return loadedCount >= kKeptDebugPlanets ? oldest : PlanetDebugScene::kCount;
	}

} // namespace swc
