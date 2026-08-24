#pragma once
// ★ 이 헤더를 windows.h 보다 먼저 include 해야 한다.
//   windows.h 는 기본으로 winsock 1.1 을 끌어오는데, 그 뒤에 winsock2.h 가 오면
//   sockaddr / fd_set / timeval 이 전부 재정의돼서 컴파일이 터진다.
//   winsock2.h 가 먼저 오면 _WINSOCKAPI_ 를 정의하므로 windows.h 가 건너뛴다.
#include <winsock2.h>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================
//  SimpleNet — 클라이언트 네트워크 (멀티플레이어 단계)
//
//  렌더 루프에서 1/30초마다 send_to_server(x, y, z) 로 좌표를 보내고,
//  서버가 전원에게 뿌리는 다른 플레이어의 좌표를 net_poll() 로 받는다.
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

	// 접속. 실패하면 false 이고 사유는 error 에 담긴다.
	bool net_connect(const char* host, unsigned short port, std::wstring& error);
	void net_disconnect();
	bool net_connected();

	// ★ 렌더 루프에서 1/30초마다 호출한다.
	//   좌표를 Shared::PlayerMovePacket 규격으로 담아 보낸다.
	void send_to_server(float x, float y, float z);

	// 서버가 보낸 것을 받아 해석한다. 매 프레임 호출한다.
	void net_poll();

	// 서버가 접속 직후 알려준 내 번호. 아직 못 받았으면 0.
	uint32_t net_my_id();

	// ★ 지금 그려야 할 원격 플레이어 목록을 채운다. 매 프레임 호출한다.
	//   서버 갱신은 1/30초인데 렌더는 그보다 훨씬 빠르므로,
	//   받은 좌표를 그대로 쓰면 초당 30번 뚝뚝 끊겨 보인다.
	//   여기서 두 스냅샷 사이를 시간으로 보간해 부드럽게 만든다.
	void net_remote_players(std::vector<RemoteView>& out);

	// 확인용 통계
	unsigned net_sent_count();
	unsigned net_echo_count();
	void     net_last_echo(float outPos[3]);
	unsigned net_remote_count();
}
