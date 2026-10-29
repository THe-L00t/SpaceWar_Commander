#pragma once
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>
#include "Planet.h"
#include "Shared/GameLogic/PlayerMovement.h"

// 구면 위 3인칭 이동. 헬다이버즈식 관성·회전 지연은 그대로 유지하고
// 축만 월드 XZ 평면에서 접평면으로 바꾼다.
//
// ★ 각도(yaw) 대신 방향 벡터를 들고 다닌다.
//   구면에서 각도로 방향을 표현하려면 기준 방향이 필요한데
//   어떤 기준을 잡아도 극점에서 무너진다.
//
// ★ 상태는 "지형 위 고도"가 아니라 "기준구 위 고도"다.
//   지형 위 고도로 잡으면 점프 중에도 지형을 따라가서
//   포물선이 아니라 언덕에 들러붙는 점프가 된다.
//
// ★ 2026-10-29 B안(입력 기반) — 이동 계산은 Shared::PlayerMovement 로 옮겼다(서버가 같은 코드로 판정한다).
//   명세 §6.13 Player ① 로컬 예측 ② 같은 입력을 서버로 ③ 서버 결과와 비교 → 어긋나면 보정.
//   · 고정 틱(PlayerMovement::kTickSeconds = 1/60 s)으로만 Step 한다. 렌더 프레임과 분리 — 남은 시간은 다음 프레임에 넘긴다.
//   · 틱마다 MoveCommand(입력 번호 · 이동 축 · 버튼 · 카메라 방향)를 기록하고 FrameCommands() 로 내놓는다(Engine 이 서버로 보낸다).
//   · Reconcile: 서버가 돌려준 «마지막으로 처리한 입력 번호 + 상태» 가 내 기록과 kCorrectionThreshold 넘게 다르면
//     서버 상태로 되감고 그 뒤 입력을 다시 Step 한다(멀티스레딩 명세 §3.2 ② «Player 보정(되감기 · 재적용)»).
//   · Position() · Up() · Facing() · WorldMatrix() 는 «그릴 자세» — 마지막 두 틱 사이를 보간한다(144 fps 에서 60 Hz 계단이 안 보이게).
//     사격 원점처럼 판정에 가까운 곳은 SimPosition() 을 써도 된다.
//   · 충돌체(Shared::CollisionWorld)는 SetCollision 으로 받는다. 없으면 예전처럼 지형만 본다.
// ★ 명세 6.6.3 — Player 는 입력 장치를 모른다. 키 · 카메라를 MoveCommand 로 바꾸는 곳은 아직 여기(Update)다
//   (Class Bridge 가 빈 클래스라서 — 옮길 자리는 §9 Class Bridge).
namespace Shared { class CollisionWorld; }

namespace swc {
	class InputManager;
	class Camera;

	// 서버가 돌려준 내 이동 상태 (Shared::PlayerStatePacket 을 Network 가 풀어 놓은 것)
	struct ServerMoveState
	{
		uint32_t lastSeq = 0;            // 서버가 마지막으로 처리한 입력 번호(0 = 아직 없음)
		float    pos[3] = {};
		float    velocity[3] = {};
		float    facing[3] = { 0.0f, 0.0f, 1.0f };
		float    verticalSpeed = 0.0f;
		uint8_t  flags = 0;              // Shared::kMoveStateGrounded · OnObject · Sprinting
	};

	class PlayerController
	{
	public:
		static constexpr double   kCorrectionThreshold = 0.05;   // m — 서버와 이보다 벌어지면 되감는다
		static constexpr size_t   kHistory = 256;                // 입력 기록(약 4 초)
		static constexpr int      kMaxStepsPerFrame = 8;         // 한 프레임이 길어도 이만큼만 따라잡는다

		void SetPlanet(const Planet* p) { planet = p; }
		void SetCollision(const Shared::CollisionWorld* world) { collision = world; }
		void Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection);

		// 렌더 프레임마다. 쌓인 시간만큼 고정 틱을 돌리고 그릴 자세를 보간한다.
		void Update(float, const InputManager&, const Camera&);

		// 이번 Update 에서 만든 입력(서버로 보낼 것). 다음 Update 가 비운다.
		const std::vector<Shared::MoveCommand>& FrameCommands() const { return frameCommands; }

		// 서버 상태로 맞춘다. 고쳤으면 움직인 거리(m), 안 고쳤으면 0.
		double Reconcile(const ServerMoveState&);

		const Vec3d& Position() const { return renderPosition; }
		const Vec3d& SimPosition() const { return state.position; }
		const Vec3d& Facing() const { return renderFacing; }
		const Vec3d& Up() const { return renderUp; }

		float  Speed() const { return float(Length(state.velocity)); }
		double Altitude() const { return state.altitude; }
		bool   IsGrounded() const { return state.grounded; }
		bool   IsSprinting() const { return state.sprinting; }
		bool   OnObject() const { return state.onObject; }

		DirectX::XMMATRIX WorldMatrix() const;

		// ── 계측 (창 제목) ──
		uint32_t LastSequence() const { return nextSeq - 1; }
		uint32_t Corrections() const { return corrections; }
		double   LastCorrection() const { return lastCorrection; }

	private:
		struct HistoryEntry
		{
			Shared::MoveCommand command;
			Shared::MoveState   after;     // 이 입력을 Step 한 뒤의 예측 상태
		};

		Shared::MoveWorld World() const;
		void UpdateRenderPose(double alpha);

		const Planet* planet = nullptr;
		const Shared::CollisionWorld* collision = nullptr;

		Shared::MoveState state;           // 마지막 틱 뒤(예측)
		Shared::MoveState previous;        // 그 앞 틱 — 그릴 자세 보간의 시작
		double accumulator = 0.0;
		bool   jumpLatched = false;        // 틱 사이에 눌린 점프 — 다음 틱이 가져간다

		uint32_t nextSeq = 1;
		std::array<HistoryEntry, kHistory> history{};
		std::vector<Shared::MoveCommand> frameCommands;

		Vec3d renderPosition{ 0.0, 0.0, 0.0 };
		Vec3d renderFacing{ 0.0, 0.0, 1.0 };
		Vec3d renderUp{ 0.0, 1.0, 0.0 };

		uint32_t corrections = 0;
		double   lastCorrection = 0.0;
	};
}
