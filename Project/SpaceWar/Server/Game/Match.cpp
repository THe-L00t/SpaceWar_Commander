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

		// 네트워크로 온 입력 패킷을 시뮬레이션이 쓰는 형태로 되돌린다.
		// 클라는 float(-1~1) 를 int8(-100~100) 로 줄여 보낸다.
		Shared::MoveInput ToMoveInput(const Shared::PlayerInputPacket& in)
		{
			Shared::MoveInput m{};
			m.moveX = float(in.moveX) / 100.0f;
			m.moveZ = float(in.moveZ) / 100.0f;
			m.buttons = in.buttons;
			m.aimDir = Shared::Vec3d{ in.aimDir[0], in.aimDir[1], in.aimDir[2] };
			return m;
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
		//
		//   ★ 구면 위에 놓는다. 월드 원점이 행성 표면이므로
		//     접평면 좌표 (x, 0, z) 를 그대로 넘기면 SpawnMotion 이 구에 붙여준다.
		const uint32_t n = teamCount[team].fetch_add(1);
		const double sx = (double(n % 10) - 4.5) * 60.0;
		const double sz = (team == 0 ? -400.0 : 400.0)
			+ (team == 0 ? -1.0 : 1.0) * double(n / 10) * 40.0;

		Shared::SpawnMotion(p->motion, planet,
			Shared::Vec3d{ sx, 0.0, sz },
			Shared::Vec3d{ 0.0, 0.0, team == 0 ? 1.0 : -1.0 });

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

			// ★ 클라와 완전히 같은 함수다.
			//   같은 상태 + 같은 입력 + 같은 dt -> 같은 결과.
			//   이게 성립해야 클라 예측이 서버와 어긋나지 않는다.
			Shared::StepMotion(p.motion, ToMoveInput(in), planet, Shared::kTickSeconds);
		}

		lock.unlock();
		BroadcastSnapshot();
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

		// 시뮬레이션은 double 이지만 전송은 float 이다.
		// 월드 원점이 행성 표면이라 좌표가 작은 수여서 float 로 충분하다.
		auto put = [](float* dst, const Shared::Vec3d& v) {
			dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z);
		};

		for (auto& [sid, up] : players)
		{
			const MatchPlayer& p = *up;
			Shared::PlayerStateEntry e{};
			e.playerId = p.playerId;
			put(e.pos, p.motion.position);
			put(e.vel, p.motion.velocity);
			put(e.facing, p.motion.facing);
			e.altitude = float(p.motion.altitude);
			e.verticalSpeed = float(p.motion.verticalSpeed);
			e.grounded = p.motion.grounded ? 1 : 0;
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
					const float dx = entryCache[i].pos[0] - float(me.motion.position.x);
					const float dy = entryCache[i].pos[1] - float(me.motion.position.y);
					const float dz = entryCache[i].pos[2] - float(me.motion.position.z);
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
