#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>
#include "Shared/Protocol.h"
#include "../Net/Session.h"

// ============================================================
//  Match — 경기 하나 (50 대 50 = 100명)
//
//  ★ 스레드 규칙 — 이 파일의 핵심
//    이 경기의 게임 상태(players 안의 좌표·속도)는
//    ★ 틱 워커 스레드 한 개만 ★ 만진다.
//
//    IOCP 워커(여러 개)는 상태를 직접 고치지 않고
//    입력 큐에 밀어넣기만 한다. 큐만 짧게 잠근다.
//
//        IOCP 워커 N개  ──[입력 큐]──>  틱 워커 1개  ──[스냅샷]──> 전송
//              (락 아주 짧음)             (락 없음)
//
//    이렇게 하면 좌표를 건드리는 코드에 락이 아예 없어서
//    코어 하나가 경기 하나(100명)를 통째로 감당할 수 있다.
//    상태마다 락을 걸면 100명 x 30틱 = 초당 3000번 경합이 생겨 이 목표가 무너진다.
//
//  ★ 지금 이동 계산은 자리만 잡아둔 임시 코드다
//    2단계에서 클라의 PlayerController 계산부를 Shared/Sim/PlayerMotion 으로
//    옮기고 나면, StepMotion() 한 줄로 교체된다. (Simulate() 안 주석 참고)
// ============================================================

namespace swc {

	// 서버가 들고 있는 플레이어 한 명
	struct MatchPlayer
	{
		uint32_t playerId = 0;
		uint32_t sessionId = 0;
		uint8_t  team = 0;
		std::weak_ptr<Session> session;      // 끊긴 세션을 붙잡지 않도록 weak

		// ── 입력 (IOCP 워커가 넣고, 틱 워커가 뺀다) ──
		std::mutex inputMutex;
		std::deque<Shared::PlayerInputPacket> inputQueue;
		Shared::PlayerInputPacket lastInput{};   // 패킷이 안 왔을 때 재사용
		uint32_t ackTick = 0;                    // 마지막으로 처리한 입력의 tick

		// ── 상태 (틱 워커 전용. 락 없이 만진다) ──
		float pos[3]{ 0.0f, 0.0f, 0.0f };
		float vel[3]{ 0.0f, 0.0f, 0.0f };
		float facing[3]{ 0.0f, 0.0f, 1.0f };
		float altitude = 0.0f;
		float verticalSpeed = 0.0f;
		bool  grounded = true;

		// 통계
		std::atomic<uint32_t> receivedInputs{ 0 };
	};

	class Match
	{
	public:
		explicit Match(uint32_t id) : matchId(id) {}

		uint32_t Id() const { return matchId; }

		// ── IOCP 워커 스레드에서 호출 ──
		bool AddPlayer(const std::shared_ptr<Session>& s, uint32_t& outPlayerId, uint8_t& outTeam);
		void RemovePlayer(uint32_t sessionId);
		void EnqueueInput(uint32_t sessionId, const Shared::PlayerInputPacket& in);

		// ── 틱 워커 스레드에서 호출 ──
		void Tick();

		size_t PlayerCount() const;
		bool   IsFull() const;
		bool   IsEmpty() const;
		uint32_t CurrentTick() const { return tick.load(std::memory_order_relaxed); }
		uint64_t TotalInputs() const { return totalInputs.load(std::memory_order_relaxed); }

	private:
		void Simulate(MatchPlayer& p, const Shared::PlayerInputPacket& in, float dt);
		void BroadcastSnapshot();

		const uint32_t matchId;

		// 명단 자체의 변경(입장·퇴장)만 보호한다. 플레이어 내부 상태는 보호 대상이 아니다.
		mutable std::shared_mutex rosterMutex;
		std::unordered_map<uint32_t, std::unique_ptr<MatchPlayer>> players;   // key = sessionId

		std::atomic<uint32_t> tick{ 0 };
		std::atomic<uint32_t> nextPlayerId{ 1 };
		std::atomic<uint32_t> teamCount[2]{ {0}, {0} };
		std::atomic<uint64_t> totalInputs{ 0 };

		// 스냅샷 조립용 버퍼. 매 틱 새로 할당하지 않으려고 멤버로 들고 있다.
		std::vector<char> snapshotBuffer;
	};
}
