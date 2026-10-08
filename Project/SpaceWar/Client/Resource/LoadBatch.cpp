#include "LoadBatch.h"

namespace swc {

	LoadBatch::LoadBatch(std::wstring batchName)
		: name(std::move(batchName))
	{
		stages.emplace_back();
	}

	void LoadBatch::NextStage()
	{
		if (!stages.back().empty())
			stages.emplace_back();
	}

	void LoadBatch::Add(std::wstring label, Work work, Finish finish)
	{
		stages.back().push_back(Task{ std::move(label), std::move(work), std::move(finish) });
		++total;
	}

	void LoadBatch::Start(LoadQueue& queue)
	{
		if (started) return;
		started = true;

		// 빈 단계(마지막 NextStage 뒤에 Add 가 없던 것)는 건너뛴다.
		while (!stages.empty() && stages.back().empty())
			stages.pop_back();

		stage = 0;
		SubmitStage(queue);
	}

	void LoadBatch::Update(LoadQueue& queue)
	{
		if (!started || Done()) return;

		// 단계가 메인에서 모두 끝났으면 다음 단계를 낸다. 실패했으면 더 내지 않는다.
		if (stageSubmitted && pendingInStage == 0)
		{
			stageSubmitted = false;
			if (failed)
			{
				stage = stages.size();
				return;
			}
			++stage;
			SubmitStage(queue);
		}
	}

	void LoadBatch::SubmitStage(LoadQueue& queue)
	{
		if (stage >= stages.size()) return;

		std::vector<Task>& tasks = stages[stage];
		pendingInStage = tasks.size();
		stageSubmitted = true;

		for (Task& task : tasks)
		{
			// finish 는 메인에서 PumpCompleted 가 부른다. 묶음은 SceneManager 가 끝까지 들고 있다.
			Finish finish = std::move(task.finish);
			std::wstring label = task.label;
			queue.Submit(std::move(task.work),
				[this, finish = std::move(finish), label = std::move(label)](bool workOk)
				{
					bool ok = workOk;
					if (finish) ok = finish(workOk);
					else if (!workOk) ok = false;
					OnTaskFinished(label, ok);
				});
		}
	}

	void LoadBatch::OnTaskFinished(const std::wstring& label, bool ok)
	{
		++finished;
		if (pendingInStage > 0) --pendingInStage;
		currentLabel = label;

		if (!ok && !failed)
		{
			failed = true;
			error = label;
		}
	}

} // namespace swc
