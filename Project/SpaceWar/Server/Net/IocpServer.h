#pragma once
#include <winsock2.h>
#include <mswsock.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include "Session.h"

// ============================================================
//  IocpServer — 전송 계층 전부
//
//  ★ IOCP 가 뭔가 (한 줄로)
//    "소켓 1000개를 스레드 1000개로 보는" 대신
//    "소켓 1000개를 스레드 8개가 돌아가며 본다".
//
//    스레드 하나가 소켓 하나를 붙잡고 recv 에서 잠들면(블로킹 모델),
//    동접 100명 = 스레드 100개 = 문맥 전환 지옥이 된다.
//    IOCP 는 "다 됐으면 알려줘" 방식이라, 실제로 데이터가 온 소켓만
//    남는 스레드가 집어간다. 그래서 스레드 수를 코어 수에 맞출 수 있다.
//
//  ★ 목표 성능 : 코어 1개당 동접 100명 (= 경기 1개)
//    50 대 50 이 한 경기 100명이므로, 코어 1개가 경기 1개를 감당하는 셈이다.
//
//  ★ 이 클래스는 게임 규칙을 전혀 모른다
//    받은 패킷을 콜백으로 위에 올려줄 뿐이다. 게임은 Match 가 한다.
// ============================================================

namespace swc {

	class IocpServer
	{
	public:
		~IocpServer();

		// port  : 리슨 포트
		// worker: IOCP 워커 스레드 수. 0 이면 코어 수 x 2 로 잡는다.
		bool Start(uint16_t port, int workerCount = 0);
		void Stop();

		// 완전한 패킷 하나가 도착할 때마다 불린다. (IOCP 워커 스레드에서 호출됨)
		void SetPacketHandler(PacketHandler h) { onPacket = std::move(h); }
		void SetConnectHandler(std::function<void(const std::shared_ptr<Session>&)> h)
		{
			onConnect = std::move(h);
		}
		void SetDisconnectHandler(std::function<void(const std::shared_ptr<Session>&)> h)
		{
			onDisconnect = std::move(h);
		}

		std::shared_ptr<Session> Find(uint32_t sessionId) const;
		size_t SessionCount() const;

		// 통계 (초당 대역폭 계산용. 누적값이므로 호출자가 차분을 낸다)
		uint64_t TotalBytesRecv() const;
		uint64_t TotalBytesSent() const;
		uint32_t TimedOutCount() const { return timedOut.load(std::memory_order_relaxed); }

	private:
		void WorkerLoop();                 // IOCP 워커 스레드 본체
		void JanitorLoop();                // 죽은 연결 청소 스레드
		bool PostAccept();                 // 다음 접속을 미리 예약
		void OnAcceptComplete(IoContext* ctx);
		void RemoveSession(const std::shared_ptr<Session>& s);

		SOCKET listenSocket = INVALID_SOCKET;
		HANDLE iocp = nullptr;

		// ★ AcceptEx 는 Winsock 표준 함수가 아니라 확장 함수라
		//   실행 중에 주소를 물어봐서 써야 한다 (아래 .cpp 참고)
		LPFN_ACCEPTEX acceptExPtr = nullptr;

		std::vector<std::thread> workers;
		std::thread janitor;
		std::atomic<bool> running{ false };
		std::atomic<uint32_t> nextSessionId{ 1 };   // 0 은 "없음" 으로 예약
		std::atomic<uint32_t> timedOut{ 0 };

		// 끊긴 세션이 남기고 간 누적 전송량 (살아있는 세션 것과 합쳐서 총계를 낸다)
		std::atomic<uint64_t> closedBytesRecv{ 0 };
		std::atomic<uint64_t> closedBytesSent{ 0 };

		// 세션 목록. 읽기(찾기)가 쓰기(접속/종료)보다 훨씬 잦으므로
		// shared_mutex 로 읽기는 여럿이 동시에 통과시킨다.
		mutable std::shared_mutex sessionMutex;
		std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions;

		PacketHandler onPacket;
		std::function<void(const std::shared_ptr<Session>&)> onConnect;
		std::function<void(const std::shared_ptr<Session>&)> onDisconnect;
	};
}
