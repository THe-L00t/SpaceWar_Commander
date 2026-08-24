#include "SimpleNet.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <cstring>
#include <unordered_map>
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

		uint32_t g_myId = 0;		// 서버가 알려준 내 번호

		// ── 보간 지연 ───────────────────────────────────────────
		//  받은 좌표를 즉시 그리면, 다음 갱신이 올 때까지 멈춰 있다가 튄다.
		//  일부러 이만큼 과거를 그리면 항상 "받아둔 두 점 사이"를 지나가므로
		//  끊김이 사라진다. 대신 원격 플레이어가 이만큼 늦게 보인다.
		//  서버 갱신 간격(1/30초 = 33ms)보다 넉넉히 커야 한 번 늦게 와도 버틴다.
		constexpr double kInterpDelay = 0.10;   // 100ms

		// ── 원격 플레이어 한 명 ─────────────────────────────────
		//  마지막 두 개의 수신 좌표와 그 도착 시각을 들고 있는다.
		//  그 사이를 시간으로 훑으면 부드러운 움직임이 나온다.
		struct Remote
		{
			float  prevPos[3];
			float  currPos[3];
			double prevTime;
			double currTime;
			bool   hasPrev;
		};

		std::unordered_map<uint32_t, Remote> g_remotes;

		// ── 시각 ────────────────────────────────────────────────
		//  GetTickCount 는 해상도가 10~16ms 라 33ms 간격을 재기엔 거칠다.
		//  보간 비율이 계단처럼 튀므로 고해상도 카운터를 쓴다.
		double NowSeconds()
		{
			static LARGE_INTEGER freq = {};
			if (freq.QuadPart == 0) ::QueryPerformanceFrequency(&freq);

			LARGE_INTEGER now = {};
			::QueryPerformanceCounter(&now);
			return double(now.QuadPart) / double(freq.QuadPart);
		}

		void OnWelcome(const Shared::WelcomePacket* p)
		{
			g_myId = p->playerId;
		}

		void OnPlayerMove(const Shared::PlayerMovePacket* p)
		{
			g_lastEcho[0] = p->pos[0];
			g_lastEcho[1] = p->pos[1];
			g_lastEcho[2] = p->pos[2];
			++g_nEcho;

			// 내 좌표가 되돌아온 것은 무시한다. 그리면 큐브가 겹치고,
			// 과거 위치로 끌려가 조작이 밀리는 것처럼 보인다.
			if (p->playerId == g_myId || p->playerId == 0) return;

			Remote& r = g_remotes[p->playerId];
			const double now = NowSeconds();

			if (r.currTime > 0.0)
			{
				r.prevPos[0] = r.currPos[0];
				r.prevPos[1] = r.currPos[1];
				r.prevPos[2] = r.currPos[2];
				r.prevTime = r.currTime;
				r.hasPrev = true;
			}

			r.currPos[0] = p->pos[0];
			r.currPos[1] = p->pos[1];
			r.currPos[2] = p->pos[2];
			r.currTime = now;
		}

		void OnPlayerLeave(const Shared::PlayerLeavePacket* p)
		{
			g_remotes.erase(p->playerId);
		}

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
		//    TCP 는 양방향 모두 스트림이다. 서버가 보낸 32바이트 패킷도
		//    20+12 로 쪼개져 오거나 두 개가 붙어서 올 수 있다.
		//    "recv 한 번 = 패킷 한 개" 로 가정하면 좌표가 깨진다.
		//
		//  ★ 고정 크기로 자르면 안 된다
		//    Welcome(8) / PlayerLeave(8) / PlayerMove(32) 가 섞여서 온다.
		//    32 단위로 자르면 8바이트짜리 하나에 경계가 밀려 전부 쓰레기가 된다.
		//    반드시 header.size 만큼씩 잘라야 한다.
		void ProcessPackets()
		{
			const int nHeaderSize = (int)sizeof(Shared::PacketHeader);
			int nOffset = 0;

			while (g_nRecvd - nOffset >= nHeaderSize)
			{
				const Shared::PacketHeader* pHead =
					(const Shared::PacketHeader*)(g_recvBuf + nOffset);
				const int nSize = (int)pHead->size;

				// 규격에 없는 크기 = 스트림이 어긋났다. 이어 읽어도 복구되지 않는다.
				if (nSize < nHeaderSize || nSize >(int)sizeof(g_recvBuf))
				{
					g_nRecvd = 0;
					return;
				}

				// 아직 다 안 왔다. 다음 수신 때 이어서 처리한다.
				if (g_nRecvd - nOffset < nSize)
					break;

				switch (pHead->type)
				{
				case Shared::PacketType::Welcome:
					if (nSize == (int)sizeof(Shared::WelcomePacket))
						OnWelcome((const Shared::WelcomePacket*)pHead);
					break;

				case Shared::PacketType::PlayerMove:
					if (nSize == (int)sizeof(Shared::PlayerMovePacket))
						OnPlayerMove((const Shared::PlayerMovePacket*)pHead);
					break;

				case Shared::PacketType::PlayerLeave:
					if (nSize == (int)sizeof(Shared::PlayerLeavePacket))
						OnPlayerLeave((const Shared::PlayerLeavePacket*)pHead);
					break;

				default:
					break;		// 모르는 종류는 크기만큼 건너뛴다
				}

				nOffset += nSize;
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
		g_myId = 0;
		g_remotes.clear();
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

	// ── 원격 플레이어 ───────────────────────────────────────
	uint32_t net_my_id() { return g_myId; }

	// ★ 지금 그려야 할 위치를 시간 보간으로 만들어 낸다.
	//
	//   기준 시각을 kInterpDelay 만큼 과거로 잡는다. 그러면 그 시점은
	//   이미 받아둔 두 좌표 사이에 있으므로, 미래를 추측할 필요 없이
	//   두 점을 잇기만 하면 된다. (추측하면 틀렸을 때 되돌아가며 떨린다)
	void net_remote_players(std::vector<RemoteView>& out)
	{
		out.clear();
		out.reserve(g_remotes.size());

		const double renderTime = NowSeconds() - kInterpDelay;

		for (std::unordered_map<uint32_t, Remote>::const_iterator it = g_remotes.begin();
			it != g_remotes.end(); ++it)
		{
			const Remote& r = it->second;
			if (r.currTime <= 0.0) continue;

			RemoteView v = {};
			v.playerId = it->first;

			const double span = r.currTime - r.prevTime;

			if (!r.hasPrev || span <= 0.0 || renderTime >= r.currTime)
			{
				// 보간할 구간이 없다 = 방금 처음 봤거나, 갱신이 끊겼다.
				// 이럴 때 계속 밀어붙이면(외삽) 벽을 뚫고 나간다. 그냥 멈춰 세운다.
				v.pos[0] = r.currPos[0];
				v.pos[1] = r.currPos[1];
				v.pos[2] = r.currPos[2];
			}
			else if (renderTime <= r.prevTime)
			{
				v.pos[0] = r.prevPos[0];
				v.pos[1] = r.prevPos[1];
				v.pos[2] = r.prevPos[2];
			}
			else
			{
				const float t = float((renderTime - r.prevTime) / span);
				v.pos[0] = r.prevPos[0] + (r.currPos[0] - r.prevPos[0]) * t;
				v.pos[1] = r.prevPos[1] + (r.currPos[1] - r.prevPos[1]) * t;
				v.pos[2] = r.prevPos[2] + (r.currPos[2] - r.prevPos[2]) * t;
			}

			out.push_back(v);
		}
	}

	unsigned net_sent_count() { return g_nSent; }
	unsigned net_echo_count() { return g_nEcho; }
	unsigned net_remote_count() { return (unsigned)g_remotes.size(); }

	void net_last_echo(float outPos[3])
	{
		outPos[0] = g_lastEcho[0];
		outPos[1] = g_lastEcho[1];
		outPos[2] = g_lastEcho[2];
	}
}
