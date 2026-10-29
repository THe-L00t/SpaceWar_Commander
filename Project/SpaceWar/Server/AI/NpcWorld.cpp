#include "NpcWorld.h"

#include <cmath>
#include <utility>

#include "Shared/PlanetConst.h"
#include "Shared/GameLogic/GameLogic.h"
#include "Shared/GameLogic/PlayerMovement.h"
#include "Shared/Physics/CollisionWorld.h"
#include "Shared/Physics/PhysicsCalculator.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "Shared/AI/Node/SelectorNode.h"
#include "Shared/AI/Node/SequenceNode.h"
#include "HasTargetCondition.h"
#include "ChaseTargetAction.h"
#include "IdleAction.h"

namespace srv {

	namespace {

		// ── 실험 스위치는 실행 인자가 아니라 여기 상수로 둔다 ──
		constexpr float kDetectRange = 60.0f;   // 이 안에 들어오면 쫓는다 (m)
		constexpr float kStopDistance = 3.0f;   // 이만큼 붙으면 멈춘다 (m)
		constexpr float kNpcSpeed = 4.0f;       // m/s — 걷는 플레이어(6.5)보다 느리다

		// ── 무리 배치 (2026-10-29, 사용자 «스폰 위치에서 일정 거리 떨어진 범위에 무리지어») ──
		//  무리 중심 = 시작 지점에서 지면 거리 kGroupDistanceMin ~ kGroupDistanceMax 띠 안.
		//  띠 안쪽 끝 100 m − 무리 반지름 10 m = 90 m > 인지 거리 60 m — 시작 지점에 막 선 플레이어를 바로 쫓지 않는다.
		//  깔린 직후에는 대기하고, 플레이어가 다가가야 쫓기 시작한다 — Selector 가 갈래를 바꾸는 것을 눈으로 볼 수 있다.
		//  무리 방향은 한 바퀴를 무리 수로 나눈 칸마다 하나(칸 가운데 60 % 안에서 흔든다) — 무리끼리 겹치지 않고 사방에 흩어진다.
		//  (무리 10개면 칸 36°, 이웃 무리 중심은 최소 14.4° · 100 m 에서 약 25 m 떨어진다 > 무리 지름 20 m)
		constexpr float    kGroupDistanceMin = 100.0f;  // 시작 지점 → 무리 중심 최소 (m)
		constexpr float    kGroupDistanceMax = 180.0f;  // 시작 지점 → 무리 중심 최대 (m)
		constexpr float    kGroupRadius = 10.0f;        // 무리 중심에서 이 원 안에 선다 (m)
		constexpr float    kMemberSpacing = 1.5f;       // 같은 무리 NPC 끼리 최소 간격 (m) — 캡슐 지름 0.8 m
		constexpr int      kPlacementTries = 24;        // 빈자리 찾기 시도 수(무리 중심 · NPC 자리 각각)
		constexpr uint32_t kLayoutSeed = 20261029u;     // 서버를 켤 때마다 같은 배치

		// ── 오브젝트 충돌 (2026-10-29, 사용자 «npc도 오브젝트와 충돌하도록») — PushOutOfObjects ──
		constexpr int   kNpcCollisionIterations = 4;    // 한 틱에 밀어내는 횟수(플레이어 kCollisionIterations 와 같다)
		constexpr float kNpcMinSidePush = 0.3f;         // 법선의 접평면 성분이 이보다 작으면 누운 면(바닥 · 천장) — 옆으로 밀지 않는다
		constexpr float kNpcSkin = 0.001f;              // 밀어낼 때 더 띄우는 몫(m) — 다음 틱에 같은 면에 다시 닿지 않게

		// Shared::Vec3 는 연산자가 없는 순수 데이터다. 여기서만 쓰는 계산을 붙인다.
		Shared::Vec3 Add(const Shared::Vec3& a, const Shared::Vec3& b)
		{
			return { a.x + b.x, a.y + b.y, a.z + b.z };
		}

		Shared::Vec3 Sub(const Shared::Vec3& a, const Shared::Vec3& b)
		{
			return { a.x - b.x, a.y - b.y, a.z - b.z };
		}

		Shared::Vec3 Scale(const Shared::Vec3& a, float s)
		{
			return { a.x * s, a.y * s, a.z * s };
		}

		float Dot(const Shared::Vec3& a, const Shared::Vec3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		float Length(const Shared::Vec3& a)
		{
			return std::sqrt(Dot(a, a));
		}

		Shared::Vec3 Normalize(const Shared::Vec3& a)
		{
			const float len = Length(a);
			return len > 1.0e-6f ? Scale(a, 1.0f / len) : Shared::Vec3{ 0.0f, 1.0f, 0.0f };
		}

		Shared::Vec3 Cross(const Shared::Vec3& a, const Shared::Vec3& b)
		{
			return { a.y * b.z - a.z * b.y,
					 a.z * b.x - a.x * b.z,
					 a.x * b.y - a.y * b.x };
		}

		Shared::Vec3 PlanetCenter()
		{
			return { float(Shared::kPlanetCenterX),
					 float(Shared::kPlanetCenterY),
					 float(Shared::kPlanetCenterZ) };
		}

	} // namespace

	NpcWorld::NpcWorld()
		: rng(kLayoutSeed)
	{
		BuildTree();
	}

	NpcWorld::~NpcWorld() = default;

	/////////////////////////////////////////////////////////////////////
	//  트리를 조립한다. 노드마다 상태 슬롯 번호를 순서대로 준다.
	void NpcWorld::BuildTree()
	{
		Shared::SelectorNode* selector = new Shared::SelectorNode();
		Shared::SequenceNode* chase = new Shared::SequenceNode();
		HasTargetCondition*   hasTarget = new HasTargetCondition(kDetectRange);
		ChaseTargetAction*    chaseMove = new ChaseTargetAction();
		IdleAction*           idle = new IdleAction();

		nodes.emplace_back(selector);
		nodes.emplace_back(chase);
		nodes.emplace_back(hasTarget);
		nodes.emplace_back(chaseMove);
		nodes.emplace_back(idle);

		nodeCount = (uint16_t)nodes.size();
		for (uint16_t i = 0; i < nodeCount; ++i)
		{
			nodes[i]->SetIndex(i);
		}

		chase->AddChild(hasTarget);
		chase->AddChild(chaseMove);

		selector->AddChild(chase);
		selector->AddChild(idle);

		root = selector;
	}

	/////////////////////////////////////////////////////////////////////
	//  같은 XZ 또는 구면 방향의 지면 위로 옮긴다.
	//
	//  ★ 플레이어 착지(클라 PlayerController)·위치 검사(서버 main.cpp)와 같은 계산이다.
	//    지형 높이 + kGroundOffset. 구면 모드에서만 기준구 반지름을 더한다.
	Shared::Vec3 NpcWorld::OnGround(const Shared::Vec3& position) const
	{
		if constexpr (Shared::kUsePlanarMap)
		{
			const double height = terrain ? terrain->Height(position.x, position.y, position.z) : 0.0;
			return { position.x, float(height + Shared::kGroundOffset), position.z };
		}
		const Shared::Vec3 center = PlanetCenter();
		const Shared::Vec3 up = Normalize(Sub(position, center));

		const double height = terrain ? terrain->Height(up.x, up.y, up.z) : 0.0;
		const float  radius = float(Shared::kPlanetRadius + height + Shared::kGroundOffset);

		return Add(center, Scale(up, radius));
	}

	/////////////////////////////////////////////////////////////////////
	//  from 에서 지면을 따라 distance 떨어진 angle 방향의 한 점을 지면 위로 올려 돌려준다.
	//  무리 중심(시작 지점 기준)과 무리 안 자리(무리 중심 기준)가 같은 계산을 쓴다.
	//
	//  ★ 구면 맵은 접평면에 그리지 않고 대원을 따라 돌린다 — 접평면으로 180 m 를 그리면 지면 거리는 약 176 m 다(반지름 650 m).
	Shared::Vec3 NpcWorld::SurfacePoint(const Shared::Vec3& from, float angle, float distance) const
	{
		if constexpr (Shared::kUsePlanarMap)
		{
			return OnGround({ from.x + std::cos(angle) * distance, from.y, from.z + std::sin(angle) * distance });
		}

		const Shared::Vec3 center = PlanetCenter();
		const Shared::Vec3 up = Normalize(Sub(from, center));

		// 접평면의 기준축 두 개. up 과 나란하지 않은 아무 벡터에서 만든다.
		const Shared::Vec3 seed = (std::fabs(up.z) < 0.9f)
			? Shared::Vec3{ 0.0f, 0.0f, 1.0f }
			: Shared::Vec3{ 1.0f, 0.0f, 0.0f };
		const Shared::Vec3 axisX = Normalize(Cross(up, seed));
		const Shared::Vec3 axisY = Cross(up, axisX);
		const Shared::Vec3 heading = Add(Scale(axisX, std::cos(angle)), Scale(axisY, std::sin(angle)));

		// 중심각 = 호 길이 ÷ 반지름. 방향만 정하면 높이는 OnGround 가 지형으로 잡는다.
		const float theta = distance / float(Shared::kPlanetRadius);
		const Shared::Vec3 direction = Add(Scale(up, std::cos(theta)), Scale(heading, std::sin(theta)));

		return OnGround(Add(center, Scale(direction, float(Shared::kPlanetRadius))));
	}

	/////////////////////////////////////////////////////////////////////
	//  몸통 중심에 선 캡슐이 정적 충돌체와 겹치는가.
	//
	//  ★ 플레이어 이동(Shared/GameLogic/PlayerMovement.cpp ResolveCollisions)과 같은 캡슐 · 같은 맵 공간 변환이다.
	//    NPC 도 플레이어와 같은 캐릭터 모델을 쓴다(클라 Engine — characterModel).
	bool NpcWorld::ObjectContact(const Shared::Vec3& position, Shared::CapsuleContact& out) const
	{
		if (!collision || !collision->Loaded()) return false;

		const Shared::Vec3 center = PlanetCenter();
		const Shared::Vec3 up = Shared::kUsePlanarMap
			? Shared::Vec3{ 0.0f, 1.0f, 0.0f } : Normalize(Sub(position, center));
		const Shared::Vec3 local = Sub(position, center);   // 맵 공간

		const float radius = Shared::PlayerMovement::kCapsuleRadius;
		const float bottom = float(Shared::kGroundOffset) - radius;                                  // 몸통 중심 → 아래 구 중심
		const float top = Shared::PlayerMovement::kCapsuleHeight - float(Shared::kGroundOffset) - radius; // 몸통 중심 → 위 구 중심

		return collision->DeepestCapsuleContact(Sub(local, Scale(up, bottom)), Add(local, Scale(up, top)),
			radius, out);
	}

	bool NpcWorld::Blocked(const Shared::Vec3& position) const
	{
		Shared::CapsuleContact contact;
		return ObjectContact(position, contact);
	}

	/////////////////////////////////////////////////////////////////////
	//  이동한 자리(지면 위)가 오브젝트와 겹치면 밀어낸다 (2026-10-29 사용자 «npc도 오브젝트와 충돌하도록 해줘»).
	//
	//  ★ 접평면으로만 민다 — NPC 높이는 지형이 정한다(OnGround). 위로 올라서거나 뛰어넘지 않는다.
	//    겹친 깊이를 옆으로만 풀므로 경사진 면일수록 더 민다(깊이 ÷ 법선의 옆 성분).
	//    벽에 비스듬히 부딪히면 벽으로 들어간 몫만 빠져 벽을 따라 미끄러진다 — 플레이어(PlayerMovement ResolveCollisions)와 같은 느낌.
	//  ★ 누운 면(법선이 거의 위 · 아래 — 바닥 판 · 천장)은 옆으로 밀어 풀 수 없다 — 그 자리를 그대로 둔다(멈춰 서지 않게).
	//  ★ kNpcCollisionIterations 번 밀어도 겹치면(구석) 이번 틱은 움직이지 않는다 — 벽에 박히는 것보다 낫다.
	Shared::Vec3 NpcWorld::PushOutOfObjects(const Shared::Vec3& previous, Shared::Vec3 position) const
	{
		if (!collision || !collision->Loaded()) return position;

		const Shared::Vec3 center = PlanetCenter();
		for (int iteration = 0; iteration < kNpcCollisionIterations; ++iteration)
		{
			Shared::CapsuleContact contact;
			if (!ObjectContact(position, contact)) return position;   // 빈자리

			const Shared::Vec3 up = Shared::kUsePlanarMap
				? Shared::Vec3{ 0.0f, 1.0f, 0.0f } : Normalize(Sub(position, center));
			const Shared::Vec3 side = Sub(contact.normal, Scale(up, Dot(contact.normal, up)));
			const float        sideLength = Length(side);
			if (sideLength < kNpcMinSidePush) return position;          // 누운 면

			const float push = (contact.depth + kNpcSkin) / sideLength;
			position = OnGround(Add(position, Scale(side, push / sideLength)));
		}

		return Blocked(position) ? previous : position;
	}

	float NpcWorld::Random01()
	{
		// 위 24비트만 쓴다 — float 가 1.0 으로 반올림되지 않는다.
		return float(rng() >> 8) * (1.0f / 16777216.0f);
	}

	/////////////////////////////////////////////////////////////////////
	//  무리 원 안의 빈자리. 원 안에 고르게 퍼지도록 반지름은 √u 로 뽑는다.
	Shared::Vec3 NpcWorld::PickMemberPosition(uint32_t group)
	{
		const Shared::Vec3& groupCenter = groupCenters[group];
		Shared::Vec3 candidate = groupCenter;

		for (int attempt = 0; attempt < kPlacementTries; ++attempt)
		{
			const float angle = 6.2831853f * Random01();
			const float distance = kGroupRadius * std::sqrt(Random01());
			candidate = SurfacePoint(groupCenter, angle, distance);

			if (Blocked(candidate)) continue;

			bool crowded = false;
			for (size_t i = 0; i < entries.size(); ++i)
			{
				if (!entries[i].alive || entries[i].group != group) continue;
				if (Length(Sub(entries[i].npc.position, candidate)) < kMemberSpacing) { crowded = true; break; }
			}
			if (!crowded) return candidate;
		}

		++blockedPlacements;   // 못 찾았다 — 마지막 후보에 그냥 세운다(콘솔에 누계가 찍힌다)
		return candidate;
	}

	/////////////////////////////////////////////////////////////////////
	//  시작 지점에서 떨어진 띠에 무리지어 깐다.
	//
	//  ① 무리 중심을 고른다 — 방향은 칸마다 하나, 거리는 띠 안에서. 충돌체 안이면 다시 고른다.
	//  ② 무리마다 kNpcGroupSize 마리씩 중심 둘레 원 안의 빈자리에 세운다.
	void NpcWorld::SpawnGroups(const Shared::Vec3& spawnPos, int count)
	{
		if (count <= 0) return;

		const int   groupSize = kNpcGroupSize > 0 ? kNpcGroupSize : 1;
		const int   groups = (count + groupSize - 1) / groupSize;
		const float sector = 6.2831853f / float(groups);
		const uint32_t firstGroup = (uint32_t)groupCenters.size();

		for (int g = 0; g < groups; ++g)
		{
			Shared::Vec3 groupCenter = spawnPos;
			for (int attempt = 0; attempt < kPlacementTries; ++attempt)
			{
				const float angle = sector * (float(g) + 0.2f + 0.6f * Random01());
				const float distance = kGroupDistanceMin + (kGroupDistanceMax - kGroupDistanceMin) * Random01();
				groupCenter = SurfacePoint(spawnPos, angle, distance);
				if (!Blocked(groupCenter)) break;   // 못 찾으면 마지막 후보 — NPC 자리는 따로 다시 검사한다
			}
			groupCenters.push_back(groupCenter);
		}

		for (int i = 0; i < count; ++i)
		{
			Entry e;
			e.npcId = nextNpcId++;
			e.group = firstGroup + uint32_t(i / groupSize);
			e.npc.position = PickMemberPosition(e.group);
			e.npc.speed = kNpcSpeed;
			e.npc.health = Shared::kMaxHealth;
			e.ctx.npc = e.npcId;
			e.ctx.Allocate(nodeCount);

			entries.push_back(std::move(e));
		}
	}

	/////////////////////////////////////////////////////////////////////
	//  레이에 가장 먼저 맞는 살아 있는 NPC.
	bool NpcWorld::Raycast(const Shared::Vec3& origin, const Shared::Vec3& direction,
		float maxDistance, uint32_t& outNpcId, float& outDistance) const
	{
		bool  found = false;
		float nearest = maxDistance;

		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (!entries[i].alive) continue;

			float dist = 0.0f;
			if (!Shared::PhysicsCalculator::RaySphere(origin, direction,
				entries[i].npc.position, Shared::kHitRadius, nearest, dist))
				continue;

			if (found && dist >= nearest) continue;

			found = true;
			nearest = dist;
			outNpcId = entries[i].npcId;
			outDistance = dist;
		}

		return found;
	}

	/////////////////////////////////////////////////////////////////////
	//  피해. 이번 피격으로 쓰러졌으면 true.
	//
	//  ★ 사라지는 것은 «목록에서 빼는 것» 이 아니라 alive 를 내리는 것이다
	//    항목을 지우면 번호가 사라져 재등장 시계를 어디에 둘지가 없어진다.
	//    같은 번호로 10초 뒤에 다시 세우면 클라도 처음 보는 NPC 처럼 노드를 만든다.
	bool NpcWorld::Damage(uint32_t npcId, float damage)
	{
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i].npcId != npcId || !entries[i].alive) continue;

			entries[i].npc.health = Shared::GameLogic::ApplyDamage(entries[i].npc.health, damage);

			if (!Shared::GameLogic::IsDown(entries[i].npc.health))
				return false;

			entries[i].alive = false;
			entries[i].respawnTimer = Shared::kNpcRespawnDelay;
			entries[i].ctx.decision = Shared::BehaviorDecision{};
			despawned.push_back(npcId);
			return true;
		}

		return false;
	}

	void NpcWorld::TakeDespawned(std::vector<uint32_t>& out)
	{
		out.swap(despawned);
		despawned.clear();
	}

	/////////////////////////////////////////////////////////////////////
	//  한 틱.
	void NpcWorld::Tick(const std::vector<PlayerView>& players, float dt)
	{
		if (!root) return;

		const Shared::Vec3 center = PlanetCenter();

		for (size_t i = 0; i < entries.size(); ++i)
		{
			Entry& e = entries[i];

			// ── 0) 쓰러져 있으면 재등장 시계만 돈다 ──
			//  자리는 자기 무리 원 안의 빈자리(2026-10-29 — 예전엔 «그때의 플레이어 둘레» 80 m 고리).
			//  플레이어 위치가 필요 없으므로 플레이어가 없어도 다시 선다.
			if (!e.alive)
			{
				e.respawnTimer -= dt;
				if (e.respawnTimer > 0.0f) continue;

				e.npc.position = PickMemberPosition(e.group);
				e.npc.health = Shared::kMaxHealth;
				e.alive = true;
				continue;
			}

			// ── 1) 이번 틱에 노드가 볼 것을 채운다 ──
			e.ctx.dt = dt;
			e.ctx.npcPos = e.npc.position;
			e.ctx.hasNearestPlayer = false;
			e.ctx.nearestPlayerId = 0;
			e.ctx.nearestPlayerDist = 0.0f;
			e.ctx.decision = Shared::BehaviorDecision{};

			float nearest = 0.0f;
			for (size_t p = 0; p < players.size(); ++p)
			{
				const float dist = Length(Sub(players[p].pos, e.npc.position));

				if (!e.ctx.hasNearestPlayer || dist < nearest)
				{
					nearest = dist;
					e.ctx.hasNearestPlayer = true;
					e.ctx.nearestPlayerId = players[p].playerId;
					e.ctx.nearestPlayerPos = players[p].pos;
					e.ctx.nearestPlayerDist = dist;
				}
			}

			// ── 2) 트리를 돌려 «무엇을 할지» 를 정한다 ──
			root->Tick(e.ctx);

			// ── 3) 결정대로 움직인다 ──
			if (!e.ctx.decision.hasMoveTarget) continue;

			const Shared::Vec3 target = e.ctx.decision.moveTarget;
			const Shared::Vec3 toTarget = Sub(target, e.npc.position);

			if (Length(toTarget) <= kStopDistance) continue;   // 충분히 붙었다

			// 평면 맵은 XZ, 구면 맵은 접평면 성분만 남긴다.
			const Shared::Vec3 up = Shared::kUsePlanarMap
				? Shared::Vec3{ 0.0f, 1.0f, 0.0f } : Normalize(Sub(e.npc.position, center));
			const Shared::Vec3 tangent = Sub(toTarget, Scale(up, Dot(toTarget, up)));
			const float        tangentLen = Length(tangent);

			if (tangentLen < 1.0e-4f) continue;   // 바로 위/아래. 접선 방향이 없다

			const float step = e.npc.speed * dt;
			const float move = step < tangentLen ? step : tangentLen;

			const Shared::Vec3 next = Add(e.npc.position,
										  Scale(Scale(tangent, 1.0f / tangentLen), move));

			// 높이는 지형이 정한다. 쫓는 플레이어가 오르내려도 NPC 고도는 끌려가지 않는다.
			// 오브젝트(정적 충돌체)에 막히면 접평면으로 밀려나 벽을 따라 미끄러진다(2026-10-29).
			e.npc.position = PushOutOfObjects(e.npc.position, OnGround(next));

			// 바라보는 방향도 갱신해 둔다. 아직 아무도 쓰지 않는다.
			e.npc.direction = Scale(tangent, 1.0f / tangentLen);
		}
	}

	void NpcWorld::Snapshot(std::vector<NpcView>& out) const
	{
		out.clear();
		out.reserve(entries.size());

		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (!entries[i].alive) continue;   // 쓰러진 NPC 는 보내지 않는다

			NpcView v;
			v.npcId = entries[i].npcId;
			v.pos = entries[i].npc.position;
			out.push_back(v);
		}
	}

	int NpcWorld::ChasingCount() const
	{
		int n = 0;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i].alive && entries[i].ctx.decision.hasMoveTarget) ++n;
		}
		return n;
	}

} // namespace srv
