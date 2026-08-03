#include "MatchManager.h"
#include <chrono>
#include <cstdio>

namespace swc {

	using clock_t_ = std::chrono::steady_clock;

	MatchManager::~MatchManager()
	{
		Stop();
	}

	void MatchManager::Start(int tickWorkers)
	{
		if (tickWorkers <= 0)
			tickWorkers = static_cast<int>(std::thread::hardware_concurrency());
		if (tickWorkers <= 0) tickWorkers = 2;

		// atomic 은 복사·이동이 안 되므로 통째로 새로 만든다
		tickMs = std::vector<std::atomic<double>>(tickWorkers);
		for (auto& v : tickMs) v.store(0.0);

		workerCountCache = size_t(tickWorkers);

		running.store(true);
		for (int i = 0; i < tickWorkers; ++i)
			workers.emplace_back([this, i, tickWorkers] {
				TickLoop(size_t(i), size_t(tickWorkers));
			});

		std::printf("[game] 틱 워커 %d개 시작 (%u Hz, 코어당 경기 1개 = %u명 목표)\n",
			tickWorkers, Shared::kTickRateHz, Shared::kPlayersPerMatch);
	}

	void MatchManager::Stop()
	{
		if (!running.exchange(false)) return;
		for (auto& t : workers) if (t.joinable()) t.join();
		workers.clear();
	}

	// ── 매칭 ────────────────────────────────────────────────
	std::shared_ptr<Match> MatchManager::FindOrCreateMatch()
	{
		{
			std::shared_lock lock(matchMutex);
			for (auto& m : matches)
				if (!m->IsFull()) return m;
		}

		std::unique_lock lock(matchMutex);
		// 락을 다시 잡는 사이에 누가 만들었을 수 있으므로 한 번 더 본다
		for (auto& m : matches)
			if (!m->IsFull()) return m;

		// ★ 담당 워커를 여기서 정해서 Match 안에 박아둔다. 이후 절대 안 바뀐다.
		//   가장 한가한 워커에게 준다 (= 맡은 경기가 가장 적은 워커)
		std::vector<size_t> load(workerCountCache, 0);
		for (auto& m : matches)
			if (m->OwnerWorker() < load.size()) ++load[m->OwnerWorker()];

		size_t owner = 0;
		for (size_t i = 1; i < load.size(); ++i)
			if (load[i] < load[owner]) owner = i;

		auto m = std::make_shared<Match>(nextMatchId.fetch_add(1), owner);
		m->SetAoiEnabled(aoiEnabled);
		m->SetAoiRadius(aoiRadiusM);
		m->World().terrain = terrain;   // ★ 스폰 전에 붙여야 착지 높이가 맞는다
		matches.push_back(m);
		std::printf("[game] 경기 %u 생성 (담당 워커 %zu, 현재 %zu개)\n",
			m->Id(), owner, matches.size());
		return m;
	}

	// ── 빈 경기 정리 ────────────────────────────────────────
	//
	//  경기가 계속 쌓이면 틱 루프가 빈 경기를 순회하느라 낭비한다.
	//  단, 만들자마자 지우면 안 된다 — 접속이 오는 중일 수 있다.
	//  그래서 "10초 넘게 비어 있던" 경기만 지운다.
	//
	//  ※ 지워도 안전한 이유 : 담당 워커가 Match 안에 값으로 박혀 있어
	//    목록에서 빠져도 다른 경기의 담당이 바뀌지 않는다.
	void MatchManager::ReapEmptyMatches()
	{
		using namespace std::chrono;
		const int64_t now =
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

		std::unique_lock lock(matchMutex);
		for (size_t i = matches.size(); i-- > 0; )
		{
			const int64_t since = matches[i]->EmptySinceMs();
			if (since != 0 && now - since > 10000)
			{
				std::printf("[game] 경기 %u 종료 (빈 상태 유지)\n", matches[i]->Id());
				matches.erase(matches.begin() + i);
			}
		}
	}

	std::shared_ptr<Match> MatchManager::FindByMatchId(uint32_t matchId) const
	{
		std::shared_lock lock(matchMutex);
		for (auto& m : matches)
			if (m->Id() == matchId) return m;
		return nullptr;
	}

	// ── 틱 루프 ─────────────────────────────────────────────
	//
	//  워커 i 는 OwnerWorker() == i 인 경기만 처리한다.
	//  담당이 Match 안에 값으로 박혀 있으므로, 목록에서 경기가 빠져도
	//  다른 경기의 담당이 바뀌지 않는다. (= 두 워커가 같은 경기를 만질 일이 없다)
	void MatchManager::TickLoop(size_t workerIndex, size_t /*workerCount*/)
	{
		const auto period = std::chrono::microseconds(1'000'000 / Shared::kTickRateHz);
		auto nextTick = clock_t_::now() + period;

		std::vector<std::shared_ptr<Match>> mine;   // 매 틱 재할당하지 않으려고 밖에 둔다
		int reapCounter = 0;

		while (running.load(std::memory_order_relaxed))
		{
			const auto begin = clock_t_::now();

			// 내가 맡은 경기만 골라 담는다 (락은 여기서만, 아주 짧게)
			mine.clear();
			{
				std::shared_lock lock(matchMutex);
				for (auto& m : matches)
					if (m->OwnerWorker() == workerIndex) mine.push_back(m);
			}

			for (auto& m : mine)
				m->Tick();

			// 청소는 워커 0 이 대표로, 5초에 한 번만 한다
			if (workerIndex == 0 && ++reapCounter >= int(Shared::kTickRateHz) * 5)
			{
				reapCounter = 0;
				ReapEmptyMatches();
			}

			const double ms = std::chrono::duration<double, std::milli>(
				clock_t_::now() - begin).count();
			tickMs[workerIndex].store(ms, std::memory_order_relaxed);

			// ★ 절대 시각 기준으로 잔다 — 오차가 누적되지 않는다
			//   sleep_for(33ms) 를 반복하면 "처리 시간 + 33ms" 가 되어 점점 밀린다.
			nextTick += period;
			const auto now = clock_t_::now();
			if (nextTick > now)
				std::this_thread::sleep_until(nextTick);
			else
				nextTick = now;      // 이미 늦었다 = 이 코어가 포화됐다. 따라잡지 않고 리셋
		}
	}

	size_t MatchManager::MatchCount() const
	{
		std::shared_lock lock(matchMutex);
		return matches.size();
	}

	size_t MatchManager::TotalPlayers() const
	{
		std::shared_lock lock(matchMutex);
		size_t n = 0;
		for (auto& m : matches) n += m->PlayerCount();
		return n;
	}

	uint64_t MatchManager::TotalInputs() const
	{
		std::shared_lock lock(matchMutex);
		uint64_t n = 0;
		for (auto& m : matches) n += m->TotalInputs();
		return n;
	}

	double MatchManager::LastTickMs(size_t worker) const
	{
		return worker < tickMs.size() ? tickMs[worker].load(std::memory_order_relaxed) : 0.0;
	}
}
