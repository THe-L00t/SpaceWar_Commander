#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include "Client/Resource/LoadQueue.h"

// ============================================================
//  Client/Resource/LoadBatch.h — 로드 묶음 (단계 순서 + 진행률)
//
//  한 번의 씬 전환이나 리소스 묶음을 «단계» 로 나눠 LoadQueue 에 흘려보낸다.
//  같은 단계의 작업은 함께 제출되고, 한 단계가 메인에서 모두 끝나야 다음 단계를 낸다.
//      단계 0: 하이트맵 디코딩 ─▶ (메인) 지형 설정
//      단계 1: 지형을 읽어 지면 메시 생성 ─▶ (메인) GPU 업로드
//  뒤 단계의 work 가 앞 단계 finish 의 결과를 읽어도 되는 이유가 이 순서다.
//
//  ★ 메인 스레드 소유. Update 는 매 프레임 메인에서 부른다.
//  ★ SceneManager 가 이 묶음이 끝날 때까지 로딩 씬을 보여 준다.
// ============================================================

namespace swc {

	class LoadBatch
	{
	public:
		using Work = LoadQueue::Work;                 // 로딩 스레드. 실패면 false
		using Finish = std::function<bool(bool)>;     // 메인 스레드. 인자 = work 성공. 묶음을 실패로 끝내려면 false

		explicit LoadBatch(std::wstring name = L"");

		// 이후 Add 하는 작업은 새 단계에 들어간다. 첫 Add 앞에서는 부를 필요 없다.
		void NextStage();
		void Add(std::wstring label, Work work, Finish finish);

		// 메인 스레드. Start 는 한 번, Update 는 매 프레임.
		void Start(LoadQueue& queue);
		void Update(LoadQueue& queue);

		bool Started() const { return started; }
		// ★ 실패해도 이미 낸 작업이 다 돌아올 때까지는 Done 이 아니다.
		//   큐에 남은 finish 가 이 묶음을 가리키므로, 그 전에 묶음을 지우면 안 된다.
		bool Done() const { return started && pendingInStage == 0 && stage >= stages.size(); }
		bool Failed() const { return failed; }

		size_t TaskCount() const { return total; }
		size_t FinishedCount() const { return finished; }
		float Progress() const { return total == 0 ? 1.0f : float(finished) / float(total); }

		const std::wstring& Name() const { return name; }
		const std::wstring& CurrentLabel() const { return currentLabel; }
		const std::wstring& Error() const { return error; }

	private:
		struct Task
		{
			std::wstring label;
			Work         work;
			Finish       finish;
		};

		void SubmitStage(LoadQueue& queue);
		void OnTaskFinished(const std::wstring& label, bool ok);

		std::wstring                    name;
		std::vector<std::vector<Task>>  stages;
		size_t                          stage = 0;            // 진행 중인 단계
		size_t                          pendingInStage = 0;   // 그 단계에서 아직 안 끝난 수
		bool                            stageSubmitted = false;

		size_t                          total = 0;
		size_t                          finished = 0;
		bool                            started = false;
		bool                            failed = false;
		std::wstring                    currentLabel;
		std::wstring                    error;
	};

} // namespace swc
