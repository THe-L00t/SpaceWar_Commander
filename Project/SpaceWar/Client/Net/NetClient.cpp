#include "NetClient.h"
#include <ws2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace swc {

	namespace {
		std::wstring ToWide(const char* s)
		{
			if (!s) return L"";
			const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
			std::wstring w(n > 0 ? n - 1 : 0, L'\0');
			if (n > 0) ::MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
			return w;
		}
	}

	NetClient::~NetClient()
	{
		Disconnect();
	}

	// ── 접속 ────────────────────────────────────────────────
	//
	//  접속 자체는 blocking connect 로 한다. 딱 한 번뿐이고,
	//  게임이 시작되기 전이라 잠깐 멈춰도 문제가 없기 때문이다.
	//  (접속을 비동기로 하려면 ConnectEx 를 써야 하는데 여기선 이득이 없다)
	//  그 뒤의 송수신만 IOCP 로 돌린다.
	bool NetClient::Connect(const char* host, uint16_t port, std::wstring& error)
	{
		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			error = L"WSAStartup 실패";
			return false;
		}

		sock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			nullptr, 0, WSA_FLAG_OVERLAPPED);   // ★ OVERLAPPED 여야 IOCP 에 붙는다
		if (sock == INVALID_SOCKET)
		{
			error = L"소켓 생성 실패";
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = ::htons(port);
		if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
		{
			error = L"주소 형식 오류: " + ToWide(host);
			::closesocket(sock); sock = INVALID_SOCKET;
			return false;
		}

		if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			error = L"접속 실패 (서버가 꺼져 있는지 확인). WSA=" +
				std::to_wstring(::WSAGetLastError());
			::closesocket(sock); sock = INVALID_SOCKET;
			return false;
		}

		// ★ Nagle 끄기. 33ms 마다 28바이트짜리 입력을 보내는데
		//   Nagle 이 켜져 있으면 최대 40ms 를 모았다 보내서 조작이 밀린다.
		BOOL nodelay = TRUE;
		::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

		iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		if (!iocp || !::CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), iocp, 1, 0))
		{
			error = L"IOCP 등록 실패";
			::closesocket(sock); sock = INVALID_SOCKET;
			return false;
		}

		assembly.reserve(64 * 1024);
		connected.store(true, std::memory_order_release);
		running.store(true);

		recvThread = std::thread([this] { RecvLoop(); });
		PostRecv();
		return true;
	}

	void NetClient::Disconnect()
	{
		if (!running.exchange(false)) return;

		connected.store(false, std::memory_order_release);

		if (sock != INVALID_SOCKET)
		{
			::shutdown(sock, SD_BOTH);
			::CancelIoEx(reinterpret_cast<HANDLE>(sock), nullptr);
		}
		// 수신 스레드가 GQCS 에서 자고 있을 수 있으니 가짜 완료로 깨운다
		if (iocp) ::PostQueuedCompletionStatus(iocp, 0, 0, nullptr);

		if (recvThread.joinable()) recvThread.join();

		if (sock != INVALID_SOCKET) { ::closesocket(sock); sock = INVALID_SOCKET; }
		if (iocp) { ::CloseHandle(iocp); iocp = nullptr; }
		::WSACleanup();
	}

	bool NetClient::PostRecv()
	{
		if (!running.load()) return false;

		std::memset(&recvOv, 0, sizeof(recvOv));

		WSABUF buf{};
		buf.buf = recvBuf;
		buf.len = static_cast<ULONG>(sizeof(recvBuf));

		DWORD flags = 0, received = 0;
		const int r = ::WSARecv(sock, &buf, 1, &received, &flags, &recvOv, nullptr);
		if (r == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
		{
			connected.store(false, std::memory_order_release);
			return false;
		}
		return true;
	}

	// ── 수신 스레드 ─────────────────────────────────────────
	//  게임 루프와 완전히 분리돼 있다. 여기가 막혀도 화면은 계속 돈다.
	void NetClient::RecvLoop()
	{
		while (running.load(std::memory_order_relaxed))
		{
			DWORD bytes = 0;
			ULONG_PTR key = 0;
			OVERLAPPED* ov = nullptr;

			const BOOL ok = ::GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, INFINITE);

			if (ov == nullptr) break;                 // Disconnect() 가 보낸 종료 신호
			if (!ok || bytes == 0)                    // 서버가 끊었다
			{
				connected.store(false, std::memory_order_release);
				break;
			}

			// ── TCP 스트림 자르기 (서버와 완전히 같은 방식) ──
			assembly.insert(assembly.end(), recvBuf, recvBuf + bytes);

			size_t offset = 0;
			while (assembly.size() - offset >= sizeof(Shared::PacketHeader))
			{
				const auto* head =
					reinterpret_cast<const Shared::PacketHeader*>(assembly.data() + offset);

				if (head->size < sizeof(Shared::PacketHeader) ||
					head->size > Shared::kMaxPacketSize)
				{
					connected.store(false, std::memory_order_release);
					return;
				}
				if (assembly.size() - offset < head->size) break;   // 아직 덜 왔다

				HandlePacket(head);
				offset += head->size;
			}
			if (offset > 0) assembly.erase(assembly.begin(), assembly.begin() + offset);

			if (!PostRecv()) break;
		}
		connected.store(false, std::memory_order_release);
	}

	void NetClient::HandlePacket(const Shared::PacketHeader* head)
	{
		switch (head->type)
		{
		case Shared::PacketType::ServerHello:
		{
			if (head->size != sizeof(Shared::ServerHelloPacket)) return;
			const auto* p = reinterpret_cast<const Shared::ServerHelloPacket*>(head);
			myPlayerId.store(p->playerId, std::memory_order_relaxed);
			myMatchId.store(p->matchId, std::memory_order_relaxed);
			myTeam.store(p->team, std::memory_order_relaxed);
			break;
		}

		case Shared::PacketType::PlayerState:
		{
			// 가변 길이 패킷이다. 헤더가 말한 개수와 실제 크기가 맞는지 확인한다.
			if (head->size < sizeof(Shared::WorldSnapshotHeader)) return;
			const auto* snap = reinterpret_cast<const Shared::WorldSnapshotHeader*>(head);

			const size_t expect = sizeof(Shared::WorldSnapshotHeader)
				+ size_t(snap->count) * sizeof(Shared::PlayerStateEntry);
			if (head->size != expect) return;      // 조작되었거나 버전이 다르다

			const auto* e = reinterpret_cast<const Shared::PlayerStateEntry*>(
				reinterpret_cast<const char*>(head) + sizeof(Shared::WorldSnapshotHeader));

			const uint32_t me = myPlayerId.load(std::memory_order_relaxed);

			ServerSnapshot s;
			s.tick = snap->tick;
			s.ackTick = snap->ackTick;
			s.others.reserve(snap->count);

			for (uint16_t i = 0; i < snap->count; ++i)
			{
				RemotePlayer r;
				r.playerId = e[i].playerId;
				r.position = FromArray3(e[i].pos);
				r.velocity = FromArray3(e[i].vel);
				r.facing = FromArray3(e[i].facing);
				r.altitude = e[i].altitude;
				r.grounded = e[i].grounded != 0;
				r.lastSeenTick = snap->tick;

				if (r.playerId == me) { s.self = r; s.hasSelf = true; }
				else                    s.others.push_back(r);
			}

			snapshotsRecv.fetch_add(1, std::memory_order_relaxed);
			lastCount.store(snap->count, std::memory_order_relaxed);

			{
				std::lock_guard<std::mutex> lock(snapshotMutex);
				pending = std::move(s);
				pendingFresh = true;
			}
			break;
		}

		default:
			break;
		}
	}

	// ── 송신 ────────────────────────────────────────────────
	//  입력 패킷은 28바이트라 한 번에 다 나간다. 게임 루프에서 직접 보낸다.
	//  (부분 송신을 걱정할 크기가 아니고, 큐를 두면 오히려 지연이 는다)
	void NetClient::SendInput(uint32_t tick, const MoveInput& in)
	{
		if (!Connected()) return;

		Shared::PlayerInputPacket p{};
		p.header.size = sizeof(p);
		p.header.type = Shared::PacketType::PlayerInput;
		p.playerId = myPlayerId.load(std::memory_order_relaxed);
		p.tick = tick;

		// float(-1~1) 를 int8(-100~100) 로 줄여 보낸다. 정밀도는 충분하다.
		auto quant = [](float v) -> int8_t {
			const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
			return static_cast<int8_t>(c * 100.0f);
		};
		p.moveX = quant(in.moveX);
		p.moveZ = quant(in.moveZ);
		p.buttons = in.buttons;
		ToArray3(in.aimDir, p.aimDir);

		const char* data = reinterpret_cast<const char*>(&p);
		int sent = 0;
		while (sent < int(sizeof(p)))
		{
			const int n = ::send(sock, data + sent, int(sizeof(p)) - sent, 0);
			if (n <= 0) { connected.store(false, std::memory_order_release); return; }
			sent += n;
		}
	}

	bool NetClient::PollSnapshot(ServerSnapshot& out)
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		if (!pendingFresh) return false;
		out = pending;
		pendingFresh = false;
		return true;
	}
}
