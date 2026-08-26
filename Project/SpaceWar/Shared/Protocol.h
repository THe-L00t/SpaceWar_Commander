#pragma once
#include <cstdint>

// ============================================================
//  Shared/Protocol.h
//  클라이언트와 서버가 "똑같이" 알아야 하는 것만 여기에 둔다.
//  - DirectX, IOCP 구현, 렌더/게임플레이 전용 코드는 넣지 않는다.
//  - 여기를 한 번 고치면 클라·서버가 동시에 반영된다(단일 원본).
// ============================================================

namespace Shared {

// 네트워크 패킷 종류
enum class PacketType : uint16_t {
    None = 0,
    PlayerMove,
    PlayerFire,
    PlayerNeutralized,

    Welcome,        // 서버 -> 클라 : 접속 직후 1회. 네 번호는 이것이다
    PlayerLeave,    // 서버 -> 클라 : 이 번호가 나갔다
};

// 모든 패킷 공통 헤더
//
// ★ size 를 반드시 채워서 보내야 한다
//   패킷 종류마다 크기가 다르므로, 받는 쪽은 이 값을 보고 잘라야 한다.
//   크기를 고정으로 가정하면 종류가 섞이는 순간 스트림 전체가 어긋난다.
struct PacketHeader {
    uint16_t   size;   // 패킷 전체 바이트 수 (헤더 포함)
    PacketType type;   // 패킷 종류
};

// 예시: 플레이어 이동 패킷 (클라·서버 동일 구조)
//
// 클라 -> 서버 : playerId 는 0 으로 두고 보낸다 (번호는 서버가 안다)
// 서버 -> 클라 : 누가 움직였는지 playerId 를 채워서 전원에게 보낸다
struct PlayerMovePacket {
    PacketHeader header;
    uint32_t     playerId;
    float        pos[3];        // 구체 표면 위 위치
    float        velocity[3];
};

// 서버 -> 클라 : 접속 직후 한 번. 내 번호를 알려준다.
//
// ★ 이게 없으면 브로드캐스트를 받아도 어느 것이 나인지 구분할 수 없다.
//   내 위치까지 원격 플레이어로 그려서 큐브가 겹쳐 보이게 된다.
struct WelcomePacket {
    PacketHeader header;
    uint32_t     playerId;
};

// 서버 -> 클라 : 이 플레이어가 나갔다. 화면에서 지우라는 뜻.
struct PlayerLeavePacket {
    PacketHeader header;
    uint32_t     playerId;
};

// ── 크기 확인 ────────────────────────────────────────────────
//  구조체를 그대로 바이트로 보내므로 크기가 어긋나면 좌표가 통째로 깨진다.
//  런타임에 이상한 값이 나오는 것보다 컴파일이 실패하는 편이 낫다.
static_assert(sizeof(PacketHeader)      ==  4, "PacketHeader 크기 변경됨");
static_assert(sizeof(PlayerMovePacket)  == 32, "PlayerMovePacket 크기 변경됨");
static_assert(sizeof(WelcomePacket)     ==  8, "WelcomePacket 크기 변경됨");
static_assert(sizeof(PlayerLeavePacket) ==  8, "PlayerLeavePacket 크기 변경됨");

} // namespace Shared
