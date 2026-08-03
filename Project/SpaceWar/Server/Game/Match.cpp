#include "Match.h"
#include "Shared/Units.h"
#include <cmath>
#include <cstring>
#include <chrono>

namespace swc {

	namespace {
		int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}
	}

	// 비어 있기 시작한 시각을 갱신한다. (rosterMutex 를 잡은 상태에서 호출)
	void Match::UpdateEmptyMark()
	{
		if (players.empty())
		{
			if (emptySinceMs.load(std::memory_order_relaxed) == 0)
				emptySinceMs.store(NowMs(), std::memory_order_relaxed);
		}
		else
		{
			emptySinceMs.store(0, std::memory_order_relaxed);
		}
	}

	// ── 입장 ────────────────────────────────────────────────
	bool Match::AddPlayer(const std::shared_ptr<Session>& s, uint32_t& outPlayerId, uint8_t& outTeam)
	{
		std::unique_lock lock(rosterMutex);
		if (players.size() >= Shared::kPlayersPerMatch) return false;

		// 인원이 적은 팀에 넣는다 (50 대 50 유지)
		const uint8_t team = (teamCount[0].load() <= teamCount[1].load()) ? 0 : 1;

		auto p = std::make_unique<MatchPlayer>();
		p->playerId = nextPlayerId.fetch_add(1);
		p->sessionId = s->Id();
		p->team = team;
		p->session = s;

		// 스폰 위치 — 50 대 50 전장 규모로 퍼뜨린다.
		//   가로 10명 x 60m = 540m, 세로 5줄 x 40m, 양 팀 간격 800m.
		//   뭉쳐서 스폰하면 전원이 서로의 AOI 안에 들어와 시야 필터가 무의미해진다.
		const uint32_t n = teamCount[team].fetch_add(1);
		p->pos[0] = (float(n % 10) - 4.5f) * 60.0f;
		p->pos[1] = 0.0f;
		p->pos[2] = (team == 0 ? -400.0f : 400.0f)
			+ (team == 0 ? -1.0f : 1.0f) * float(n / 10) * 40.0f;
		p->facing[2] = (team == 0 ? 1.0f : -1.0f);

		outPlayerId = p->playerId;
		outTeam = team;

		players.emplace(s->Id(), std::move(p));
		UpdateEmptyMark();
		return true;
	}

	void Match::RemovePlayer(uint32_t sessionId)
	{
		std::unique_lock lock(rosterMutex);
		auto it = players.find(sessionId);
		if (it == players.end()) return;

		const uint8_t team = it->second->team;
		const uint32_t leavingId = it->second->playerId;
		if (teamCount[team].load() > 0) teamCount[team].fetch_sub(1);
		players.erase(it);
		UpdateEmptyMark();

		// 남은 사람들에게 알린다
		Shared::PlayerLeavePacket pkt{};
		pkt.header.size = sizeof(pkt);
		pkt.header.type = Shared::PacketType::PlayerLeave;
		pkt.playerId = leavingId;

		for (auto& [sid, p] : players)
			if (auto sess = p->session.lock())
				sess->Send(&pkt, pkt.header.size);
	}

	// ── 입력 수신 (IOCP 워커) ───────────────────────────────
	//
	//  ★ 여기서 게임 상태를 고치지 않는다
	//    IOCP 워커는 여러 개다. 여기서 좌표를 만지면 락이 필요해지고,
	//    그러면 초당 3000번 경합이 생겨 코어당 100명이 안 나온다.
	//    큐에 넣기만 하고, 계산은 틱 워커 한 개가 몰아서 한다.
	void Match::EnqueueInput(uint32_t sessionId, const Shared::PlayerInputPacket& in)
	{
		std::shared_lock lock(rosterMutex);      // 명단만 읽는다 (여럿이 동시 통과)
		auto it = players.find(sessionId);
		if (it == players.end()) return;

		MatchPlayer& p = *it->second;
		{
			std::lock_guard<std::mutex> q(p.inputMutex);

			// 큐가 밀리면 렉이거나 조작이다. 최근 것만 남긴다.
			// (무한히 쌓이면 메모리도 늘고, 처리도 계속 과거를 재생하게 된다)
			if (p.inputQueue.size() >= 8) p.inputQueue.pop_front();
			p.inputQueue.push_back(in);
		}
		p.receivedInputs.fetch_add(1, std::memory_order_relaxed);
		totalInputs.fetch_add(1, std::memory_order_relaxed);
	}

	// ── 고정 틱 (틱 워커 스레드 1개) ────────────────────────
	//
	//  1/30 초마다 한 번. 이 안에서는 락이 거의 없다.
	void Match::Tick()
	{
		const uint32_t t = tick.fetch_add(1) + 1;

		std::shared_lock lock(rosterMutex);

		for (auto& [sid, up] : players)
		{
			MatchPlayer& p = *up;

			// 입력 하나 꺼내기
			Shared::PlayerInputPacket in{};
			{
				std::lock_guard<std::mutex> q(p.inputMutex);
				if (!p.inputQueue.empty())
				{
					in = p.inputQueue.front();
					p.inputQueue.pop_front();
					p.lastInput = in;
				}
				else
				{
					// ★ 패킷이 안 왔으면 직전 입력을 그대로 반복한다.
					//   여기서 "입력 없음"으로 처리하면 렉이 살짝만 나도
					//   캐릭터가 멈칫멈칫한다. 계속 달리던 사람은 계속 달리는 게 맞다.
					in = p.lastInput;
				}
			}
			p.ackTick = in.tick;

			Simulate(p, in, Shared::kTickSeconds);
		}

		lock.unlock();
		BroadcastSnapshot();
	}

	// ── 이동 계산 ───────────────────────────────────────────
	//
	//  ★★ 여기가 임시 코드다 ★★
	//
	//  지금은 평면 위에서 입력 방향으로 미는 최소 구현이다.
	//  이 테스트 브랜치의 목적은 "IOCP 배관이 도는가" 를 확인하는 것이라
	//  구면 이동·중력·지형은 일부러 넣지 않았다.
	//
	//  본 작업(2단계)에서 클라의 PlayerController::Update 계산부를
	//  Shared/Sim/PlayerMotion.cpp 로 옮기고 나면, 이 함수 전체가
	//
	//      Shared::StepMotion(p.state, ToMoveInput(in), planet, dt);
	//
	//  한 줄로 교체된다. 그때 비로소 클라와 서버가 같은 코드로 계산하게 되고
	//  예측·보정이 성립한다.
	void Match::Simulate(MatchPlayer& p, const Shared::PlayerInputPacket& in, float dt)
	{
		constexpr float kSpeed = 6.5f;      // m/s. 실제 값은 Shared/Units.h 의 kWalkSpeed
		constexpr float kAccel = 26.0f;

		// int8 (-100~100) 로 온 입력을 -1~1 로 되돌린다
		const float mx = float(in.moveX) / 100.0f;
		const float mz = float(in.moveZ) / 100.0f;

		// 카메라 전방(aimDir)을 기준으로 이동축을 만든다.
		// 서버엔 카메라가 없으므로 클라가 보낸 이 값이 유일한 근거다.
		float fx = in.aimDir[0], fz = in.aimDir[2];
		const float flen = std::sqrt(fx * fx + fz * fz);
		if (flen > 1e-6f) { fx /= flen; fz /= flen; }
		else { fx = 0.0f; fz = 1.0f; }

		const float rx = fz, rz = -fx;      // 오른쪽 = 전방을 y축 기준 90도 회전

		const float sprint = (in.buttons & Shared::Btn_Sprint) ? 1.7f : 1.0f;
		const float targetX = (fx * mz + rx * mx) * kSpeed * sprint;
		const float targetZ = (fz * mz + rz * mx) * kSpeed * sprint;

		// 목표 속도로 서서히 접근 (관성감)
		const float step = kAccel * dt;
		float dvx = targetX - p.vel[0];
		float dvz = targetZ - p.vel[2];
		const float dlen = std::sqrt(dvx * dvx + dvz * dvz);
		if (dlen > step && dlen > 0.0f) { dvx *= step / dlen; dvz *= step / dlen; }
		p.vel[0] += dvx;
		p.vel[2] += dvz;

		p.pos[0] += p.vel[0] * dt;
		p.pos[2] += p.vel[2] * dt;

		// 점프 / 중력 (평면 기준 임시)
		if (p.grounded && (in.buttons & Shared::Btn_Jump))
		{
			p.verticalSpeed = 7.0f;
			p.grounded = false;
		}
		p.verticalSpeed -= 18.0f * dt;
		p.altitude += p.verticalSpeed * dt;
		if (p.altitude <= 0.0f)
		{
			p.altitude = 0.0f;
			p.verticalSpeed = 0.0f;
			p.grounded = true;
		}
		p.pos[1] = p.altitude;

		// 이동 중이면 진행 방향을 본다
		const float sp = std::sqrt(p.vel[0] * p.vel[0] + p.vel[2] * p.vel[2]);
		if (sp > 0.15f) { p.facing[0] = p.vel[0] / sp; p.facing[2] = p.vel[2] / sp; }
	}

	// ── 스냅샷 브로드캐스트 ─────────────────────────────────
	//
	//  ★ 한 명씩 보내지 않고 전원 상태를 한 패킷에 몰아 담는다.
	//    100명이 서로의 상태를 30Hz 로 받을 때
	//      한 명씩 : 100 x 100 x 30 = 초당 30만 패킷
	//      묶어서  : 100 x 30       = 초당 3천 패킷
	//    패킷 하나당 커널 진입 비용이 붙으므로 개수가 곧 CPU 다.
	//
	//  ★ 나중에 AOI(주변만 보내기)를 끼울 자리
	//    지금은 전원을 담지만, 시야 밖을 걸러내면 count 만 줄어들 뿐
	//    구조는 그대로다.
	void Match::BroadcastSnapshot()
	{
		std::shared_lock lock(rosterMutex);
		if (players.empty()) return;

		const uint32_t nowTick = tick.load(std::memory_order_relaxed);

		// ── ① 전원 상태를 한 번만 만든다 ──
		//   수신자가 100명이어도 이 변환은 100번이 아니라 1번이다.
		entryCache.clear();
		receiverCache.clear();
		entryCache.reserve(players.size());
		receiverCache.reserve(players.size());

		for (auto& [sid, up] : players)
		{
			const MatchPlayer& p = *up;
			Shared::PlayerStateEntry e{};
			e.playerId = p.playerId;
			std::memcpy(e.pos, p.pos, sizeof(e.pos));
			std::memcpy(e.vel, p.vel, sizeof(e.vel));
			std::memcpy(e.facing, p.facing, sizeof(e.facing));
			e.altitude = p.altitude;
			e.verticalSpeed = p.verticalSpeed;
			e.grounded = p.grounded ? 1 : 0;
			entryCache.push_back(e);
			receiverCache.push_back(&p);
		}

		// 한 패킷에 담을 수 있는 최대 인원
		constexpr size_t kMaxEntries =
			(Shared::kMaxPacketSize - sizeof(Shared::WorldSnapshotHeader))
			/ sizeof(Shared::PlayerStateEntry);

		const float r2 = aoiRadiusM * aoiRadiusM;
		const size_t n = entryCache.size();

		snapshotBuffer.resize(sizeof(Shared::WorldSnapshotHeader)
			+ (n < kMaxEntries ? n : kMaxEntries) * sizeof(Shared::PlayerStateEntry));

		// ── ② 수신자마다 시야 안의 것만 담아 보낸다 ──
		for (size_t r = 0; r < n; ++r)
		{
			const MatchPlayer& me = *receiverCache[r];
			auto sess = me.session.lock();
			if (!sess) continue;

			auto* head = reinterpret_cast<Shared::WorldSnapshotHeader*>(snapshotBuffer.data());
			auto* out = reinterpret_cast<Shared::PlayerStateEntry*>(
				snapshotBuffer.data() + sizeof(Shared::WorldSnapshotHeader));

			uint16_t count = 0;
			for (size_t i = 0; i < n && count < kMaxEntries; ++i)
			{
				if (aoiEnabled && i != r)
				{
					// 시야 밖이면 건너뛴다. 자기 자신은 언제나 포함한다
					// (자기 위치가 빠지면 클라가 보정할 기준을 잃는다)
					const float dx = entryCache[i].pos[0] - me.pos[0];
					const float dy = entryCache[i].pos[1] - me.pos[1];
					const float dz = entryCache[i].pos[2] - me.pos[2];
					if (dx * dx + dy * dy + dz * dz > r2) continue;
				}
				out[count++] = entryCache[i];
			}

			const size_t total = sizeof(Shared::WorldSnapshotHeader)
				+ size_t(count) * sizeof(Shared::PlayerStateEntry);

			head->header.size = static_cast<uint16_t>(total);
			head->header.type = Shared::PacketType::PlayerState;
			head->tick = nowTick;
			head->ackTick = me.ackTick;     // ★ 이 값만 사람마다 다르다
			head->count = count;
			head->reserved = 0;

			sess->Send(snapshotBuffer.data(), head->header.size);
			snapshotBytes.fetch_add(total, std::memory_order_relaxed);
		}
	}

	size_t Match::PlayerCount() const
	{
		std::shared_lock lock(rosterMutex);
		return players.size();
	}

	bool Match::IsFull() const { return PlayerCount() >= Shared::kPlayersPerMatch; }
	bool Match::IsEmpty() const { return PlayerCount() == 0; }
}
