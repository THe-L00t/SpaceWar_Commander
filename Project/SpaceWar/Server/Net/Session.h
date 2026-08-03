#pragma once
#include <winsock2.h>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>
#include "Shared/Protocol.h"

// ============================================================
//  Session — 연결 하나(= 접속자 한 명)
//
//  하는 일이 딱 둘이다.
//    ① 받기 : TCP 스트림을 잘라서 "완전한 패킷"만 위로 올려준다
//    ② 보내기 : 여러 스레드가 아무 때나 Send() 를 불러도 안전하게 순서대로 내보낸다
//
//  게임 규칙은 하나도 모른다. 그건 Match 가 한다.
// ============================================================

namespace swc {

	class Session;

	// 수신한 완전한 패킷 하나를 위(게임 계층)로 올리는 콜백
	using PacketHandler = std::function<void(const std::shared_ptr<Session>&,
		const Shared::PacketHeader*)>;

	// ── IO 종류 ──────────────────────────────────────────────
	//  GetQueuedCompletionStatus 는 "무슨 작업이 끝났는지" 를 알려주지 않는다.
	//  OVERLAPPED 만 돌려준다. 그래서 OVERLAPPED 를 감싸서 우리가 직접 표시해 둔다.
	//  이걸 흔히 "확장 OVERLAPPED" 패턴이라고 부른다.
	enum class IoType : uint8_t { Accept, Recv, Send };

	// ── IO 한 건의 문맥 ──────────────────────────────────────
	//
	//  ★ ov 가 반드시 첫 멤버여야 한다
	//    커널은 우리가 넘긴 OVERLAPPED* 주소를 그대로 돌려준다.
	//    첫 멤버라야 그 주소가 곧 IoContext* 라서 캐스팅 한 번에 되찾을 수 있다.
	//
	//  ★ owner 가 shared_ptr 인 이유 — IOCP 최대 함정
	//    IO 를 걸어둔 채 세션이 파괴되면, 커널이 이미 사라진 메모리에 결과를 쓴다.
	//    (= 재현이 거의 불가능한 크래시)
	//    걸어둔 IO 가 shared_ptr 로 세션을 붙잡고 있으면 그 IO 가 끝나기 전에는
	//    절대 파괴되지 않는다.
	struct IoContext
	{
		OVERLAPPED ov{};                    // ★ 반드시 첫 멤버
		IoType     type = IoType::Recv;
		std::shared_ptr<Session> owner;     // IO 가 끝날 때까지 세션을 살려둔다

		// Accept 전용 — AcceptEx 는 "미리 만들어둔 소켓"에 연결을 꽂아준다
		SOCKET acceptSocket = INVALID_SOCKET;
		char   acceptBuffer[(sizeof(sockaddr_in) + 16) * 2]{};

		// Send 전용 — 커널이 보내는 동안 이 버퍼가 살아있어야 한다
		std::vector<char> sendData;
		WSABUF            wsabuf{};
	};

	// ── 한계값 ───────────────────────────────────────────────
	//  클라가 데이터를 안 읽어가면 커널 송신 버퍼가 차고, 우리 큐가 무한히 자란다.
	//  느린 클라 하나가 서버 메모리를 다 먹는 것을 막는다. (= slow-reader 공격 방어)
	inline constexpr size_t kMaxSendQueueBytes = 1024 * 1024;   // 1MB 넘으면 강제 종료
	inline constexpr size_t kSendChunkBytes = 64 * 1024;        // 한 번에 보낼 상한
	inline constexpr int64_t kIdleTimeoutMs = 15000;            // 이 시간 무응답이면 끊는다

	class Session : public std::enable_shared_from_this<Session>
	{
	public:
		Session(SOCKET s, uint32_t id);
		~Session();

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		// 다음 수신을 커널에 예약한다. 실패하면 false.
		bool PostRecv();

		// 수신 완료 통지. 조각을 이어붙이고 완전한 패킷마다 handler 를 부른다.
		// bytes == 0 이면 상대가 정상 종료한 것이다.
		void OnRecvComplete(DWORD bytes, const PacketHandler& handler);

		// ★ 아무 스레드에서나 불러도 안전하다.
		//   실제 WSASend 는 한 번에 하나만 나가고, 나머지는 큐에 쌓인다.
		//   (동시에 두 개를 보내면 TCP 스트림에서 순서가 섞인다)
		void Send(const void* data, uint16_t size);

		void OnSendComplete(DWORD bytes);

		// 소켓을 닫아 걸려 있는 IO 를 전부 실패시킨다.
		// 실제 메모리 해제는 마지막 IO 가 회수된 뒤 자동으로 일어난다.
		void Close();

		SOCKET   Sock()   const { return socket; }
		uint32_t Id()     const { return sessionId; }
		bool     Closed() const { return closed.load(std::memory_order_acquire); }

		// 게임 계층이 붙여두는 꼬리표 (어느 경기의 몇 번 플레이어인가)
		void     SetMatchId(uint32_t m) { matchId = m; }
		uint32_t MatchId() const { return matchId; }

		// ── 죽은 연결 감지 ──
		//  TCP 는 케이블을 뽑아도 바로 알려주지 않는다. OS 기본 keepalive 는 2시간이라
		//  그때까지 자리를 차지한다. 그래서 "마지막으로 뭔가 받은 시각" 을 직접 재고
		//  IocpServer 의 청소 스레드가 오래된 세션을 끊는다.
		int64_t  LastActivityMs() const { return lastActivityMs.load(std::memory_order_relaxed); }

		uint64_t BytesRecv() const { return bytesRecv.load(std::memory_order_relaxed); }
		uint64_t BytesSent() const { return bytesSent.load(std::memory_order_relaxed); }

	private:
		void BuildAndPostSend();   // sendMutex 를 이미 잡은 상태에서 호출
		bool PostSendCurrent();    // sendMutex 를 이미 잡은 상태에서 호출

		SOCKET   socket;
		uint32_t sessionId;
		uint32_t matchId = 0;

		std::atomic<bool>    closed{ false };
		std::atomic<int64_t> lastActivityMs{ 0 };
		std::atomic<uint64_t> bytesRecv{ 0 };
		std::atomic<uint64_t> bytesSent{ 0 };

		// ── 수신 ──
		// recvCtx 는 세션당 하나. 수신은 항상 한 번에 하나만 걸어둔다.
		// (여러 개 걸면 완료 순서가 뒤바뀌어 스트림이 섞인다)
		IoContext recvCtx;
		char      recvBuf[16 * 1024]{};      // 커널이 직접 써 넣는 곳
		std::vector<char> assembly;          // 잘린 조각을 이어붙이는 곳

		// ── 송신 ──
		std::mutex sendMutex;
		std::deque<std::vector<char>> sendQueue;
		size_t     queuedBytes = 0;          // 큐에 쌓인 총 바이트 (한계 검사용)
		IoContext  sendCtx;
		size_t     sentOffset = 0;           // ★ 부분 송신 처리용. 지금 버퍼에서 보낸 양
		bool       sending = false;          // WSASend 가 진행 중인가
	};
}
