#pragma once
#include <winsock2.h>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include "Shared/Protocol.h"
#include "../SimTypes.h"

// ============================================================
//  NetClient — 클라이언트 쪽 IOCP
//
//  ★ 클라가 소켓 하나뿐인데 왜 IOCP 인가
//    성능 때문이 아니다. "게임 루프를 절대 멈추지 않기 위해서"다.
//
//    blocking recv 를 렌더 루프에서 부르면 패킷이 안 올 때 화면이 멈춘다.
//    별도 스레드에서 blocking recv 를 돌리는 방법도 있지만,
//    그러면 종료할 때 그 스레드가 recv 에 박혀서 안 빠져나온다.
//
//    IOCP 는 "받으면 알려줘" 방식이라
//      - 게임 루프는 절대 안 막히고
//      - 종료 신호(PostQueuedCompletionStatus)로 즉시 깨울 수 있다
//    서버와 같은 구조라 한 번 이해하면 양쪽 다 읽힌다는 이점도 있다.
//
//  ★ 스레드 경계
//      수신 스레드 : 소켓에서 읽어 스냅샷을 snapshot 에 넣는다 (뮤텍스)
//      게임 루프   : 매 프레임 스냅샷을 꺼내 간다 (뮤텍스)
//    렌더 스레드가 소켓을 직접 만지는 일은 없다.
// ============================================================

namespace swc {

	// 서버가 알려준 다른 플레이어 한 명
	struct RemotePlayer
	{
		uint32_t playerId = 0;
		Vec3d position{ 0.0, 0.0, 0.0 };
		Vec3d velocity{ 0.0, 0.0, 0.0 };
		Vec3d facing{ 0.0, 0.0, 1.0 };
		double altitude = 0.0;
		bool   grounded = true;
		uint32_t lastSeenTick = 0;     // 이 틱 이후로 안 보이면 시야를 벗어난 것
	};

	// 한 틱분 서버 권위 상태
	struct ServerSnapshot
	{
		uint32_t tick = 0;
		uint32_t ackTick = 0;                  // 내 입력을 어디까지 반영했는가
		bool     hasSelf = false;
		RemotePlayer self;
		std::vector<RemotePlayer> others;
	};

	class NetClient
	{
	public:
		~NetClient();

		bool Connect(const char* host, uint16_t port, std::wstring& error);
		void Disconnect();

		bool Connected() const { return connected.load(std::memory_order_acquire); }
		uint32_t MyPlayerId() const { return myPlayerId.load(std::memory_order_relaxed); }
		uint32_t MyMatchId() const { return myMatchId.load(std::memory_order_relaxed); }
		uint8_t  MyTeam() const { return myTeam.load(std::memory_order_relaxed); }

		// 게임 루프에서 30Hz 로 부른다
		void SendInput(uint32_t tick, const MoveInput& in);

		// 마지막으로 받은 스냅샷을 가져간다. 새 게 없으면 false.
		bool PollSnapshot(ServerSnapshot& out);

		// 통계 (타이틀바 표시용)
		uint64_t SnapshotsReceived() const { return snapshotsRecv.load(std::memory_order_relaxed); }
		uint32_t LastSnapshotPlayers() const { return lastCount.load(std::memory_order_relaxed); }

	private:
		void RecvLoop();
		void HandlePacket(const Shared::PacketHeader* head);
		bool PostRecv();

		SOCKET sock = INVALID_SOCKET;
		HANDLE iocp = nullptr;
		std::thread recvThread;

		std::atomic<bool> connected{ false };
		std::atomic<bool> running{ false };
		std::atomic<uint32_t> myPlayerId{ 0 };
		std::atomic<uint32_t> myMatchId{ 0 };
		std::atomic<uint8_t>  myTeam{ 0 };
		std::atomic<uint64_t> snapshotsRecv{ 0 };
		std::atomic<uint32_t> lastCount{ 0 };

		OVERLAPPED recvOv{};
		char       recvBuf[32 * 1024]{};
		std::vector<char> assembly;

		std::mutex snapshotMutex;
		ServerSnapshot pending;
		bool pendingFresh = false;
	};
}
