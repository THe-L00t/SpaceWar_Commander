#include "NpcWorld.h"

#include <cmath>
#include <utility>

#include "Shared/PlanetConst.h"
#include "Shared/GameLogic/GameLogic.h"
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
		//  스폰 고리(80m) > 인지 거리(60m) 로 잡아 두었다.
		//  깔린 직후에는 대기하고, 플레이어가 다가가야 쫓기 시작한다 —
		//  Selector 가 갈래를 바꾸는 것을 눈으로 볼 수 있다.
		constexpr float kSpawnRing = 80.0f;     // 플레이어에서 이만큼 떨어져 스폰 (m)
		constexpr float kDetectRange = 60.0f;   // 이 안에 들어오면 쫓는다 (m)
		constexpr float kStopDistance = 3.0f;   // 이만큼 붙으면 멈춘다 (m)
		constexpr float kNpcSpeed = 4.0f;       // m/s — 걷는 플레이어(6.5)보다 느리다

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
	//  그 방향의 지면 위로 옮긴다.
	//
	//  ★ 플레이어 착지(클라 PlayerController)·위치 검사(서버 main.cpp)와 같은 계산이다.
	//    기준구 반지름 + 지형 높이 + kGroundOffset. 하나라도 다르면 NPC 가 묻히거나 뜬다.
	Shared::Vec3 NpcWorld::OnGround(const Shared::Vec3& position) const
	{
		const Shared::Vec3 center = PlanetCenter();
		const Shared::Vec3 up = Normalize(Sub(position, center));

		const double height = terrain ? terrain->Height(up.x, up.y, up.z) : 0.0;
		const float  radius = float(Shared::kPlanetRadius + height + Shared::kGroundOffset);

		return Add(center, Scale(up, radius));
	}

	/////////////////////////////////////////////////////////////////////
	//  플레이어 둘레 고리 위의 한 점. 접평면에 그린 뒤 그 방향의 지면으로 내린다.
	Shared::Vec3 NpcWorld::RingPosition(const Shared::Vec3& playerPos, float angle) const
	{
		const Shared::Vec3 center = PlanetCenter();
		const Shared::Vec3 up = Normalize(Sub(playerPos, center));

		// 접평면의 기준축 두 개. up 과 나란하지 않은 아무 벡터에서 만든다.
		const Shared::Vec3 seed = (std::fabs(up.z) < 0.9f)
			? Shared::Vec3{ 0.0f, 0.0f, 1.0f }
			: Shared::Vec3{ 1.0f, 0.0f, 0.0f };
		const Shared::Vec3 axisX = Normalize(Cross(up, seed));
		const Shared::Vec3 axisY = Cross(up, axisX);

		const Shared::Vec3 offset = Add(Scale(axisX, std::cos(angle) * kSpawnRing),
										Scale(axisY, std::sin(angle) * kSpawnRing));

		return OnGround(Add(playerPos, offset));
	}

	/////////////////////////////////////////////////////////////////////
	//  플레이어 주위에 고리 모양으로 깔아둔다.
	void NpcWorld::SpawnAround(const Shared::Vec3& playerPos, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			const float angle = 6.2831853f * float(i) / float(count);

			Entry e;
			e.npcId = nextNpcId++;
			e.npc.position = RingPosition(playerPos, angle);
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
			//  자리는 «그때의 플레이어 둘레» 라 매번 달라진다. 플레이어가 없으면 기다린다.
			if (!e.alive)
			{
				e.respawnTimer -= dt;
				if (e.respawnTimer > 0.0f || players.empty()) continue;

				const size_t pick = size_t(e.npcId) % players.size();
				const float  angle = 6.2831853f * float(e.npcId % 8u) / 8.0f;

				e.npc.position = RingPosition(players[pick].pos, angle);
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

			// 구 위를 걷는다: 접평면 성분만 남긴다.
			const Shared::Vec3 up = Normalize(Sub(e.npc.position, center));
			const Shared::Vec3 tangent = Sub(toTarget, Scale(up, Dot(toTarget, up)));
			const float        tangentLen = Length(tangent);

			if (tangentLen < 1.0e-4f) continue;   // 바로 위/아래. 접선 방향이 없다

			const float step = e.npc.speed * dt;
			const float move = step < tangentLen ? step : tangentLen;

			const Shared::Vec3 next = Add(e.npc.position,
										  Scale(Scale(tangent, 1.0f / tangentLen), move));

			// 높이는 지형이 정한다. 쫓는 플레이어가 오르내려도 NPC 고도는 끌려가지 않는다.
			e.npc.position = OnGround(next);

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
