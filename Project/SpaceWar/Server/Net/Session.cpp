#include "Session.h"
#include <ws2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace swc {

	Session::Session(SOCKET s, uint32_t id)
		: socket(s), sessionId(id)
	{
		assembly.reserve(32 * 1024);
	}

	Session::~Session()
	{
		// 여기 도달했다는 건 걸려 있던 IO 가 전부 회수됐다는 뜻이다.
		// (IoContext 가 shared_ptr 로 잡고 있었으므로)
		if (socket != INVALID_SOCKET)
			::closesocket(socket);
	}

	// ── 수신 예약 ────────────────────────────────────────────
	//  WSARecv 는 "받을 때까지 기다리는" 함수가 아니다.
	//  "받으면 알려달라"고 커널에 예약만 걸고 즉시 돌아온다.
	//  실제 데이터가 도착하면 IOCP 워커의 GQCS 가 깨어난다.
	bool Session::PostRecv()
	{
		if (Closed()) return false;

		std::memset(&recvCtx.ov, 0, sizeof(recvCtx.ov));
		recvCtx.type = IoType::Recv;
		recvCtx.owner = shared_from_this();   // ★ 이 IO 가 끝날 때까지 나를 살려둔다

		WSABUF buf{};
		buf.buf = recvBuf;
		buf.len = static_cast<ULONG>(sizeof(recvBuf));

		DWORD flags = 0;
		DWORD received = 0;

		const int r = ::WSARecv(socket, &buf, 1, &received, &flags, &recvCtx.ov, nullptr);
		if (r == SOCKET_ERROR)
		{
			const int err = ::WSAGetLastError();
			// WSA_IO_PENDING = 정상. "예약됐다"는 뜻이다.
			if (err != WSA_IO_PENDING)
			{
				recvCtx.owner.reset();        // 완료 통지가 안 오므로 직접 풀어준다
				Close();
				return false;
			}
		}
		return true;
	}

	// ── 수신 완료 : TCP 스트림을 패킷으로 자른다 ─────────────
	//
	//  ★ 여기가 서버에서 제일 많이 틀리는 곳이다.
	//    TCP 는 바이트 스트림이라 패킷 경계가 없다.
	//      - send 3번 -> recv 1번에 몰려서 올 수 있고
	//      - send 1번 -> recv 2번에 쪼개져 올 수 있다
	//    "recv 한 번 = 패킷 한 개" 라고 가정하면 반드시 터진다.
	//    그래서 헤더의 size 를 보고 우리가 직접 잘라야 한다.
	void Session::OnRecvComplete(DWORD bytes, const PacketHandler& handler)
	{
		if (bytes == 0)          // 상대가 연결을 정상 종료했다
		{
			Close();
			return;
		}

		assembly.insert(assembly.end(), recvBuf, recvBuf + bytes);

		size_t offset = 0;
		while (assembly.size() - offset >= sizeof(Shared::PacketHeader))
		{
			const auto* head =
				reinterpret_cast<const Shared::PacketHeader*>(assembly.data() + offset);

			// 방어 : 조작된 헤더로 거대한 메모리를 잡게 만들 수 있다
			if (head->size < sizeof(Shared::PacketHeader) ||
				head->size > Shared::kMaxPacketSize)
			{
				Close();
				return;
			}

			if (assembly.size() - offset < head->size)
				break;                        // 아직 덜 왔다 -> 다음 수신을 기다린다

			if (handler)
				handler(shared_from_this(), head);

			offset += head->size;
		}

		// 처리한 만큼 앞에서 잘라낸다.
		// (매번 erase 하면 memmove 가 잦으므로 offset 으로 모아서 한 번에)
		if (offset > 0)
			assembly.erase(assembly.begin(), assembly.begin() + offset);

		PostRecv();                           // 다음 수신을 다시 예약
	}

	// ── 송신 ────────────────────────────────────────────────
	//
	//  ★ 왜 큐가 필요한가
	//    같은 소켓에 WSASend 를 두 개 동시에 걸면 커널이 완료 순서를 보장하지 않는다.
	//    스트림 중간이 뒤바뀌면 그 뒤로는 전부 쓰레기가 된다.
	//    그래서 "한 번에 하나만 발사, 나머지는 대기" 로 직렬화한다.
	void Session::Send(const void* data, uint16_t size)
	{
		if (Closed() || size == 0) return;

		std::vector<char> packet(static_cast<const char*>(data),
			static_cast<const char*>(data) + size);

		std::lock_guard<std::mutex> lock(sendMutex);
		sendQueue.push_back(std::move(packet));

		if (!sending)
			FlushSendQueue();
	}

	void Session::FlushSendQueue()
	{
		if (sendQueue.empty() || Closed()) return;

		// 대기 중인 패킷을 하나로 합쳐서 한 번에 보낸다.
		// send 호출 횟수가 곧 커널 진입 횟수라, 묶을수록 싸다.
		sendCtx.sendData.clear();
		while (!sendQueue.empty())
		{
			const auto& front = sendQueue.front();
			if (!sendCtx.sendData.empty() &&
				sendCtx.sendData.size() + front.size() > 64 * 1024)
				break;                        // 한 번에 64KB 까지만

			sendCtx.sendData.insert(sendCtx.sendData.end(), front.begin(), front.end());
			sendQueue.pop_front();
		}
		if (sendCtx.sendData.empty()) return;

		std::memset(&sendCtx.ov, 0, sizeof(sendCtx.ov));
		sendCtx.type = IoType::Send;
		sendCtx.owner = shared_from_this();

		sendCtx.wsabuf.buf = sendCtx.sendData.data();
		sendCtx.wsabuf.len = static_cast<ULONG>(sendCtx.sendData.size());

		DWORD sent = 0;
		sending = true;

		const int r = ::WSASend(socket, &sendCtx.wsabuf, 1, &sent, 0, &sendCtx.ov, nullptr);
		if (r == SOCKET_ERROR)
		{
			const int err = ::WSAGetLastError();
			if (err != WSA_IO_PENDING)
			{
				sending = false;
				sendCtx.owner.reset();
				Close();
			}
		}
	}

	void Session::OnSendComplete(DWORD /*bytes*/)
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		sending = false;
		FlushSendQueue();                     // 그사이 쌓인 게 있으면 이어서 발사
	}

	// ── 종료 ────────────────────────────────────────────────
	//
	//  ★ 여기서 closesocket 을 하지 않는다 — 중요하다
	//    소켓을 닫으면 그 핸들 번호가 즉시 재사용될 수 있다.
	//    아직 회수 안 된 IO 가 남아 있으면 "엉뚱한 새 소켓"에 대고 동작하게 된다.
	//    (재현이 거의 불가능한 버그다)
	//
	//    그래서 여기서는 shutdown + CancelIoEx 로 "걸려 있는 IO 를 끝내기만" 한다.
	//    실제 closesocket 은 마지막 IO 가 회수돼 shared_ptr 이 0이 될 때,
	//    즉 소멸자에서 한다.
	void Session::Close()
	{
		bool expected = false;
		if (!closed.compare_exchange_strong(expected, true))
			return;                           // 이미 닫혔다 (여러 스레드가 동시에 부를 수 있다)

		::shutdown(socket, SD_BOTH);
		::CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
	}
}
