#include "IocpServer.h"
#include <ws2tcpip.h>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace swc {

	namespace {
		// 동시에 걸어두는 Accept 예약 개수.
		// 하나만 걸어두면 접속이 몰릴 때 "예약 -> 완료 -> 재예약" 사이의 틈에서
		// 접속이 대기열에 쌓이다 거절된다. 미리 여러 개 깔아둔다.
		constexpr int kBacklogPosts = 32;
	}

	IocpServer::~IocpServer()
	{
		Stop();
	}

	// ── 기동 ────────────────────────────────────────────────
	bool IocpServer::Start(uint16_t port, int workerCount)
	{
		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			std::printf("[net] WSAStartup 실패\n");
			return false;
		}

		// ① 완료 포트 생성 — "IO 가 끝났다"는 통지가 여기 쌓인다
		iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		if (!iocp)
		{
			std::printf("[net] CreateIoCompletionPort 실패\n");
			return false;
		}

		// ② 리슨 소켓
		//   WSA_FLAG_OVERLAPPED 가 있어야 IOCP 에 붙일 수 있다.
		listenSocket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (listenSocket == INVALID_SOCKET)
		{
			std::printf("[net] 리슨 소켓 생성 실패\n");
			return false;
		}

		// 서버를 껐다 바로 켤 때 "이미 사용 중인 주소" 를 피한다
		BOOL reuse = TRUE;
		::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
			reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = ::htons(port);

		if (::bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			std::printf("[net] bind 실패 (포트 %u 사용 중?)\n", port);
			return false;
		}
		if (::listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
		{
			std::printf("[net] listen 실패\n");
			return false;
		}

		// ③ AcceptEx 주소 얻기
		//   AcceptEx 는 Winsock 표준이 아니라 마이크로소프트 확장 함수다.
		//   DLL 마다 구현이 달라서, 컴파일 시점에 링크하지 않고
		//   실행 중에 소켓한테 "네 AcceptEx 주소가 뭐냐"고 물어본다.
		GUID guidAcceptEx = WSAID_ACCEPTEX;
		DWORD bytes = 0;
		if (::WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&guidAcceptEx, sizeof(guidAcceptEx),
			&acceptExPtr, sizeof(acceptExPtr),
			&bytes, nullptr, nullptr) == SOCKET_ERROR)
		{
			std::printf("[net] AcceptEx 주소 획득 실패\n");
			return false;
		}

		// ④ 리슨 소켓을 완료 포트에 등록
		if (!::CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSocket), iocp, 0, 0))
		{
			std::printf("[net] 리슨 소켓 IOCP 등록 실패\n");
			return false;
		}

		// ⑤ 워커 스레드
		//   ★ 왜 코어 수 x 2 인가
		//     IOCP 는 "실행 가능한 스레드 수" 를 코어 수로 제한한다.
		//     여분을 둬야 한 스레드가 잠깐 블로킹돼도 다른 스레드가 즉시 이어받는다.
		if (workerCount <= 0)
			workerCount = static_cast<int>(std::thread::hardware_concurrency()) * 2;
		if (workerCount <= 0) workerCount = 4;

		running.store(true);
		for (int i = 0; i < workerCount; ++i)
			workers.emplace_back([this] { WorkerLoop(); });

		// ⑦ 죽은 연결 청소 스레드
		janitor = std::thread([this] { JanitorLoop(); });

		// ⑧ Accept 를 미리 여러 개 깔아둔다
		for (int i = 0; i < kBacklogPosts; ++i)
			PostAccept();

		std::printf("[net] 포트 %u 대기 시작 (워커 스레드 %d개)\n", port, workerCount);
		return true;
	}

	// ── 죽은 연결 청소 ──────────────────────────────────────
	//
	//  ★ 왜 필요한가
	//    랜선을 뽑거나 클라가 강제 종료되면 TCP 는 그것을 바로 알려주지 않는다.
	//    OS 기본 keepalive 는 2시간이라 그때까지 세션이 자리를 차지한다.
	//    50 대 50 경기에서 유령 한 명이 2시간 남아 있으면 매칭이 막힌다.
	//
	//    그래서 "마지막으로 뭔가 받은 시각" 을 직접 재고 오래된 것을 끊는다.
	//    클라는 30Hz 로 입력을 보내므로, 15초 무응답이면 확실히 죽은 것이다.
	void IocpServer::JanitorLoop()
	{
		using namespace std::chrono;

		while (running.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(seconds(1));
			if (!running.load(std::memory_order_relaxed)) break;

			const int64_t now =
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

			// 끊을 대상을 먼저 모은다.
			// 락을 잡은 채 Close() 를 부르면 그 안에서 다시 락을 잡으려 해 교착할 수 있다.
			std::vector<std::shared_ptr<Session>> dead;
			{
				std::shared_lock lock(sessionMutex);
				for (auto& [id, s] : sessions)
					if (now - s->LastActivityMs() > kIdleTimeoutMs)
						dead.push_back(s);
			}

			for (auto& s : dead)
			{
				timedOut.fetch_add(1, std::memory_order_relaxed);
				s->Close();      // 걸린 IO 가 실패로 완료되며 WorkerLoop 이 정리한다
			}
		}
	}

	void IocpServer::Stop()
	{
		if (!running.exchange(false)) return;

		if (janitor.joinable()) janitor.join();

		// 접속 중인 세션 전부 종료
		{
			std::unique_lock lock(sessionMutex);
			for (auto& [id, s] : sessions)
			{
				closedBytesRecv.fetch_add(s->BytesRecv(), std::memory_order_relaxed);
				closedBytesSent.fetch_add(s->BytesSent(), std::memory_order_relaxed);
				s->Close();
			}
			sessions.clear();
		}

		if (listenSocket != INVALID_SOCKET)
		{
			::closesocket(listenSocket);
			listenSocket = INVALID_SOCKET;
		}

		// 워커를 깨우기 위해 가짜 완료 통지를 스레드 수만큼 밀어넣는다.
		// (GQCS 에서 잠들어 있으므로 이것 말고는 깨울 방법이 없다)
		if (iocp)
			for (size_t i = 0; i < workers.size(); ++i)
				::PostQueuedCompletionStatus(iocp, 0, 0, nullptr);

		for (auto& t : workers)
			if (t.joinable()) t.join();
		workers.clear();

		if (iocp) { ::CloseHandle(iocp); iocp = nullptr; }
		::WSACleanup();
	}

	// ── Accept 예약 ─────────────────────────────────────────
	//
	//  ★ 일반 accept() 와 다른 점
	//    accept() 는 "올 때까지 기다리는" 함수라 스레드가 묶인다.
	//    AcceptEx 는 "접속이 오면 이 소켓에 꽂아줘" 라고 미리 예약만 한다.
	//    그래서 접속 대기에 스레드를 쓰지 않는다.
	bool IocpServer::PostAccept()
	{
		if (!running.load()) return false;

		// AcceptEx 는 소켓을 만들어주지 않는다. 우리가 미리 만들어 넘겨야 한다.
		SOCKET client = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (client == INVALID_SOCKET) return false;

		auto* ctx = new IoContext();     // 완료 통지에서 회수한다
		ctx->type = IoType::Accept;
		ctx->acceptSocket = client;

		DWORD received = 0;
		// 마지막 두 인자를 주소 크기로만 채우고 데이터 길이를 0 으로 주면
		// "접속만 받고 데이터는 기다리지 않는다" 는 뜻이다.
		// (0 이 아니면 첫 데이터가 올 때까지 완료되지 않아 접속이 늦어진다)
		const BOOL ok = acceptExPtr(
			listenSocket, client,
			ctx->acceptBuffer, 0,
			sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
			&received, &ctx->ov);

		if (!ok && ::WSAGetLastError() != ERROR_IO_PENDING)
		{
			::closesocket(client);
			delete ctx;
			return false;
		}
		return true;
	}

	void IocpServer::OnAcceptComplete(IoContext* ctx)
	{
		const SOCKET client = ctx->acceptSocket;

		// ★ 이 한 줄을 빠뜨리면 getpeername 등이 실패하고
		//   소켓이 리슨 소켓의 속성을 물려받지 못한다. AcceptEx 필수 후처리다.
		::setsockopt(client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
			reinterpret_cast<const char*>(&listenSocket), sizeof(listenSocket));

		// ★ Nagle 알고리즘 끄기
		//   Nagle 은 작은 패킷을 모아서 한 번에 보내 대역폭을 아끼는 기능인데,
		//   그 대가로 최대 40ms 를 붙잡아 둔다.
		//   우리는 33ms 마다 작은 입력을 보내므로 이게 켜져 있으면 조작이 밀린다.
		BOOL nodelay = TRUE;
		::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

		const uint32_t id = nextSessionId.fetch_add(1);
		auto session = std::make_shared<Session>(client, id);

		// 이 소켓을 완료 포트에 등록.
		// CompletionKey 로 세션 id 를 넘겨두면 완료 통지에서 누구인지 바로 안다.
		if (!::CreateIoCompletionPort(reinterpret_cast<HANDLE>(client), iocp,
			static_cast<ULONG_PTR>(id), 0))
		{
			::closesocket(client);
			return;
		}

		{
			std::unique_lock lock(sessionMutex);
			sessions.emplace(id, session);
		}

		if (onConnect) onConnect(session);

		session->PostRecv();      // 첫 수신 예약. 이때부터 데이터가 들어온다
	}

	// ── 워커 스레드 본체 ────────────────────────────────────
	//
	//  모든 IO 완료가 여기로 모인다. 스레드 여러 개가 같은 루프를 돈다.
	//  커널이 "완료된 것이 있는 스레드만" 깨우므로 놀고 있는 스레드는 CPU 를 안 쓴다.
	void IocpServer::WorkerLoop()
	{
		while (running.load())
		{
			DWORD bytes = 0;
			ULONG_PTR key = 0;
			OVERLAPPED* ov = nullptr;

			const BOOL ok = ::GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, INFINITE);

			if (ov == nullptr)
				break;            // Stop() 이 밀어넣은 종료 신호

			// ★ OVERLAPPED* 를 IoContext* 로 되돌린다.
			//   ov 가 IoContext 의 첫 멤버라서 주소가 같다.
			auto* ctx = reinterpret_cast<IoContext*>(ov);

			// ★ 소유권을 먼저 빼낸다.
			//   여기서 owner 를 비워야 Session 이 같은 ctx 에 다음 IO 를 걸 수 있다.
			//   owner 는 이 함수가 끝날 때까지 세션을 살려준다.
			std::shared_ptr<Session> owner = std::move(ctx->owner);

			switch (ctx->type)
			{
			case IoType::Accept:
				if (ok) OnAcceptComplete(ctx);
				else if (ctx->acceptSocket != INVALID_SOCKET) ::closesocket(ctx->acceptSocket);
				delete ctx;          // Accept 문맥은 매번 새로 만들었으므로 여기서 해제
				PostAccept();        // 하나 소비했으니 하나 다시 깔아둔다
				break;

			case IoType::Recv:
				if (!owner) break;
				if (!ok || bytes == 0)
				{
					owner->Close();
					RemoveSession(owner);
				}
				else
				{
					owner->OnRecvComplete(bytes, onPacket);
					if (owner->Closed()) RemoveSession(owner);
				}
				break;

			case IoType::Send:
				if (!owner) break;
				if (!ok)
				{
					owner->Close();
					RemoveSession(owner);
				}
				else
				{
					owner->OnSendComplete(bytes);
				}
				break;
			}
		}
	}

	void IocpServer::RemoveSession(const std::shared_ptr<Session>& s)
	{
		bool existed = false;
		{
			std::unique_lock lock(sessionMutex);
			existed = sessions.erase(s->Id()) > 0;
		}
		if (!existed) return;   // 다른 스레드가 이미 지웠다 (콜백 중복 호출 방지)

		// 사라질 세션의 누적 전송량을 총계에 합친다
		closedBytesRecv.fetch_add(s->BytesRecv(), std::memory_order_relaxed);
		closedBytesSent.fetch_add(s->BytesSent(), std::memory_order_relaxed);

		if (onDisconnect) onDisconnect(s);
	}

	std::shared_ptr<Session> IocpServer::Find(uint32_t sessionId) const
	{
		std::shared_lock lock(sessionMutex);
		auto it = sessions.find(sessionId);
		return it != sessions.end() ? it->second : nullptr;
	}

	size_t IocpServer::SessionCount() const
	{
		std::shared_lock lock(sessionMutex);
		return sessions.size();
	}

	uint64_t IocpServer::TotalBytesRecv() const
	{
		uint64_t n = closedBytesRecv.load(std::memory_order_relaxed);
		std::shared_lock lock(sessionMutex);
		for (auto& [id, s] : sessions) n += s->BytesRecv();
		return n;
	}

	uint64_t IocpServer::TotalBytesSent() const
	{
		uint64_t n = closedBytesSent.load(std::memory_order_relaxed);
		std::shared_lock lock(sessionMutex);
		for (auto& [id, s] : sessions) n += s->BytesSent();
		return n;
	}
}
