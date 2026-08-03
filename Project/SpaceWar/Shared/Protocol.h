#pragma once
#include <cstdint>

// ============================================================
//  Shared/Protocol.h
//  클라이언트와 서버가 "똑같이" 알아야 하는 것만 여기에 둔다.
//  - DirectX, IOCP 구현, 렌더/게임플레이 전용 코드는 넣지 않는다.
//  - 여기를 한 번 고치면 클라·서버가 동시에 반영된다(단일 원본).
// ============================================================

namespace Shared {

// ── 서버 틱 ──────────────────────────────────────────────────
//  클라와 서버가 반드시 같은 값을 써야 한다.
//  다르면 같은 입력에도 다른 위치가 나와서 예측·보정이 성립하지 않는다.
inline constexpr uint32_t kTickRateHz = 30;
inline constexpr float    kTickSeconds = 1.0f / float(kTickRateHz);

// 네트워크 패킷 종류
enum class PacketType : uint16_t {
    None = 0,
    PlayerMove,           // (구) 위치 직접 전송. PlayerInput/PlayerState 로 대체 예정
    PlayerFire,
    PlayerNeutralized,

    ServerHello,          // 서버 -> 클라 : 접속 직후 1회. 내 playerId 를 알려준다
    PlayerInput,          // 클라 -> 서버 : 이번 틱에 무엇을 눌렀는가
    PlayerState,          // 서버 -> 클라 : 그 입력을 반영한 결과 위치
    PlayerLeave,          // 서버 -> 클라 : 누가 나갔다
};

// 모든 패킷 공통 헤더
//
// ★ size 가 왜 필요한가
//   TCP 는 "스트림"이라 패킷 경계가 없다. send 를 3번 해도 recv 가 한 번에
//   다 받을 수 있고, 반대로 패킷 하나가 두 조각으로 나뉘어 올 수도 있다.
//   받는 쪽은 이 size 를 보고 직접 잘라야 한다.
struct PacketHeader {
    uint16_t   size;   // 패킷 전체 바이트 수 (헤더 포함)
    PacketType type;   // 패킷 종류
};

// 예시: 플레이어 이동 패킷 (클라·서버 동일 구조)
struct PlayerMovePacket {
    PacketHeader header;
    uint32_t     playerId;
    float        pos[3];        // 구체 표면 위 위치
    float        velocity[3];
};

// ── 서버 권위 이동용 패킷 ────────────────────────────────────

// 버튼 비트. bool 을 여러 개 두는 대신 1바이트에 몰아넣는다.
enum MoveButton : uint8_t {
    Btn_Jump   = 1 << 0,
    Btn_Sprint = 1 << 1,
    Btn_Aim    = 1 << 2,
};

// 접속 직후 서버가 딱 한 번 보낸다.
struct ServerHelloPacket {
    PacketHeader header;
    uint32_t     playerId;      // 이제부터 너는 이 번호다
    uint32_t     matchId;       // 배정된 경기
    uint32_t     tickRateHz;    // 서버 틱 주기. 클라가 이 주기로 입력을 보내야 한다
    uint8_t      team;          // 0 / 1  (50 대 50)
    uint8_t      reserved[3];
};

// 클라 -> 서버 : "이 틱에 이걸 눌렀다"
//
// ★ 위치가 아니라 "입력"을 보내는 이유
//   위치를 보내면 클라가 보낸 값을 서버가 그대로 믿어야 한다 = 순간이동 핵.
//   입력만 보내면 실제 위치는 서버가 계산하므로 조작이 불가능하다.
//
// ★ aimDir 이 왜 입력인가
//   우리 이동은 카메라 상대(전진 = 카메라가 보는 쪽)다. 서버엔 카메라가 없으므로
//   클라가 보는 방향을 입력의 일부로 함께 보내야 이동 축을 잡을 수 있다.
struct PlayerInputPacket {
    PacketHeader header;
    uint32_t     playerId;
    uint32_t     tick;          // 몇 번째 틱의 입력인가. 서버 보정의 기준점
    int8_t       moveX;         // -100 ~ +100 (A/D). float 대신 정수로 보내 크기를 줄인다
    int8_t       moveZ;         // -100 ~ +100 (S/W)
    uint8_t      buttons;       // MoveButton 비트합
    uint8_t      reserved;      // 정렬용 여유 1바이트 (패딩을 명시적으로 드러낸다)
    float        aimDir[3];     // 카메라 전방 (접평면 단위벡터)
};

// 한 명분 상태. 패킷이 아니라 "알맹이"다 — 스냅샷 뒤에 여러 개가 줄줄이 붙는다.
//
// ★ 위치만 보내지 않는 이유
//   위치만 맞추면 다음 틱부터 다시 어긋난다. 클라가 이 지점에서 계산을
//   이어가려면 속도·고도·수직속도·접지여부까지 시뮬레이션 상태 전체가 필요하다.
struct PlayerStateEntry {
    uint32_t playerId;
    float    pos[3];
    float    vel[3];
    float    facing[3];
    float    altitude;
    float    verticalSpeed;
    uint8_t  grounded;
    uint8_t  reserved[3];
};

// 서버 -> 클라 : 한 틱분 상태를 "한 패킷에 몰아서" 보낸다.
//
// ★ 왜 한 명씩 안 보내고 묶는가 — 50 대 50 이면 이게 필수다
//   한 경기 100명이 서로의 상태를 30Hz 로 받으면
//       100명 x 100명 x 30회 = 초당 30만 패킷
//   묶어서 보내면 100명 x 30회 = 초당 3천 패킷. 100배 차이다.
//   패킷 하나당 커널 진입 비용이 붙으므로 개수가 곧 CPU 다.
//
// ★ 가변 길이다
//   이 구조체 바로 뒤에 PlayerStateEntry 가 count 개 이어서 붙는다.
//   header.size = sizeof(WorldSnapshotHeader) + count * sizeof(PlayerStateEntry)
//
// ★ 나중에 AOI(주변만 보내기)를 끼우는 자리도 여기다
//   지금은 전원을 담지만, 시야 밖 플레이어를 빼면 count 가 줄어들 뿐
//   구조는 그대로다.
struct WorldSnapshotHeader {
    PacketHeader header;
    uint32_t     tick;          // 어느 틱의 결과인가
    uint32_t     ackTick;       // ★ 받는 사람의 입력을 어디까지 반영했는가 (보정 기준)
    uint16_t     count;         // 뒤에 붙은 PlayerStateEntry 개수
    uint16_t     reserved;
};

// 서버 -> 클라 : 누군가 접속을 끊었다
struct PlayerLeavePacket {
    PacketHeader header;
    uint32_t     playerId;
};

// ── 크기 고정 확인 ───────────────────────────────────────────
//  구조체를 그대로 바이트로 보내므로 크기가 어긋나면 좌표가 통째로 깨진다.
//  런타임에 이상한 값이 나오는 것보다 컴파일이 실패하는 편이 백 배 낫다.
//  멤버를 추가하면 이 숫자도 같이 고쳐야 한다.
static_assert(sizeof(PacketHeader)        ==  4, "PacketHeader 크기 변경됨");
static_assert(sizeof(PlayerMovePacket)    == 32, "PlayerMovePacket 크기 변경됨");
static_assert(sizeof(ServerHelloPacket)   == 20, "ServerHelloPacket 크기 변경됨");
static_assert(sizeof(PlayerInputPacket)   == 28, "PlayerInputPacket 크기 변경됨");
static_assert(sizeof(PlayerStateEntry)    == 52, "PlayerStateEntry 크기 변경됨");
static_assert(sizeof(WorldSnapshotHeader) == 16, "WorldSnapshotHeader 크기 변경됨");
static_assert(sizeof(PlayerLeavePacket)   ==  8, "PlayerLeavePacket 크기 변경됨");

// ── 경기 규모 ────────────────────────────────────────────────
inline constexpr uint32_t kTeamSize       = 50;                 // 한 팀
inline constexpr uint32_t kPlayersPerMatch = kTeamSize * 2;      // 50 대 50 = 100명

// 100명 전원을 담은 스냅샷 = 16 + 100*52 = 5216 바이트.
// 여유를 둬서 8KB 로 잡는다. 이보다 큰 패킷은 조작으로 보고 연결을 끊는다.
inline constexpr uint16_t kMaxPacketSize = 8192;

} // namespace Shared
