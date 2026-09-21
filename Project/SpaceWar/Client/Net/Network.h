#pragma once
// ★ 이 헤더를 windows.h 보다 먼저 include 해야 한다.
//   windows.h 는 기본으로 winsock 1.1 을 끌어오는데, 그 뒤에 winsock2.h 가 오면
//   sockaddr / fd_set / timeval 이 전부 재정의돼서 컴파일이 터진다.
//   winsock2.h 가 먼저 오면 _WINSOCKAPI_ 를 정의하므로 windows.h 가 건너뛴다.
#include <winsock2.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Shared {
	struct WelcomePacket;
	struct PlayerMovePacket;
	struct PlayerLeavePacket;
	struct NpcStatePacket;
}

// ============================================================
//  Client/Net/Network.h — 아키텍처 명세서 10절
//
//  서버와의 통신 — 연결 · 송신 · 수신 · 패킷 처리.
//  렌더 루프에서 1/30초마다 SendToServer(x, y, z) 로 좌표를 보내고,
//  서버가 전원에게 뿌리는 다른 플레이어와 NPC 의 좌표를 Poll() 로 받는다.
//
//  ★ 이름을 노션 기준으로 맞췄다 (SimpleNet → Network, 2026-09-21). 명세서 1·10절.
//    10절의 Socket 은 아직 따로 클래스로 만들지 않았다. SOCKET 핸들을 여기서 직접 든다.
//  ★ 실행 위치 — 지금은 메인 스레드에서 논블로킹으로 폴링한다 (멀티스레딩 10장 M0).
//    목표는 네트워크 스레드(수신 · 송신 큐 비우기) + 메인(소비)이다 (「멀티스레딩 분류 명세서」 4·5장, M4).
//  ★ 원격 플레이어 · NPC 보간은 Network 몫이 아니다 (멀티스레딩 5장 «OtherPlayer 보간 — 메인», 명세 6.13).
//    옮길 자리(OtherPlayer · NPC 객체)가 아직 없어 여기 남겨 두었다.
//
//  ★ 클라는 IOCP 를 쓰지 않는다
//    서버는 접속자 수백 명을 스레드 몇 개로 감당해야 하니 IOCP 가 필요하지만,
//    클라는 소켓이 하나뿐이라 얻을 게 없다.
//    대신 소켓을 논블로킹으로 두어 렌더 루프가 절대 멈추지 않게 한다.
//    (블로킹 recv 를 렌더 루프에서 부르면 패킷이 안 올 때 화면이 멈춘다)
// ============================================================

namespace swc {

	// 원격 플레이어 한 명의, 지금 이 순간 그려야 할 위치.
	struct RemoteView
	{
		uint32_t playerId;
		float    pos[3];
	};

	// 서버가 굴리는 NPC 한 마리의, 지금 이 순간 그려야 할 위치.
	//
	// ★ 행동 계산은 전부 서버가 한다. 클라는 받은 좌표를 보간해 그리기만 한다.
	//   그래서 원격 플레이어와 처리 경로가 똑같다.
	struct NpcView
	{
		uint32_t npcId;
		float    pos[3];
	};

	class Network
	{
	public:
		Network();
		~Network();

		// 접속. 실패하면 false 이고 사유는 error 에 담긴다.
		bool Connect(const char* host, unsigned short port, std::wstring& error);
		void Disconnect();
		bool Connected() const { return connected; }

		// ★ 렌더 루프에서 1/30초마다 호출한다.
		//   좌표를 Shared::PlayerMovePacket 규격으로 담아 보낸다.
		void SendToServer(float x, float y, float z);

		// 서버가 보낸 것을 받아 해석한다. 매 프레임 호출한다.
		void Poll();

		// 서버가 접속 직후 알려준 내 번호. 아직 못 받았으면 0.
		uint32_t MyId() const { return myId; }

		// ★ 지금 그려야 할 원격 플레이어 목록을 채운다. 매 프레임 호출한다.
		//   서버 갱신은 1/30초인데 렌더는 그보다 훨씬 빠르므로,
		//   받은 좌표를 그대로 쓰면 초당 30번 뚝뚝 끊겨 보인다.
		//   여기서 두 스냅샷 사이를 시간으로 보간해 부드럽게 만든다.
		void RemotePlayers(std::vector<RemoteView>& out) const;

		// ★ 지금 그려야 할 NPC 목록을 채운다. 매 프레임 호출한다.
		//   원격 플레이어와 같은 시간 보간을 쓴다.
		void Npcs(std::vector<NpcView>& out) const;

		// 확인용 통계
		unsigned SentCount() const { return nSent; }
		unsigned EchoCount() const { return nEcho; }
		void     LastEcho(float outPos[3]) const;
		unsigned RemoteCount() const { return (unsigned)remotes.size(); }
		unsigned NpcCount() const { return (unsigned)npcs.size(); }

	private:
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

		static void PushSnapshot(Remote&, const float pos[3]);
		static void SampleAt(const Remote&, double renderTime, float out[3]);

		void ProcessPackets();
		void OnWelcome(const Shared::WelcomePacket*);
		void OnPlayerMove(const Shared::PlayerMovePacket*);
		void OnNpcState(const Shared::NpcStatePacket*);
		void OnPlayerLeave(const Shared::PlayerLeavePacket*);

		SOCKET sock = INVALID_SOCKET;
		bool   connected = false;

		// 수신 조립 버퍼. 서버와 완전히 같은 방식으로 자른다.
		char recvBuf[8192] = {};
		int  nRecvd = 0;

		unsigned nSent = 0;
		unsigned nEcho = 0;
		float    lastEcho[3] = { 0.0f, 0.0f, 0.0f };

		uint32_t myId = 0;		// 서버가 알려준 내 번호

		std::unordered_map<uint32_t, Remote> remotes;

		// ★ NPC 도 같은 구조로 다룬다
		//   서버가 행동을 계산해 좌표만 보내주므로, 클라 입장에서는
		//   원격 플레이어와 구별할 이유가 없다. 보간도 그대로 쓴다.
		std::unordered_map<uint32_t, Remote> npcs;
	};
}
