// ============================================================
//  SpaceWar Server — IOCP 기반 대규모(50v50) 서버 진입점
//
//  구조 (아래에서 위로 읽으면 된다)
//
//      [ 소켓 ]
//         │  IocpServer : 접속 받기 / 스트림 자르기 / 보내기
//         │               게임 규칙을 전혀 모른다
//         ▼  완전한 패킷
//      [ main.cpp ]  ← 지금 이 파일. 두 계층을 잇는 배선판
//         │  "이 패킷은 이 경기의 이 사람 것" 으로 넘겨준다
//         ▼
//      [ MatchManager ] : 경기 여러 개를 코어에 나눠 배정
//         └ [ Match ] x N : 경기 하나(50 대 50 = 100명)의 고정 틱 시뮬레이션
//
//  스레드 구성
//      IOCP 워커 (코어 x 2) : 패킷을 받아 입력 큐에 넣기만 한다
//      틱  워커 (코어 x 1) : 30Hz 로 경기를 계산하고 스냅샷을 뿌린다
//      메인 스레드          : 통계 출력
//
//  실행 방법
//      Server.exe                 서버 시작 (기본 27015)
//      Server.exe --port 27020    포트 지정
//      Server.exe --bots 100      서버를 띄우고 봇 100개를 붙여 부하 확인
//      Server.exe --client 50     이미 떠 있는 서버에 봇 50개만 붙인다
//
//  ★ DirectX 없음. 서버는 그래픽을 모른다.
//  ★ 공용 규약은 Shared 프로젝트를 그대로 쓴다.
// ============================================================
#include <winsock2.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "Shared/Protocol.h"   // AdditionalIncludeDirectories = $(ProjectDir)..\ 덕분에 이렇게 참조
#include "Net/IocpServer.h"
#include "Game/MatchManager.h"

// TestClient.cpp — 부하 확인용 봇
extern int RunBotClients(const char* host, uint16_t port, int count, int seconds);

namespace {

	swc::IocpServer   g_server;
	swc::MatchManager g_matches;

	// 세션 -> 어느 경기 소속인가. IOCP 워커가 매 패킷마다 조회하므로 빨라야 한다.
	std::shared_mutex g_mapMutex;
	std::unordered_map<uint32_t, std::shared_ptr<swc::Match>> g_sessionMatch;

	// ── 접속 ────────────────────────────────────────────────
	void OnConnect(const std::shared_ptr<swc::Session>& s)
	{
		auto match = g_matches.FindOrCreateMatch();

		uint32_t playerId = 0;
		uint8_t  team = 0;
		if (!match->AddPlayer(s, playerId, team))
		{
			s->Close();
			return;
		}

		s->SetMatchId(match->Id());
		{
			std::unique_lock lock(g_mapMutex);
			g_sessionMatch[s->Id()] = match;
		}

		// "너는 몇 번이고, 어느 경기 어느 팀이다" 를 알려준다.
		// 클라는 이 tickRateHz 주기로 입력을 보내야 한다.
		Shared::ServerHelloPacket hello{};
		hello.header.size = sizeof(hello);
		hello.header.type = Shared::PacketType::ServerHello;
		hello.playerId = playerId;
		hello.matchId = match->Id();
		hello.tickRateHz = Shared::kTickRateHz;
		hello.team = team;
		s->Send(&hello, hello.header.size);
	}

	// ── 종료 ────────────────────────────────────────────────
	void OnDisconnect(const std::shared_ptr<swc::Session>& s)
	{
		std::shared_ptr<swc::Match> match;
		{
			std::unique_lock lock(g_mapMutex);
			auto it = g_sessionMatch.find(s->Id());
			if (it != g_sessionMatch.end()) { match = it->second; g_sessionMatch.erase(it); }
		}
		if (match) match->RemovePlayer(s->Id());
	}

	// ── 패킷 도착 (IOCP 워커 스레드에서 호출) ───────────────
	//
	//  ★ 여기서 무거운 일을 하면 안 된다
	//    이 함수가 도는 동안 그 워커 스레드는 다른 소켓을 못 본다.
	//    그래서 "어느 경기인지 찾아 큐에 넣기" 까지만 하고 즉시 빠진다.
	void OnPacket(const std::shared_ptr<swc::Session>& s, const Shared::PacketHeader* head)
	{
		switch (head->type)
		{
		case Shared::PacketType::PlayerInput:
		{
			// 크기 검증 — 조작된 패킷이 구조체 밖을 읽게 두면 안 된다
			if (head->size != sizeof(Shared::PlayerInputPacket)) { s->Close(); return; }
			const auto* in = reinterpret_cast<const Shared::PlayerInputPacket*>(head);

			std::shared_ptr<swc::Match> match;
			{
				std::shared_lock lock(g_mapMutex);
				auto it = g_sessionMatch.find(s->Id());
				if (it != g_sessionMatch.end()) match = it->second;
			}
			if (match) match->EnqueueInput(s->Id(), *in);
			break;
		}
		default:
			// 아직 처리하지 않는 종류는 무시한다 (연결을 끊지는 않는다)
			break;
		}
	}

	// ── 통계 ────────────────────────────────────────────────
	//  틱시간이 33ms(=1/30초)에 근접하면 그 코어가 포화된 것이다.
	void PrintStats()
	{
		static uint64_t prevInputs = 0;
		const uint64_t inputs = g_matches.TotalInputs();
		const uint64_t perSec = inputs - prevInputs;
		prevInputs = inputs;

		std::printf("[stat] 접속 %zu  경기 %zu  인원 %zu  입력 %llu/s  틱시간",
			g_server.SessionCount(), g_matches.MatchCount(), g_matches.TotalPlayers(),
			static_cast<unsigned long long>(perSec));

		for (size_t i = 0; i < g_matches.WorkerCount() && i < 8; ++i)
			std::printf(" %.2fms", g_matches.LastTickMs(i));
		std::printf("\n");
	}
}

int main(int argc, char** argv)
{
	uint16_t port = 27015;
	int bots = 0;
	int clientOnly = 0;
	const char* host = "127.0.0.1";

	for (int i = 1; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--port") && i + 1 < argc)        port = uint16_t(std::atoi(argv[++i]));
		else if (!std::strcmp(argv[i], "--bots") && i + 1 < argc)   bots = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--client") && i + 1 < argc) clientOnly = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--host") && i + 1 < argc)   host = argv[++i];
	}

	// 봇 전용 모드 — 이미 떠 있는 서버에 부하만 준다
	if (clientOnly > 0)
		return RunBotClients(host, port, clientOnly, 15);

	std::printf("=== SpaceWar Server (IOCP) ===\n");
	std::printf("경기당 %u명 (%u 대 %u) / 틱 %u Hz / 이 PC 코어 %u개\n",
		Shared::kPlayersPerMatch, Shared::kTeamSize, Shared::kTeamSize,
		Shared::kTickRateHz, std::thread::hardware_concurrency());

	g_server.SetConnectHandler(OnConnect);
	g_server.SetDisconnectHandler(OnDisconnect);
	g_server.SetPacketHandler(OnPacket);

	if (!g_server.Start(port))
	{
		std::printf("서버 시작 실패\n");
		return 1;
	}
	g_matches.Start();

	// 자가 테스트 — 같은 프로세스에서 봇을 붙여 배관이 도는지 확인한다
	std::thread botThread;
	if (bots > 0)
		botThread = std::thread([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		RunBotClients("127.0.0.1", port, bots, 12);
			});

	const int seconds = bots > 0 ? 15 : 3600;
	for (int i = 0; i < seconds; ++i)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		PrintStats();
	}

	if (botThread.joinable()) botThread.join();

	std::printf("종료 중...\n");
	g_matches.Stop();
	g_server.Stop();
	std::printf("종료 완료\n");
	return 0;
}
