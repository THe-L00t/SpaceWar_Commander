#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>

// ============================================================
//  Client/Resource/LoadQueue.h — 「멀티스레딩 분류 명세서」 4장 파일 I/O · 6장 ⑥
//
//  로딩 전용 스레드 하나와, 그 스레드가 끝낸 일을 메인으로 돌려주는 완료 큐.
//
//      메인 ── Submit(work, finish) ──▶ 요청 큐 ──▶ [로딩 스레드] work()
//                                                         │
//      메인 ◀── PumpCompleted() 가 finish(ok) 를 부름 ◀── 완료 큐 ⑥
//
//  ★ work 는 로딩 스레드에서, finish 는 메인 스레드에서 돈다
//    work 는 파일 읽기·디코딩·메시 생성처럼 «RAM 까지» 만 한다(명세서 §4).
//    GPU 업로드와 Resource Manager 컨테이너 등록은 finish(메인)에서 한다.
//    로딩 스레드는 GameObject·Scene·Renderer 를 건드리지 않는다(9.2 «되부르기 금지»).
//  ★ 지금은 파싱·디코딩도 이 스레드가 한다
//    명세 5장은 파싱·디코딩을 워커 풀 잡으로 두지만 워커 풀(M5)이 아직 없다.
//    워커 풀이 생기면 work 를 잡으로 넘기고 이 스레드는 파일 읽기만 남긴다.
//  ★ 스레드를 detach 하지 않는다 (9.1). Stop() 이 대기 요청을 취소하고 join 한다.
// ============================================================

namespace swc {

	class LoadQueue
	{
	public:
		using Work = std::function<bool()>;          // 로딩 스레드. 실패면 false
		using Finish = std::function<void(bool)>;    // 메인 스레드. 인자 = work 성공 여부

		LoadQueue() = default;
		~LoadQueue();

		LoadQueue(const LoadQueue&) = delete;
		LoadQueue& operator=(const LoadQueue&) = delete;

		// 메인 스레드에서 부른다. 부른 스레드가 완료 큐의 주인(메인)이 된다.
		bool Start();
		// 대기 중인 요청은 버리고(취소), 진행 중인 work 하나는 끝날 때까지 기다린다.
		void Stop();

		// work 가 비어 있으면 로딩 스레드는 성공으로 바로 넘긴다(메인 전용 작업).
		void Submit(Work work, Finish finish);

		// 완료된 finish 를 최대 maxCount 개 실행한다. 메인 스레드 전용.
		// ★ 프레임당 업로드 예산 (명세 ⑥) — 남은 것은 다음 프레임으로 넘긴다.
		size_t PumpCompleted(size_t maxCount);

		bool Running() const { return worker.joinable(); }

	private:
		struct Request
		{
			Work   work;
			Finish finish;
		};

		struct Done
		{
			Finish finish;
			bool   ok = false;
		};

		void ThreadMain();

		std::thread             worker;
		std::thread::id         ownerThread;   // 완료 큐를 비우는 스레드(메인)

		std::mutex              mutex;
		std::condition_variable wake;
		std::deque<Request>     requests;
		std::deque<Done>        completed;
		bool                    stopping = false;
	};

} // namespace swc
