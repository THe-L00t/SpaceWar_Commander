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

} // namespace swc
