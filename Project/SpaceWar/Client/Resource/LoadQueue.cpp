#include "LoadQueue.h"

#include <windows.h>
#include <objbase.h>
#include <cassert>
#include <exception>

namespace swc {

	LoadQueue::~LoadQueue()
	{
		Stop();
	}

	bool LoadQueue::Start()
	{
		if (worker.joinable()) return true;

		ownerThread = std::this_thread::get_id();
		stopping = false;
		try
		{
			worker = std::thread(&LoadQueue::ThreadMain, this);
		}
		catch (const std::exception&)
		{
			return false;
		}
		return true;
	}

	void LoadQueue::Stop()
	{
		if (!worker.joinable()) return;

		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			requests.clear();      // 대기 중인 요청 취소 (9.1 종료 순서 4)
		}
		wake.notify_all();
		worker.join();

		// 끝난 일의 finish 는 부르지 않는다. 받을 쪽(씬·게임)이 이미 내려가는 중이다.
		std::lock_guard<std::mutex> lock(mutex);
		completed.clear();
	}

	void LoadQueue::Submit(Work work, Finish finish)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			requests.push_back(Request{ std::move(work), std::move(finish) });
		}
		wake.notify_one();
	}

	size_t LoadQueue::PumpCompleted(size_t maxCount)
	{
		// 9.2 소유 스레드 검사 — finish 는 메인에서만 돈다.
		assert(!worker.joinable() || std::this_thread::get_id() == ownerThread);

		size_t ran = 0;
		while (ran < maxCount)
		{
			Done done;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (completed.empty()) break;
				done = std::move(completed.front());
				completed.pop_front();
			}

			// 락 밖에서 부른다 — finish 가 다시 Submit 해도 교착되지 않는다.
			if (done.finish) done.finish(done.ok);
			++ran;
		}
		return ran;
	}

	void LoadQueue::ThreadMain()
	{
		// 9.7 — PIX·Visual Studio 에서 구분되게 이름을 붙인다.
		SetThreadDescription(GetCurrentThread(), L"SpaceWar File I/O");
		// 9.3 — 파일 I/O 는 우선순위를 낮게 둔다.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		// WIC(PNG 디코더)는 COM 객체다. COM 초기화는 스레드마다 따로 해야 한다.
		const bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

		for (;;)
		{
			Request request;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping || !requests.empty(); });
				if (stopping) break;
				request = std::move(requests.front());
				requests.pop_front();
			}

			// ★ 예외를 스레드 밖으로 던지지 않는다 (9.3). 실패는 결과로 넘긴다 (9.6).
			bool ok = true;
			if (request.work)
			{
				try
				{
					ok = request.work();
				}
				catch (...)
				{
					ok = false;
				}
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				completed.push_back(Done{ std::move(request.finish), ok });
			}
		}

		if (comReady) CoUninitialize();
	}

} // namespace swc
