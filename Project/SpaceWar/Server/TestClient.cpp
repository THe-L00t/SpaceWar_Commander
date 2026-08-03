// ============================================================
//  TestClient — 부하 확인용 봇
//
//  진짜 클라이언트가 아직 네트워크를 안 붙였으므로,
//  "실제 클라가 할 일" 을 흉내내서 서버 구조가 도는지 검증한다.
//
//    - 접속해서 ServerHello 를 받고
//    - 30Hz 로 PlayerInputPacket 을 보내고
//    - 서버가 보내주는 스냅샷을 받아서 개수를 센다
//
//  ★ 왜 서버와 같은 exe 에 넣었나
//    별도 프로젝트를 만들면 프로토콜이 갈릴 여지가 생긴다.
//    같은 exe 안에 두면 Shared 를 똑같이 링크하므로 크기 불일치가 원천 차단된다.
//
//  ★ 봇은 블로킹 소켓 + 스레드 1개다 (IOCP 아님)
//    이건 의도적이다. 봇이 많아지면 봇 쪽이 먼저 느려지는데,
//    그래야 "서버가 느린 건지 부하기가 느린 건지" 를 구분할 수 있다.
//    실제 클라는 봇 수백 개가 아니라 한 명이므로 이 구조로 충분하다.
// ============================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "Shared/Protocol.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

	std::atomic<uint64_t> g_snapshotsReceived{ 0 };
	std::atomic<uint64_t> g_inputsSent{ 0 };
	std::atomic<uint32_t> g_connected{ 0 };
	std::atomic<uint32_t> g_failed{ 0 };

	// 봇 한 마리의 일생
	void BotMain(const char* host, uint16_t port, int index, int seconds, std::atomic<bool>& stop)
	{
		SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET) { g_failed.fetch_add(1); return; }

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = ::htons(port);
		::inet_pton(AF_INET, host, &addr.sin_addr);

		if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			::closesocket(sock);
			g_failed.fetch_add(1);
			return;
		}

		// 실제 클라와 같은 설정 — Nagle 을 끄지 않으면 입력이 최대 40ms 밀린다
		BOOL nodelay = TRUE;
		::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

		// 수신은 논블로킹으로 — 보내기와 번갈아 해야 하므로 여기서 막히면 안 된다
		u_long nonblock = 1;
		::ioctlsocket(sock, FIONBIO, &nonblock);

		g_connected.fetch_add(1);

		uint32_t myPlayerId = 0;
		uint32_t tick = 0;
		std::vector<char> assembly;
		assembly.reserve(32 * 1024);
		char buf[16 * 1024];

		const auto period = std::chrono::microseconds(1'000'000 / Shared::kTickRateHz);
		auto nextSend = std::chrono::steady_clock::now();
		const auto endAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

		while (!stop.load() && std::chrono::steady_clock::now() < endAt)
		{
			// ── 받기 ──
			for (;;)
			{
				const int n = ::recv(sock, buf, sizeof(buf), 0);
				if (n > 0) assembly.insert(assembly.end(), buf, buf + n);
				else break;                       // 더 없음(WSAEWOULDBLOCK) 또는 끊김
			}

			// 스트림 자르기 — 서버와 똑같은 방식이다
			size_t offset = 0;
			while (assembly.size() - offset >= sizeof(Shared::PacketHeader))
			{
				const auto* head =
					reinterpret_cast<const Shared::PacketHeader*>(assembly.data() + offset);
				if (head->size < sizeof(Shared::PacketHeader) ||
					head->size > Shared::kMaxPacketSize) { offset = assembly.size(); break; }
				if (assembly.size() - offset < head->size) break;

				if (head->type == Shared::PacketType::ServerHello)
				{
					const auto* hello = reinterpret_cast<const Shared::ServerHelloPacket*>(head);
					myPlayerId = hello->playerId;
				}
				else if (head->type == Shared::PacketType::PlayerState)
				{
					g_snapshotsReceived.fetch_add(1, std::memory_order_relaxed);
				}
				offset += head->size;
			}
			if (offset > 0) assembly.erase(assembly.begin(), assembly.begin() + offset);

			// ── 보내기 (30Hz 고정) ──
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextSend && myPlayerId != 0)
			{
				nextSend += period;

				Shared::PlayerInputPacket in{};
				in.header.size = sizeof(in);
				in.header.type = Shared::PacketType::PlayerInput;
				in.playerId = myPlayerId;
				in.tick = tick++;

				// 봇마다 다른 방향으로 빙빙 돌게 한다 (전부 같은 값이면
				// 캐시가 지나치게 잘 맞아서 실제 부하보다 가볍게 나온다)
				const float phase = float(index) * 0.37f + float(tick) * 0.02f;
				in.moveX = int8_t(std::sin(phase) * 100.0f);
				in.moveZ = int8_t(std::cos(phase) * 100.0f);
				in.buttons = (tick % 90 == 0) ? Shared::Btn_Jump : 0;
				if (index % 3 == 0) in.buttons |= Shared::Btn_Sprint;
				in.aimDir[0] = std::sin(phase * 0.5f);
				in.aimDir[1] = 0.0f;
				in.aimDir[2] = std::cos(phase * 0.5f);

				if (::send(sock, reinterpret_cast<const char*>(&in), in.header.size, 0) > 0)
					g_inputsSent.fetch_add(1, std::memory_order_relaxed);
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}

		::closesocket(sock);
		g_connected.fetch_sub(1);
	}
}

// 봇 count 마리를 seconds 초 동안 돌린다.
int RunBotClients(const char* host, uint16_t port, int count, int seconds)
{
	WSADATA wsa{};
	::WSAStartup(MAKEWORD(2, 2), &wsa);

	std::printf("[bot] %s:%u 로 봇 %d개 접속 (%d초)\n", host, port, count, seconds);

	std::atomic<bool> stop{ false };
	std::vector<std::thread> bots;
	bots.reserve(count);

	for (int i = 0; i < count; ++i)
	{
		bots.emplace_back(BotMain, host, port, i, seconds, std::ref(stop));
		// 한꺼번에 붙이면 접속 대기열이 넘친다. 조금씩 흘려보낸다.
		if (i % 25 == 24) std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	uint64_t prevSent = 0, prevRecv = 0;
	for (int i = 0; i < seconds; ++i)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const uint64_t sent = g_inputsSent.load();
		const uint64_t recv = g_snapshotsReceived.load();
		std::printf("[bot ] 접속 %u  실패 %u  입력송신 %llu/s  스냅샷수신 %llu/s\n",
			g_connected.load(), g_failed.load(),
			static_cast<unsigned long long>(sent - prevSent),
			static_cast<unsigned long long>(recv - prevRecv));
		prevSent = sent; prevRecv = recv;
	}

	stop.store(true);
	for (auto& t : bots) if (t.joinable()) t.join();

	std::printf("[bot] 종료 — 총 송신 %llu / 총 수신 %llu / 접속실패 %u\n",
		static_cast<unsigned long long>(g_inputsSent.load()),
		static_cast<unsigned long long>(g_snapshotsReceived.load()),
		g_failed.load());

	::WSACleanup();
	return 0;
}
