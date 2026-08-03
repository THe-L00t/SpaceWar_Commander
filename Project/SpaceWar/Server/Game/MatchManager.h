#pragma once
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include "Match.h"

// ============================================================
//  MatchManager — 경기 여러 개를 코어에 나눠 돌린다
//
//  ★ 목표 : 코어 1개당 동접 100명 = 경기 1개
//    50 대 50 이 100명이므로, 8코어면 경기 8개 = 동접 800명이 목표치다.
//
//  ★ 왜 "경기마다 스레드 1개" 가 아닌가
//    경기가 20개면 스레드 20개가 되고, 코어보다 많아지는 순간
//    문맥 전환 비용이 시뮬레이션 비용을 넘어선다.
//    그래서 ★ 틱 워커를 코어 수만큼만 ★ 두고, 경기를 나눠 맡긴다.
//
//  ★ 어떻게 나누나 — 고정 소유(static ownership)
//    경기를 만들 때 담당 워커를 정해서 Match 안에 박아둔다. 절대 바뀌지 않는다.
//    매번 큐에서 훔쳐가는 방식(work stealing)도 있지만,
//    고정이면 "한 경기는 늘 같은 스레드가 처리" 가 보장되고
//    그러면 경기 내부 상태에 락이 아예 필요 없어진다. (Match.h 주석 참고)
//
//    ※ "matches[k] 를 워커 (k % N)" 처럼 인덱스로 나누면 안 된다.
//      빈 경기를 목록에서 지우는 순간 인덱스가 밀려서
//      같은 경기를 두 워커가 동시에 Tick 할 수 있다.
//
//  ★ 틱 주기 맞추기
//    Sleep(33) 을 반복하면 오차가 누적돼 점점 느려진다.
//    "다음 틱의 절대 시각" 을 기준으로 자므로 오차가 쌓이지 않는다.
// ============================================================

namespace swc {

	class MatchManager
	{
	public:
		~MatchManager();

		// tickWorkers: 0 이면 코어 수로 잡는다
		void Start(int tickWorkers = 0);
		void Stop();

		// AOI 설정. 이후 만들어지는 경기에 적용된다.
		// (끄고 켜며 대역폭을 비교해 보라 — 이 구조에서 가장 큰 차이를 만드는 스위치다)
		void SetAoi(bool enabled, float radiusM) { aoiEnabled = enabled; aoiRadiusM = radiusM; }

		// 빈자리가 있는 경기를 찾고, 없으면 새로 연다.
		std::shared_ptr<Match> FindOrCreateMatch();

		std::shared_ptr<Match> FindByMatchId(uint32_t matchId) const;

		// 통계 출력용
		size_t MatchCount() const;
		size_t TotalPlayers() const;
		uint64_t TotalInputs() const;
		double  LastTickMs(size_t worker) const;
		size_t  WorkerCount() const { return workers.size(); }

	private:
		// workerCount 를 인자로 받는다. workers.size() 를 읽으면
		// Start() 가 스레드를 만드는 도중이라 값이 아직 안 찼을 수 있다.
		void TickLoop(size_t workerIndex, size_t workerCount);
		void ReapEmptyMatches();

		mutable std::shared_mutex matchMutex;
		std::vector<std::shared_ptr<Match>> matches;
		std::atomic<uint32_t> nextMatchId{ 1 };
		size_t workerCountCache = 1;        // 경기 생성 시 소유 워커를 정하는 데 쓴다

		bool  aoiEnabled = true;
		float aoiRadiusM = 150.0f;

		std::vector<std::thread> workers;
		std::atomic<bool> running{ false };

		// 워커별 마지막 틱 소요 시간(ms). 이게 33ms 를 넘으면 그 코어가 포화된 것이다.
		std::vector<std::atomic<double>> tickMs;
	};
}
