#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstdint>
#include "../Vec3d.h"
#include "../PlanetConst.h"

// ============================================================
//  Shared/GameLogic/PlayerMovement.h — 플레이어 이동 한 틱 (2026-10-29)
//
//  클라 예측과 서버 판정이 «같은 코드 · 같은 입력 · 같은 dt» 로 돌리는 이동 계산.
//  근거: 명세 §6.13 Player(① 로컬 예측 ② 같은 입력을 서버로 ③ 서버 판정 → 어긋나면 보정) · §13(클라 Prediction / 서버 Authoritative) ·
//        §18 원칙 4, 노션 「충돌 처리 ②」 B안(입력 기반) 단계 C1 · C2 · C4 · C5.
//  사용자 지시(2026-10-29): «B안으로 전면수정해서 나중을 준비하고 싶어» · «서버 키고 움직여도 충돌에로 인한 전진 불가나 위치 이동이 없는데?».
//
//  ★ 계산은 Client/PlayerController.cpp 의 ①~⑨ 를 그대로 옮겼다(헬다이버즈식 관성 · 회전 지연, 기준구 위 고도, 스텝다운).
//    바뀐 것: 입력이 키보드 · 카메라가 아니라 MoveCommand(명세 6.6.3 — Player 는 입력 장치를 모른다),
//    ⑧ 뒤에 충돌체 밀어내기(Shared::CollisionWorld · 캡슐) · 벽 미끄러짐 · 오브젝트 윗면에 서기를 더했다.
//  ★ 고정 틱 kTickSeconds(1/60 s) 로만 부른다 — 클라와 서버가 같은 dt 여야 같은 결과가 나온다(멀티스레딩 명세 §12 «게임 틱: 고정 틱 유력»).
//  ★ 캡슐: 반지름 kCapsuleRadius 0.4 m · 키 kCapsuleHeight 1.8 m(발 = 몸통 중심 − kGroundOffset).
//    노션 「충돌 처리 ③」 측정이 쓴 가정과 같다. 지형은 고도(PlanetSurface · TerrainSampler)로, 오브젝트는 캡슐로 막는다.
// ============================================================

namespace Shared {

	class TerrainSampler;
	class CollisionWorld;

	inline constexpr uint8_t kMoveButtonSprint = 1u << 0;
	inline constexpr uint8_t kMoveButtonJump   = 1u << 1;   // 이 틱에 눌렀다(누르고 있는 것이 아니라)
	inline constexpr uint8_t kMoveButtonAim    = 1u << 2;

	// 고정 틱 하나의 입력 — 클라가 만들어 예측에 쓰고, 그대로 서버에 보낸다(Protocol.h PlayerInputPacket).
	struct MoveCommand
	{
		uint32_t seq = 0;                          // 입력 번호. 1 부터 늘어난다(0 = 없음)
		int8_t   moveX = 0;                        // +1 = 오른쪽(D) · −1 = 왼쪽(A)
		int8_t   moveZ = 0;                        // +1 = 앞(W) · −1 = 뒤(S)
		uint8_t  buttons = 0;                      // kMoveButton*
		float    look[3] = { 0.0f, 0.0f, 1.0f };   // 카메라 앞 방향(월드, 단위) — 이동 기준 · 조준 중 몸 방향
	};

	// 이동 상태 — 서버가 권위를 가진다. 클라는 예측본을 들고 서버 값으로 고친다.
	struct MoveState
	{
		Vec3d  position{ 0.0, 0.0, 0.0 };   // 몸통 중심(월드)
		Vec3d  velocity{ 0.0, 0.0, 0.0 };   // 접평면 속도(항상 up 과 수직)
		Vec3d  facing{ 0.0, 0.0, 1.0 };     // 몸이 향한 방향(접평면 단위 벡터)
		Vec3d  up{ 0.0, 1.0, 0.0 };
		double altitude = 0.0;              // 기준구 위 고도
		double verticalSpeed = 0.0;
		bool   grounded = true;
		bool   onObject = false;            // 지형이 아니라 오브젝트 윗면에 서 있다
		bool   sprinting = false;
	};

	// 이동이 보는 세계 — 행성 기하 · 지형 · 충돌체. 클라 Planet 과 같은 규약(Client/Planet.h).
	struct MoveWorld
	{
		Vec3d  center{ kPlanetCenterX, kPlanetCenterY, kPlanetCenterZ };
		double radius = kPlanetRadius;
		double gravity = kPlanetGravity;
		const TerrainSampler* terrain = nullptr;     // 없으면 높이 0 인 지면
		const CollisionWorld* collision = nullptr;   // 없으면 오브젝트에 막히지 않는다. 맵 공간 원점 = center

		Vec3d  Up(const Vec3d& position) const;
		double Altitude(const Vec3d& position) const;
		double SurfaceHeight(const Vec3d& surfacePoint) const;
		Vec3d  PositionAt(const Vec3d& surfacePoint, double altitude) const;
	};

	class PlayerMovement
	{
	public:
		static constexpr double kTickSeconds = 1.0 / 60.0;
		static constexpr float  kCapsuleRadius = 0.4f;
		static constexpr float  kCapsuleHeight = 1.8f;

		// 월드 위치 위의 지면에 세운다(높이는 지형에서 잰다). 클라 · 서버 스폰 · 되살아나기가 같은 것을 쓴다.
		static void Spawn(MoveState& state, const MoveWorld& world, const Vec3d& worldPosition, const Vec3d& facingDirection);

		// 고정 틱 하나 — dt 는 kTickSeconds 를 넘긴다.
		static void Step(MoveState& state, const MoveCommand& command, double dt, const MoveWorld& world);
	};

} // namespace Shared
