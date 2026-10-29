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
    PlayerFire,          // 클라 -> 서버 : 쐈다. 판정은 서버가 한다
    PlayerNeutralized,   // 서버 -> 클라 : 체력이 0 이 됐다 + 되살아날 자리

    Welcome,        // 서버 -> 클라 : 접속 직후 1회. 네 번호는 이것이다
    PlayerLeave,    // 서버 -> 클라 : 이 번호가 나갔다

    NpcState,       // 서버 -> 클라 : NPC 한 마리의 현재 위치

    PlayerHealth,   // 서버 -> 클라 : 이 플레이어의 남은 체력
    NpcDespawn,     // 서버 -> 클라 : 이 NPC 가 사라졌다 (쓰러짐)

    // ── B안(입력 기반, 2026-10-29) — 노션 「충돌 처리 ②」 3절. ★ 기존 번호가 안 바뀌게 끝에 붙인다 ──
    PlayerInput,    // 클라 -> 서버 : 고정 틱 하나의 입력(번호 · 이동 축 · 버튼 · 카메라 방향)
    PlayerState,    // 서버 -> 당사자 : 마지막으로 처리한 입력 번호 + 서버가 판정한 이동 상태
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

// 서버 -> 클라 : NPC 한 마리의 현재 위치.
//
// ★ 행동 계산은 서버가 한다. 클라는 받은 위치를 보간해서 그리기만 한다
//   (아키텍처 명세서 6.13 — 판정은 언제나 서버).
//   별도의 스폰/퇴장 패킷을 두지 않았다. 처음 보는 npcId 가 오면 그때 만들고,
//   원격 플레이어와 같은 방식으로 다룬다.
struct NpcStatePacket {
    PacketHeader header;
    uint32_t     npcId;
    float        pos[3];
};

// 클라 -> 서버 : 좌클릭으로 한 발 쐈다.
//
// ★ 판정은 서버가 한다 (아키텍처 명세서 18절 원칙 4)
//   클라는 «어디서 어디로 쐈는지» 만 보내고, 누가 맞았는지는 말하지 않는다.
//   서버가 저장해 둔 위치와 origin 이 너무 벌어지면 서버 위치로 바꿔 쏜다.
// ★ playerId 는 0 으로 두고 보낸다. 번호는 서버가 안다.
struct PlayerFirePacket {
    PacketHeader header;
    uint32_t     playerId;
    float        origin[3];     // 총구 위치 (지금은 몸통 중심)
    float        direction[3];  // 단위 방향 벡터
};

// 서버 -> 클라 : 이 플레이어의 남은 체력.
//
// 맞은 사람과 쏜 사람 양쪽이 알아야 하므로 전원에게 뿌린다.
// 접속 직후에도 한 번 보내 클라가 시작 체력을 알게 한다.
struct PlayerHealthPacket {
    PacketHeader header;
    uint32_t     playerId;
    uint32_t     attackerId;    // 0 = 피해가 아닌 알림(접속 직후 등)
    float        health;
};

// 서버 -> 클라 : 체력이 0 이 됐다.
//
// ★ 되살아날 자리를 같이 싣는다
//   교수님 지시가 «HP 0 이면 시작 위치로 텔레포트 + HP 만땅» 이라 쓰러진 상태가 따로 없다.
//   당사자는 이 좌표로 즉시 옮겨가고, 나머지는 그 자리에 그린다.
struct PlayerNeutralizedPacket {
    PacketHeader header;
    uint32_t     playerId;
    uint32_t     attackerId;
    float        pos[3];        // 되살아나는 위치 (서버가 정한다)
    float        health;        // 회복된 체력
};

// 서버 -> 클라 : 이 NPC 가 사라졌다.
//
// ★ 이 패킷이 없으면 쓰러진 NPC 가 화면에 영원히 서 있는다
//   클라는 마지막으로 받은 좌표를 계속 그리기 때문이다. PlayerLeave 와 같은 이유다.
//   다시 살아나면 서버가 NpcState 를 다시 보내고, 클라는 처음 보는 번호처럼 만든다.
struct NpcDespawnPacket {
    PacketHeader header;
    uint32_t     npcId;
};

// 클라 -> 서버 : 고정 틱(1/60 s) 하나의 입력. Shared::MoveCommand 를 그대로 싣는다(Shared/GameLogic/PlayerMovement.h).
//
// ★ 좌표가 아니라 입력을 보낸다 (명세 6.13 ② «같은 입력을 서버로», B안)
//   서버가 같은 이동 코드(PlayerMovement::Step) · 같은 충돌체로 판정한다. 클라가 위치를 정하지 않는다(18절 원칙 4).
// ★ 틱마다 하나씩 보낸다(초당 60개 × 24 B). seq 는 1 부터 늘어난다.
struct PlayerInputPacket {
    PacketHeader header;
    uint32_t     seq;
    int8_t       moveX;         // −1 · 0 · 1
    int8_t       moveZ;         // −1 · 0 · 1
    uint8_t      buttons;       // Shared::kMoveButton*
    uint8_t      pad;
    float        look[3];       // 카메라 앞 방향(월드, 단위)
};

// PlayerStatePacket::flags
inline constexpr uint8_t kMoveStateGrounded  = 1u << 0;
inline constexpr uint8_t kMoveStateOnObject  = 1u << 1;
inline constexpr uint8_t kMoveStateSprinting = 1u << 2;

// 서버 -> 당사자 : 서버가 판정한 내 이동 상태. 서버 틱(30 Hz)마다.
//
// ★ 클라는 lastSeq 를 처리한 뒤의 자기 예측과 비교해, 어긋났으면 이 상태로 되감고
//   그 뒤 입력을 다시 적용한다 (명세 6.13 ③ · 멀티스레딩 명세 §3.2 ②).
//   다른 사람에게는 지금처럼 PlayerMove(위치 · 속도)를 뿌린다.
struct PlayerStatePacket {
    PacketHeader header;
    uint32_t     lastSeq;       // 마지막으로 처리한 입력 번호 (0 = 아직 없음)
    float        pos[3];        // 몸통 중심(월드)
    float        velocity[3];
    float        facing[3];
    float        verticalSpeed;
    uint8_t      flags;         // kMoveState*
    uint8_t      pad[3];
};

// ── 크기 확인 ────────────────────────────────────────────────
//  구조체를 그대로 바이트로 보내므로 크기가 어긋나면 좌표가 통째로 깨진다.
//  런타임에 이상한 값이 나오는 것보다 컴파일이 실패하는 편이 낫다.
static_assert(sizeof(PacketHeader)      ==  4, "PacketHeader 크기 변경됨");
static_assert(sizeof(PlayerMovePacket)  == 32, "PlayerMovePacket 크기 변경됨");
static_assert(sizeof(WelcomePacket)     ==  8, "WelcomePacket 크기 변경됨");
static_assert(sizeof(PlayerLeavePacket) ==  8, "PlayerLeavePacket 크기 변경됨");
static_assert(sizeof(NpcStatePacket)    == 20, "NpcStatePacket 크기 변경됨");
static_assert(sizeof(PlayerFirePacket)        == 32, "PlayerFirePacket 크기 변경됨");
static_assert(sizeof(PlayerHealthPacket)      == 16, "PlayerHealthPacket 크기 변경됨");
static_assert(sizeof(PlayerNeutralizedPacket) == 28, "PlayerNeutralizedPacket 크기 변경됨");
static_assert(sizeof(NpcDespawnPacket)        ==  8, "NpcDespawnPacket 크기 변경됨");
static_assert(sizeof(PlayerInputPacket)       == 24, "PlayerInputPacket 크기 변경됨");
static_assert(sizeof(PlayerStatePacket)       == 52, "PlayerStatePacket 크기 변경됨");

} // namespace Shared
