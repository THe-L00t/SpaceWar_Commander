#include "Network.h"
#include <windows.h>
#include <ws2tcpip.h>
#include <cstring>
#include "Shared/Protocol.h"

#pragma comment(lib, "ws2_32")

namespace swc {

	namespace {

		// ── 보간 지연 ───────────────────────────────────────────
		//  받은 좌표를 즉시 그리면, 다음 갱신이 올 때까지 멈춰 있다가 튄다.
		//  일부러 이만큼 과거를 그리면 항상 "받아둔 두 점 사이"를 지나가므로
		//  끊김이 사라진다. 대신 원격 플레이어가 이만큼 늦게 보인다.
		//  서버 갱신 간격(1/30초 = 33ms)보다 넉넉히 커야 한 번 늦게 와도 버틴다.
		constexpr double kInterpDelay = 0.10;   // 100ms

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

		std::wstring ToWide(const char* s)
		{
			if (!s) return L"";
			const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
			std::wstring w(n > 0 ? n - 1 : 0, L'\0');
			if (n > 0) ::MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
			return w;
		}
	}

	Network::Network() = default;

	Network::~Network()
	{
		Disconnect();
	}

	void Network::OnWelcome(const Shared::WelcomePacket* p)
	{
		myId = p->playerId;
	}

	// 새 좌표가 왔다. 직전 좌표를 prev 로 밀고 새 것을 curr 로 둔다.
	// 이 두 점 사이를 시간으로 훑는 것이 보간이다.
	void Network::PushSnapshot(Remote& r, const float pos[3])
	{
		const double now = NowSeconds();

		if (r.currTime > 0.0)
		{
			r.prevPos[0] = r.currPos[0];
			r.prevPos[1] = r.currPos[1];
			r.prevPos[2] = r.currPos[2];
			r.prevTime = r.currTime;
			r.hasPrev = true;
		}

		r.currPos[0] = pos[0];
		r.currPos[1] = pos[1];
		r.currPos[2] = pos[2];
		r.currTime = now;
	}

	void Network::OnPlayerMove(const Shared::PlayerMovePacket* p)
	{
		lastEcho[0] = p->pos[0];
		lastEcho[1] = p->pos[1];
		lastEcho[2] = p->pos[2];
		++nEcho;

		// 내 좌표가 되돌아온 것은 무시한다. 그리면 큐브가 겹치고,
		// 과거 위치로 끌려가 조작이 밀리는 것처럼 보인다.
		if (p->playerId == myId || p->playerId == 0) return;

		PushSnapshot(remotes[p->playerId], p->pos);
	}

	// 서버가 굴리는 NPC 의 위치. 처음 보는 번호면 여기서 생긴다.
	// 스폰 패킷이 따로 없는 이유다.
	void Network::OnNpcState(const Shared::NpcStatePacket* p)
	{
		if (p->npcId == 0) return;

		PushSnapshot(npcs[p->npcId], p->pos);
	}

	void Network::OnPlayerLeave(const Shared::PlayerLeavePacket* p)
	{
		remotes.erase(p->playerId);
	}

	// 서버가 판정한 체력. 클라는 규칙을 모르고 받은 값만 들고 있는다.
	void Network::OnPlayerHealth(const Shared::PlayerHealthPacket* p)
	{
		if (p->playerId != myId) return;   // 남의 체력은 아직 쓰지 않는다

		myHealth = p->health;
		if (p->attackerId != 0) ++nHits;   // 0 = 접속 직후 알림
	}

	// 체력이 0 이 됐다. 되살아날 자리는 서버가 정해 보낸다.
	void Network::OnPlayerNeutralized(const Shared::PlayerNeutralizedPacket* p)
	{
		if (p->playerId == myId)
		{
			myHealth = p->health;
			respawnPos[0] = p->pos[0];
			respawnPos[1] = p->pos[1];
			respawnPos[2] = p->pos[2];
			hasRespawn = true;
			return;
		}

		// 남이 쓰러졌다 = 그 자리로 순간이동했다. 보간 구간을 끊고 바로 옮긴다.
		Remote& r = remotes[p->playerId];
		r = Remote{};
		PushSnapshot(r, p->pos);
	}

	// ★ 이게 없으면 쓰러진 NPC 가 화면에 영원히 서 있는다.
	//   마지막으로 받은 좌표를 계속 그리기 때문이다.
	void Network::OnNpcDespawn(const Shared::NpcDespawnPacket* p)
	{
		npcs.erase(p->npcId);
	}

	bool Network::TakeRespawn(float outPos[3])
	{
		if (!hasRespawn) return false;

		outPos[0] = respawnPos[0];
		outPos[1] = respawnPos[1];
		outPos[2] = respawnPos[2];
		hasRespawn = false;
		return true;
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
	void Network::ProcessPackets()
	{
		const int nHeaderSize = (int)sizeof(Shared::PacketHeader);
		int nOffset = 0;

		while (nRecvd - nOffset >= nHeaderSize)
		{
			const Shared::PacketHeader* pHead =
				(const Shared::PacketHeader*)(recvBuf + nOffset);
			const int nSize = (int)pHead->size;

			// 규격에 없는 크기 = 스트림이 어긋났다. 이어 읽어도 복구되지 않는다.
			if (nSize < nHeaderSize || nSize >(int)sizeof(recvBuf))
			{
				nRecvd = 0;
				return;
			}

			// 아직 다 안 왔다. 다음 수신 때 이어서 처리한다.
			if (nRecvd - nOffset < nSize)
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

			case Shared::PacketType::NpcState:
				if (nSize == (int)sizeof(Shared::NpcStatePacket))
					OnNpcState((const Shared::NpcStatePacket*)pHead);
				break;

			case Shared::PacketType::PlayerHealth:
				if (nSize == (int)sizeof(Shared::PlayerHealthPacket))
					OnPlayerHealth((const Shared::PlayerHealthPacket*)pHead);
				break;

			case Shared::PacketType::PlayerNeutralized:
				if (nSize == (int)sizeof(Shared::PlayerNeutralizedPacket))
					OnPlayerNeutralized((const Shared::PlayerNeutralizedPacket*)pHead);
				break;

			case Shared::PacketType::NpcDespawn:
				if (nSize == (int)sizeof(Shared::NpcDespawnPacket))
					OnNpcDespawn((const Shared::NpcDespawnPacket*)pHead);
				break;

			default:
				break;		// 모르는 종류는 크기만큼 건너뛴다
			}

			nOffset += nSize;
		}

		// 처리하고 남은 자투리를 앞으로 당겨둔다. 다음 수신 때 이어붙는다.
		nRecvd -= nOffset;
		if (nRecvd > 0 && nOffset > 0)
			::memmove(recvBuf, recvBuf + nOffset, nRecvd);
	}

	// ── 접속 ────────────────────────────────────────────────
	bool Network::Connect(const char* host, unsigned short port, std::wstring& error)
	{
		WSADATA wsa = { 0 };
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			error = L"WSAStartup 실패";
			return false;
		}

		sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
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
			::closesocket(sock); sock = INVALID_SOCKET;
			return false;
		}

		// 접속은 한 번뿐이고 게임 시작 전이라 잠깐 멈춰도 된다. 그래서 블로킹.
		if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			error = L"접속 실패 (서버가 켜져 있는지 확인). WSA="
				+ std::to_wstring(::WSAGetLastError());
			::closesocket(sock); sock = INVALID_SOCKET;
			return false;
		}

		// ★ Nagle 끄기
		//   1/30초마다 32바이트씩 보내는데, Nagle 이 켜져 있으면
		//   작은 패킷을 모으느라 최대 40ms 를 붙잡아 둔다.
		BOOL nodelay = TRUE;
		::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
			(const char*)&nodelay, sizeof(nodelay));

		// ★ 논블로킹으로 전환
		//   이제부터 recv 는 받을 게 없으면 즉시 돌아온다. 렌더 루프가 안 멈춘다.
		u_long nonblock = 1;
		::ioctlsocket(sock, FIONBIO, &nonblock);

		nRecvd = 0;
		nSent = 0;
		nEcho = 0;
		myId = 0;
		myHealth = -1.0f;
		nHits = 0;
		hasRespawn = false;
		remotes.clear();
		npcs.clear();
		connected = true;
		return true;
	}

	void Network::Disconnect()
	{
		if (sock != INVALID_SOCKET)
		{
			::shutdown(sock, SD_BOTH);
			::closesocket(sock);
			sock = INVALID_SOCKET;
		}
		if (connected)
		{
			connected = false;
			::WSACleanup();
		}
	}

	// ── 좌표 전송 ───────────────────────────────────────────
	//
	//  ★ 렌더 루프에서 1/30초마다 호출한다.
	//    구조체를 그대로 바이트로 보내므로, 서버와 같은 헤더(Shared/Protocol.h)를
	//    쓰는 한 크기와 순서가 어긋날 수 없다.
	void Network::SendToServer(float x, float y, float z)
	{
		if (!connected) return;

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

		if (SendRaw(&pkt, (int)sizeof(pkt)))
			++nSent;
	}

	// ── 사격 ────────────────────────────────────────────────
	//
	//  ★ «맞췄다» 가 아니라 «쐈다» 를 보낸다
	//    누가 맞았는지는 서버가 정한다 (명세 18절 원칙 4). 클라가 대상을 정해 보내면
	//    조작된 클라가 아무나 죽일 수 있다.
	void Network::SendFire(const float origin[3], const float direction[3])
	{
		if (!connected) return;

		Shared::PlayerFirePacket pkt = {};
		pkt.header.size = (uint16_t)sizeof(pkt);
		pkt.header.type = Shared::PacketType::PlayerFire;
		pkt.playerId = 0;              // 번호는 서버가 붙인다
		pkt.origin[0] = origin[0];
		pkt.origin[1] = origin[1];
		pkt.origin[2] = origin[2];
		pkt.direction[0] = direction[0];
		pkt.direction[1] = direction[1];
		pkt.direction[2] = direction[2];

		SendRaw(&pkt, (int)sizeof(pkt));
	}

	// ★ 논블로킹 소켓이라 send 가 일부만 보낼 수 있다.
	//   보낸 만큼 빼고 남은 것을 이어서 보낸다.
	//   (수십 바이트라 사실상 한 번에 나가지만, 안 하면 언젠가 패킷이 깨진다)
	bool Network::SendRaw(const void* data, int size)
	{
		const char* p = (const char*)data;
		int nSentBytes = 0;

		while (nSentBytes < size)
		{
			const int n = ::send(sock, p + nSentBytes, size - nSentBytes, 0);
			if (n > 0) { nSentBytes += n; continue; }

			if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK)
			{
				// 커널 송신 버퍼가 찼다. 이 패킷은 버린다 (다음 좌표가 곧 온다).
				return false;
			}
			connected = false;   // 진짜 오류 = 연결이 끊겼다
			return false;
		}
		return true;
	}

	// ── 수신 ────────────────────────────────────────────────
	//  매 프레임 호출한다. 받을 게 없으면 즉시 돌아온다.
	void Network::Poll()
	{
		if (!connected) return;

		for (;;)
		{
			const int nSpace = (int)sizeof(recvBuf) - nRecvd;
			if (nSpace <= 0) { nRecvd = 0; break; }   // 방어: 해석 못 하는 쓰레기가 찼다

			const int n = ::recv(sock, recvBuf + nRecvd, nSpace, 0);

			if (n > 0) { nRecvd += n; continue; }     // 더 있는지 계속 읽는다
			if (n == 0) { connected = false; break; } // 서버가 정상 종료

			if (::WSAGetLastError() == WSAEWOULDBLOCK) break;   // 지금은 없다. 정상.
			connected = false;
			break;
		}

		ProcessPackets();
	}

	// ── 원격 플레이어 ───────────────────────────────────────
	//
	// ★ 지금 그려야 할 위치를 시간 보간으로 만들어 낸다.
	//
	//   기준 시각을 kInterpDelay 만큼 과거로 잡는다. 그러면 그 시점은
	//   이미 받아둔 두 좌표 사이에 있으므로, 미래를 추측할 필요 없이
	//   두 점을 잇기만 하면 된다. (추측하면 틀렸을 때 되돌아가며 떨린다)
	//
	// 받아둔 두 좌표 사이에서 renderTime 시점의 위치를 뽑는다.
	// 원격 플레이어와 NPC 가 같은 것을 쓴다.
	void Network::SampleAt(const Remote& r, double renderTime, float out[3])
	{
		const double span = r.currTime - r.prevTime;

		if (!r.hasPrev || span <= 0.0 || renderTime >= r.currTime)
		{
			// 보간할 구간이 없다 = 방금 처음 봤거나, 갱신이 끊겼다.
			// 이럴 때 계속 밀어붙이면(외삽) 벽을 뚫고 나간다. 그냥 멈춰 세운다.
			out[0] = r.currPos[0];
			out[1] = r.currPos[1];
			out[2] = r.currPos[2];
		}
		else if (renderTime <= r.prevTime)
		{
			out[0] = r.prevPos[0];
			out[1] = r.prevPos[1];
			out[2] = r.prevPos[2];
		}
		else
		{
			const float t = float((renderTime - r.prevTime) / span);
			out[0] = r.prevPos[0] + (r.currPos[0] - r.prevPos[0]) * t;
			out[1] = r.prevPos[1] + (r.currPos[1] - r.prevPos[1]) * t;
			out[2] = r.prevPos[2] + (r.currPos[2] - r.prevPos[2]) * t;
		}
	}

	void Network::RemotePlayers(std::vector<RemoteView>& out) const
	{
		out.clear();
		out.reserve(remotes.size());

		const double renderTime = NowSeconds() - kInterpDelay;

		for (std::unordered_map<uint32_t, Remote>::const_iterator it = remotes.begin();
			it != remotes.end(); ++it)
		{
			if (it->second.currTime <= 0.0) continue;

			RemoteView v = {};
			v.playerId = it->first;
			SampleAt(it->second, renderTime, v.pos);
			out.push_back(v);
		}
	}

	// NPC. 처리가 원격 플레이어와 똑같다 — 서버가 좌표만 뿌려주기 때문이다.
	void Network::Npcs(std::vector<NpcView>& out) const
	{
		out.clear();
		out.reserve(npcs.size());

		const double renderTime = NowSeconds() - kInterpDelay;

		for (std::unordered_map<uint32_t, Remote>::const_iterator it = npcs.begin();
			it != npcs.end(); ++it)
		{
			if (it->second.currTime <= 0.0) continue;

			NpcView v = {};
			v.npcId = it->first;
			SampleAt(it->second, renderTime, v.pos);
			out.push_back(v);
		}
	}

	void Network::LastEcho(float outPos[3]) const
	{
		outPos[0] = lastEcho[0];
		outPos[1] = lastEcho[1];
		outPos[2] = lastEcho[2];
	}
}
