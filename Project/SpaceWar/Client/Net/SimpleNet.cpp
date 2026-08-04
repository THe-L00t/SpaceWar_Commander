#include "SimpleNet.h"
#include <ws2tcpip.h>
#include <cstring>
#include "Shared/Protocol.h"

#pragma comment(lib, "ws2_32")

namespace swc {

	namespace {

		SOCKET	g_sock = INVALID_SOCKET;
		bool	g_connected = false;

		// 수신 조립 버퍼. 서버와 완전히 같은 방식으로 자른다.
		char	g_recvBuf[8192];
		int		g_nRecvd = 0;

		unsigned g_nSent = 0;
		unsigned g_nEcho = 0;
		float    g_lastEcho[3] = { 0.0f, 0.0f, 0.0f };

		std::wstring ToWide(const char* s)
		{
			if (!s) return L"";
			const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
			std::wstring w(n > 0 ? n - 1 : 0, L'\0');
			if (n > 0) ::MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
			return w;
		}

		// ── 받은 바이트에서 완전한 패킷만 꺼내 해석한다 ──
		//
		//  ★ 서버와 똑같은 처리가 클라에도 필요하다
		//    TCP 는 양방향 모두 스트림이다. 서버가 보낸 32바이트 에코도
		//    20+12 로 쪼개져 오거나 두 개가 붙어서 올 수 있다.
		//    "recv 한 번 = 패킷 한 개" 로 가정하면 좌표가 깨진다.
		void ProcessPackets()
		{
			const int nPacketSize = (int)sizeof(Shared::PlayerMovePacket);
			int nOffset = 0;

			while (g_nRecvd - nOffset >= nPacketSize)
			{
				const Shared::PlayerMovePacket* pMove =
					(const Shared::PlayerMovePacket*)(g_recvBuf + nOffset);

				if (pMove->header.type == Shared::PacketType::PlayerMove)
				{
					g_lastEcho[0] = pMove->pos[0];
					g_lastEcho[1] = pMove->pos[1];
					g_lastEcho[2] = pMove->pos[2];
					++g_nEcho;
				}

				nOffset += nPacketSize;
			}

			// 처리하고 남은 자투리를 앞으로 당겨둔다. 다음 수신 때 이어붙는다.
			g_nRecvd -= nOffset;
			if (g_nRecvd > 0 && nOffset > 0)
				::memmove(g_recvBuf, g_recvBuf + nOffset, g_nRecvd);
		}
	}

	// ── 접속 ────────────────────────────────────────────────
	bool net_connect(const char* host, unsigned short port, std::wstring& error)
	{
		WSADATA wsa = { 0 };
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			error = L"WSAStartup 실패";
			return false;
		}

		g_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (g_sock == INVALID_SOCKET)
		{
			error = L"소켓 생성 실패";
			return false;
		}

		sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_port = ::htons(port);
		if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
		{
			error = L"주소 형식 오류: " + ToWide(host);
			::closesocket(g_sock); g_sock = INVALID_SOCKET;
			return false;
		}

		// 접속은 한 번뿐이고 게임 시작 전이라 잠깐 멈춰도 된다. 그래서 블로킹.
		if (::connect(g_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			error = L"접속 실패 (서버가 켜져 있는지 확인). WSA="
				+ std::to_wstring(::WSAGetLastError());
			::closesocket(g_sock); g_sock = INVALID_SOCKET;
			return false;
		}

		// ★ Nagle 끄기
		//   1/30초마다 32바이트씩 보내는데, Nagle 이 켜져 있으면
		//   작은 패킷을 모으느라 최대 40ms 를 붙잡아 둔다.
		BOOL nodelay = TRUE;
		::setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
			(const char*)&nodelay, sizeof(nodelay));

		// ★ 논블로킹으로 전환
		//   이제부터 recv 는 받을 게 없으면 즉시 돌아온다. 렌더 루프가 안 멈춘다.
		u_long nonblock = 1;
		::ioctlsocket(g_sock, FIONBIO, &nonblock);

		g_nRecvd = 0;
		g_nSent = 0;
		g_nEcho = 0;
		g_connected = true;
		return true;
	}

	void net_disconnect()
	{
		if (g_sock != INVALID_SOCKET)
		{
			::shutdown(g_sock, SD_BOTH);
			::closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
		if (g_connected)
		{
			g_connected = false;
			::WSACleanup();
		}
	}

	bool net_connected() { return g_connected; }

	// ── 좌표 전송 ───────────────────────────────────────────
	//
	//  ★ 렌더 루프에서 1/30초마다 호출한다.
	//    구조체를 그대로 바이트로 보내므로, 서버와 같은 헤더(Shared/Protocol.h)를
	//    쓰는 한 크기와 순서가 어긋날 수 없다.
	void send_to_server(float x, float y, float z)
	{
		if (!g_connected) return;

		Shared::PlayerMovePacket pkt = {};
		pkt.header.size = (uint16_t)sizeof(pkt);
		pkt.header.type = Shared::PacketType::PlayerMove;
		pkt.playerId = 0;              // 번호는 서버가 붙여준다
		pkt.pos[0] = x;
		pkt.pos[1] = y;
		pkt.pos[2] = z;
		pkt.velocity[0] = 0.0f;
		pkt.velocity[1] = 0.0f;
		pkt.velocity[2] = 0.0f;

		// ★ 논블로킹 소켓이라 send 가 일부만 보낼 수 있다.
		//   보낸 만큼 빼고 남은 것을 이어서 보낸다.
		//   (32바이트라 사실상 한 번에 나가지만, 안 하면 언젠가 좌표가 깨진다)
		const char* p = (const char*)&pkt;
		int nTotal = (int)sizeof(pkt);
		int nSent = 0;

		while (nSent < nTotal)
		{
			const int n = ::send(g_sock, p + nSent, nTotal - nSent, 0);
			if (n > 0) { nSent += n; continue; }

			if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK)
			{
				// 커널 송신 버퍼가 찼다. 다음 프레임에 다시 시도하게 두는 게 맞지만,
				// 이 단계에서는 32바이트라 여기 걸릴 일이 사실상 없다.
				// 걸린다면 그 프레임의 좌표는 버린다 (다음 좌표가 곧 온다).
				return;
			}
			g_connected = false;   // 진짜 오류 = 연결이 끊겼다
			return;
		}
		++g_nSent;
	}

	// ── 수신 ────────────────────────────────────────────────
	//  매 프레임 호출한다. 받을 게 없으면 즉시 돌아온다.
	void net_poll()
	{
		if (!g_connected) return;

		for (;;)
		{
			const int nSpace = (int)sizeof(g_recvBuf) - g_nRecvd;
			if (nSpace <= 0) { g_nRecvd = 0; break; }   // 방어: 해석 못 하는 쓰레기가 찼다

			const int n = ::recv(g_sock, g_recvBuf + g_nRecvd, nSpace, 0);

			if (n > 0) { g_nRecvd += n; continue; }     // 더 있는지 계속 읽는다
			if (n == 0) { g_connected = false; break; } // 서버가 정상 종료

			if (::WSAGetLastError() == WSAEWOULDBLOCK) break;   // 지금은 없다. 정상.
			g_connected = false;
			break;
		}

		ProcessPackets();
	}

	unsigned net_sent_count() { return g_nSent; }
	unsigned net_echo_count() { return g_nEcho; }

	void net_last_echo(float outPos[3])
	{
		outPos[0] = g_lastEcho[0];
		outPos[1] = g_lastEcho[1];
		outPos[2] = g_lastEcho[2];
	}
}
