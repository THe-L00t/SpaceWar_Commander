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
};

// 모든 패킷 공통 헤더
struct PacketHeader {
    uint16_t   size;   // 패킷 전체 바이트 수
    PacketType type;   // 패킷 종류
};

// 예시: 플레이어 이동 패킷 (클라·서버 동일 구조)
struct PlayerMovePacket {
    PacketHeader header;
    uint32_t     playerId;
    float        pos[3];        // 구체 표면 위 위치
    float        velocity[3];
};

} // namespace Shared
