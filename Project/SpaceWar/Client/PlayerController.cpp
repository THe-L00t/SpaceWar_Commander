#include "PlayerController.h"
#include "InputManager.h"
#include "Camera.h"
#include "Shared/PlanetConst.h"
#include "Shared/Protocol.h"
#include <cmath>

using namespace DirectX;

// 이동 계산(①~⑨ · 튜닝 값 · 스텝다운 · 충돌)은 Shared/GameLogic/PlayerMovement.cpp 로 옮겼다(2026-10-29 B안).
// 여기는 키 · 카메라 → 입력 명령, 고정 틱, 입력 기록, 서버 보정, 그릴 자세 보간만 한다.

namespace swc {

	Shared::MoveWorld PlayerController::World() const
	{
		Shared::MoveWorld world;
		if (planet)
		{
			world.center = planet->center;
			world.radius = planet->radius;
			world.gravity = planet->gravity;
			world.terrain = planet->terrain;
		}
		world.collision = collision;
		return world;
	}

	void PlayerController::Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection)
	{
		Shared::PlayerMovement::Spawn(state, World(), worldPosition, facingDirection);
		previous = state;
		accumulator = 0.0;
		jumpLatched = false;
		// 되살아나기 · 맵 바꾸기 — 앞의 기록으로는 되감지 않는다(입력 번호는 이어 간다)
		for (HistoryEntry& entry : history) entry.command.seq = 0;
		UpdateRenderPose(1.0);
	}

	void PlayerController::Update(float dt, const InputManager& input, const Camera& camera)
	{
		frameCommands.clear();
		if (dt <= 0.0f || !planet) return;

		// 점프는 «누른 순간» 이다 — 이번 프레임에 틱이 없으면 다음 틱까지 들고 간다
		if (input.WasPressed(VK_SPACE)) jumpLatched = true;

		const Shared::MoveWorld world = World();
		const double tick = Shared::PlayerMovement::kTickSeconds;
		accumulator += double(dt);
		int steps = 0;
		while (accumulator >= tick)
		{
			if (++steps > kMaxStepsPerFrame)
			{
				accumulator = 0.0;   // 너무 길게 멈췄다 — 따라잡지 않고 버린다
				break;
			}
			accumulator -= tick;

			// ② 카메라 기준 입력 → 입력 명령 (명세 6.6.3 — 이동 계산은 키를 모른다)
			Shared::MoveCommand command;
			command.seq = nextSeq++;
			command.moveX = int8_t((input.IsDown('D') ? 1 : 0) - (input.IsDown('A') ? 1 : 0));
			command.moveZ = int8_t((input.IsDown('W') ? 1 : 0) - (input.IsDown('S') ? 1 : 0));
			if (input.IsDown(VK_SHIFT)) command.buttons |= Shared::kMoveButtonSprint;
			if (camera.Aiming()) command.buttons |= Shared::kMoveButtonAim;
			if (jumpLatched)
			{
				command.buttons |= Shared::kMoveButtonJump;
				jumpLatched = false;
			}
			const Vec3d look = camera.Forward();
			command.look[0] = float(look.x);
			command.look[1] = float(look.y);
			command.look[2] = float(look.z);

			previous = state;
			Shared::PlayerMovement::Step(state, command, tick, world);

			HistoryEntry& entry = history[command.seq % kHistory];
			entry.command = command;
			entry.after = state;
			frameCommands.push_back(command);
		}

		UpdateRenderPose(accumulator / tick);
	}

	double PlayerController::Reconcile(const ServerMoveState& server)
	{
		if (!planet || server.lastSeq == 0 || server.lastSeq >= nextSeq) return 0.0;
		const Shared::MoveWorld world = World();

		Shared::MoveState authoritative;
		authoritative.position = { server.pos[0], server.pos[1], server.pos[2] };
		authoritative.velocity = { server.velocity[0], server.velocity[1], server.velocity[2] };
		authoritative.up = world.Up(authoritative.position);
		authoritative.facing = ProjectOntoPlane(Vec3d{ server.facing[0], server.facing[1], server.facing[2] }, authoritative.up);
		authoritative.altitude = world.Altitude(authoritative.position);
		authoritative.verticalSpeed = server.verticalSpeed;
		authoritative.grounded = (server.flags & Shared::kMoveStateGrounded) != 0;
		authoritative.onObject = (server.flags & Shared::kMoveStateOnObject) != 0;
		authoritative.sprinting = (server.flags & Shared::kMoveStateSprinting) != 0;

		// 서버가 처리한 그 입력 뒤의 내 예측과 비교 — 가까우면 그대로 둔다
		const HistoryEntry& at = history[server.lastSeq % kHistory];
		if (at.command.seq == server.lastSeq &&
			Length(at.after.position - authoritative.position) <= kCorrectionThreshold)
			return 0.0;

		// 되감기 — 서버 상태에서 그 뒤 입력을 다시 적용한다(기록이 빠진 번호는 건너뛴다)
		Shared::MoveState replay = authoritative;
		for (uint32_t seq = server.lastSeq + 1; seq < nextSeq; ++seq)
		{
			HistoryEntry& entry = history[seq % kHistory];
			if (entry.command.seq != seq) continue;
			Shared::PlayerMovement::Step(replay, entry.command, Shared::PlayerMovement::kTickSeconds, world);
			entry.after = replay;
		}

		const double moved = Length(replay.position - state.position);
		state = replay;
		previous = replay;   // 보간을 끊고 바로 옮긴다
		UpdateRenderPose(1.0);
		++corrections;
		lastCorrection = moved;
		return moved;
	}

	void PlayerController::UpdateRenderPose(double alpha)
	{
		alpha = std::fmax(0.0, std::fmin(1.0, alpha));
		renderPosition = previous.position + (state.position - previous.position) * alpha;
		renderUp = Normalize(previous.up + (state.up - previous.up) * alpha);
		renderFacing = ProjectOntoPlane(previous.facing + (state.facing - previous.facing) * alpha, renderUp);
	}

	XMMATRIX PlayerController::WorldMatrix() const
	{
		const Vec3d right = Cross(renderUp, renderFacing);

		XMFLOAT4X4 m;
		m._11 = float(right.x);          m._12 = float(right.y);          m._13 = float(right.z);          m._14 = 0.0f;
		m._21 = float(renderUp.x);       m._22 = float(renderUp.y);       m._23 = float(renderUp.z);       m._24 = 0.0f;
		m._31 = float(renderFacing.x);   m._32 = float(renderFacing.y);   m._33 = float(renderFacing.z);   m._34 = 0.0f;
		m._41 = float(renderPosition.x); m._42 = float(renderPosition.y); m._43 = float(renderPosition.z); m._44 = 1.0f;
		return XMLoadFloat4x4(&m);
	}
}
