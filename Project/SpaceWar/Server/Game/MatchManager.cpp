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

		// atomic 은 복사·이동이 안 되므로 resize 로 제자리 생성한다
		tickMs = std::vector<std::atomic<double>>(tickWorkers);
		for (auto& v : tickMs) v.store(0.0);

		running.store(true);
		for (int i = 0; i < tickWorkers; ++i)
			workers.emplace_back([this, i] { TickLoop(size_t(i)); });

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

		auto m = std::make_shared<Match>(nextMatchId.fetch_add(1));
		matches.push_back(m);
		std::printf("[game] 경기 %u 생성 (현재 %zu개)\n", m->Id(), matches.size());
		return m;
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
	//  워커 i 는 matches[i], matches[i+N], matches[i+2N] ... 만 처리한다.
	//  (N = 워커 수). 한 경기는 언제나 같은 스레드가 맡는다.
	void MatchManager::TickLoop(size_t workerIndex)
	{
		const auto period = std::chrono::microseconds(1'000'000 / Shared::kTickRateHz);
		auto nextTick = clock_t_::now() + period;

		const size_t stride = workers.empty() ? 1 : workers.size();

		while (running.load(std::memory_order_relaxed))
		{
			const auto begin = clock_t_::now();

			// 내가 맡은 경기만 골라 담는다 (락은 여기서만, 아주 짧게)
			std::vector<std::shared_ptr<Match>> mine;
			{
				std::shared_lock lock(matchMutex);
				for (size_t k = workerIndex; k < matches.size(); k += stride)
					mine.push_back(matches[k]);
			}

			for (auto& m : mine)
				m->Tick();

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
