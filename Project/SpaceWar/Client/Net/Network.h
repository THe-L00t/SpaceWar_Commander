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
#include "Shared/Protocol.h"   // PlayerStatePacket 을 값으로 들고 있는다(2026-10-29) — 이 헤더는 <cstdint> 만 쓴다
#include "Shared/World/WorldCatalog.h"   // 맵 번호(2026-11-01) — 상수만 있는 헤더

namespace Shared {
	struct WelcomePacket;
	struct PlayerMovePacket;
	struct PlayerLeavePacket;
	struct NpcStatePacket;
	struct PlayerHealthPacket;
	struct PlayerNeutralizedPacket;
	struct NpcDespawnPacket;
	struct WorldJoinedPacket;
	struct MoveCommand;
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
//  ★ 맵 번호 (2026-11-01, 사용자 «맵번호 적용해줘», Shared/World/WorldCatalog.h)
//    서버는 세션마다 맵 번호를 들고 같은 맵 사람끼리만 판정 · 송신한다. 접속하면 게임 맵(0)에 있다.
//    행성을 바꿀 때 SendJoinWorld 로 알리면, 서버의 WorldJoined 가 올 때까지(들어가는 중) 옛 맵 패킷
//    (PlayerMove · NpcState · NpcDespawn · PlayerState · PlayerNeutralized)을 버린다 — TCP 순서상 WorldJoined 뒤의 것만 새 맵 것이다.
//    입력 · 사격은 JoinedWorld() 가 지금 맵일 때만 보낸다(Engine::UpdateNetwork · UpdateFire).
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

		// ★ 2026-10-29 B안 이후 쓰지 않는다 — 서버가 좌표를 받지 않는다(무시하고 로그만 남긴다).
		//   좌표를 Shared::PlayerMovePacket 규격으로 담아 보낸다.
		void SendToServer(float x, float y, float z);

		// ★ B안(입력 기반, 2026-10-29) — 고정 틱 입력 하나를 보낸다(Shared::PlayerInputPacket).
		//   PlayerController 가 틱마다 만든 것을 Engine 이 그대로 넘긴다. 판정은 서버가 같은 이동 코드로 한다.
		void SendInput(const Shared::MoveCommand&);

		// 서버가 판정한 내 이동 상태 중 가장 최근 것(마지막 입력 번호가 가장 큰 것). 한 번 가져가면 지워진다.
		bool TakePlayerState(Shared::PlayerStatePacket& out);
		unsigned StateCount() const { return nStates; }

		// ── 맵 번호 (2026-11-01) ────────────────────────────────
		//  서버의 답 — ok 면 서버가 정한 스폰 자리 · 몸 방향.
		struct WorldJoin
		{
			uint32_t world = Shared::kNoWorld;
			bool     ok = false;
			float    pos[3] = {};
			float    facing[3] = { 0.0f, 0.0f, 1.0f };
		};

		// 이 맵으로 옮긴다고 서버에 알린다. 답이 올 때까지 «들어가는 중» — 옛 맵 패킷을 버리고 원격 목록을 비운다.
		void SendJoinWorld(uint32_t worldId);
		// 서버가 넣어 준 맵. 들어가는 중이거나 서버가 준비하지 못했으면 Shared::kNoWorld.
		uint32_t JoinedWorld() const { return joinState == JoinState::Joined ? requestedWorld : Shared::kNoWorld; }
		bool     Joining() const { return joinState == JoinState::Joining; }
		bool     JoinFailed() const { return joinState == JoinState::Failed; }
		// world 에 대한 서버의 답이 왔으면 꺼낸다(한 번 가져가면 지워진다). 다른 맵의 답이면 남겨 둔다.
		bool TakeWorldJoined(uint32_t world, WorldJoin& out);

		// ★ 좌클릭 한 발. 판정은 서버가 한다 (명세 18절 원칙 4).
		//   «어디서 어디로» 만 보낸다. 누가 맞았는지는 말하지 않는다.
		void SendFire(const float origin[3], const float direction[3]);

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

		// ── 전투 ────────────────────────────────────────────
		//  서버가 보내준 내 체력. 아직 못 받았으면 음수.
		float MyHealth() const { return myHealth; }

		// 서버가 «시작 위치로 돌아가라» 고 했는가. 한 번 가져가면 지워진다.
		// ★ 클라가 스스로 되살아나지 않는다. 좌표도 서버가 준 것을 그대로 쓴다.
		bool TakeRespawn(float outPos[3]);

		// 마지막으로 맞은 순간부터 흐른 시간을 재려고 둔 피격 횟수 (표시용).
		unsigned HitCount() const { return nHits; }

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

		// 논블로킹 소켓이라 부분 송신이 나올 수 있다. 다 보내면 true.
		bool SendRaw(const void* data, int size);

		static void PushSnapshot(Remote&, const float pos[3]);
		static void SampleAt(const Remote&, double renderTime, float out[3]);

		void ProcessPackets();
		void OnWelcome(const Shared::WelcomePacket*);
		void OnPlayerMove(const Shared::PlayerMovePacket*);
		void OnNpcState(const Shared::NpcStatePacket*);
		void OnPlayerLeave(const Shared::PlayerLeavePacket*);
		void OnPlayerHealth(const Shared::PlayerHealthPacket*);
		void OnPlayerNeutralized(const Shared::PlayerNeutralizedPacket*);
		void OnNpcDespawn(const Shared::NpcDespawnPacket*);
		void OnPlayerState(const Shared::PlayerStatePacket*);
		void OnWorldJoined(const Shared::WorldJoinedPacket*);
		void TrySendJoinWorld();       // 못 보낸 JoinWorld 를 다시 보낸다(송신 버퍼가 찼을 때)
		void ClearWorldState();        // 원격 · NPC 목록 · 받아 둔 판정 · 리스폰을 비운다(맵을 바꿀 때)

		SOCKET sock = INVALID_SOCKET;
		bool   connected = false;

		// 수신 조립 버퍼. 서버와 완전히 같은 방식으로 자른다.
		char recvBuf[8192] = {};
		int  nRecvd = 0;

		unsigned nSent = 0;
		unsigned nEcho = 0;
		float    lastEcho[3] = { 0.0f, 0.0f, 0.0f };

		uint32_t myId = 0;		// 서버가 알려준 내 번호

		// ── 전투 ────────────────────────────────────────────
		float    myHealth = -1.0f;      // 서버가 알려준 내 체력. 음수 = 아직 모름
		unsigned nHits = 0;             // 내가 맞은 횟수 (표시용)
		bool     hasRespawn = false;    // 되살아날 자리를 받았는가
		float    respawnPos[3] = { 0.0f, 0.0f, 0.0f };

		// ── 이동 (B안) ──
		Shared::PlayerStatePacket latestState{};   // 가장 최근에 받은 내 이동 상태
		bool     hasState = false;
		unsigned nStates = 0;

		// ── 맵 번호 (2026-11-01) ──
		enum class JoinState : uint8_t { Joined, Joining, Failed };
		uint32_t  requestedWorld = Shared::kWorldGameMap;   // 마지막으로 요청한 맵(접속 직후 = 게임 맵)
		JoinState joinState = JoinState::Joined;
		bool      joinUnsent = false;                       // JoinWorld 를 아직 못 보냈다
		bool      hasJoinResult = false;
		WorldJoin joinResult;

		std::unordered_map<uint32_t, Remote> remotes;

		// ★ NPC 도 같은 구조로 다룬다
		//   서버가 행동을 계산해 좌표만 보내주므로, 클라 입장에서는
		//   원격 플레이어와 구별할 이유가 없다. 보간도 그대로 쓴다.
		std::unordered_map<uint32_t, Remote> npcs;
	};
}
