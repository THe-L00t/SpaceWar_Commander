#pragma once
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "Shared/Vec3.h"
#include "Shared/GameObject/NPC.h"
#include "Shared/AI/BehaviorContext.h"
#include "Shared/AI/Node/BehaviorNode.h"

// ============================================================
//  Server/AI/NpcWorld.h — 서버가 굴리는 NPC 무리
//
//  ★ 행동 트리는 한 벌뿐이다
//    노드가 실행 상태를 갖지 않으므로 NPC 수백 마리가 같은 트리를 본다.
//    NPC 마다 다른 것은 BehaviorContext 하나뿐이다.
//
//  ★ 트리 모양 (가장 단순한 형태)
//        Selector
//        ├── Sequence : [사거리 안에 플레이어] -> 추격
//        └── Action   : 대기
//
//  ★ 노드는 NPC 를 고치지 않는다
//    트리는 «무엇을 하겠다» 를 ctx.decision 에 적고, 위치를 실제로 옮기는 것은
//    이 클래스다. 나중에 트리를 잡으로 돌릴 때 이 경계가 그대로 필요하다.
//
//  ★ 고도는 서버가 지형으로 정한다
//    클라와 같은 TerrainSampler 로 지면 위에 세운다. 클라는 받은 좌표를 그대로 그린다.
//    오브젝트 윗면에 올라서지는 않는다 — 막히기만 한다(아래 «오브젝트 충돌»).
//
//  ★ 전투 판정도 여기서 받는다 (2026-09-24)
//    사격 판정은 «행동 스레드 한 곳» 에서만 부른다. IOCP 워커가 NPC 를 직접 깎으면
//    위의 «한 스레드만 건드린다» 규약이 깨진다. 워커는 발사 요청을 큐에 넣기만 한다.
//    쓰러진 NPC 는 목록에서 빠지고(Snapshot 제외), 10초 뒤 자기 무리 자리에 다시 선다.
//
//  ★ 무리 배치 (2026-10-29)
//    사용자 지시 «npc를 100마리 배치할건데 스폰 위치에서 일정 거리 떨어진 범위에 무리지어 생성되도록 해줘».
//    플레이어 시작 지점(main.cpp SpawnPoint — Landing L1 지면)에서 일정 거리 띠 안에 무리 중심을 고르고,
//    무리마다 중심 둘레 작은 원 안에 kNpcGroupSize 마리씩 세운다. 정적 충돌체(건물 · 소품) 안의 자리는 다시 고른다.
//    값은 NpcWorld.cpp 상단 상수. 기록: 노션 「NPC 100마리 무리 배치 (2026-10-29)」.
//
//  ★ 오브젝트 충돌 (2026-10-29 사용자 «npc도 오브젝트와 충돌하도록 해줘»)
//    쫓아 움직인 자리가 정적 충돌체와 겹치면 접평면으로 밀어낸다(PushOutOfObjects) — 높이는 그대로 지형이 정한다.
//    캡슐은 플레이어와 같다(Shared::PlayerMovement). 플레이어 · NPC 끼리는 아직 부딪히지 않는다.
// ============================================================

namespace Shared { class TerrainSampler; class CollisionWorld; struct CapsuleContact; }

namespace srv {

	// 한 번에 깔 NPC 수. 실험 스위치는 실행 인자가 아니라 코드 상수로 둔다.
	inline constexpr int kNpcSpawnCount = 100;

	// 무리 하나의 마릿수 — 무리 수 = kNpcSpawnCount ÷ kNpcGroupSize (나머지는 마지막 무리에 덜 든다).
	inline constexpr int kNpcGroupSize = 10;

	// 서버가 보는 플레이어 한 명. NpcWorld 는 세션도 소켓도 모른다.
	struct PlayerView
	{
		uint32_t     playerId = 0;
		Shared::Vec3 pos{};
	};

	// 브로드캐스트할 NPC 한 마리의 현재 상태.
	struct NpcView
	{
		uint32_t     npcId = 0;
		Shared::Vec3 pos{};
	};

	class NpcWorld
	{
	public:
		NpcWorld();
		~NpcWorld();

		bool   Empty() const { return entries.empty(); }
		size_t Count() const { return entries.size(); }

		// 스폰·이동 고도를 잡을 지형. 행동 스레드를 띄우기 전에 한 번 준다.
		void SetTerrain(const Shared::TerrainSampler* t) { terrain = t; }

		// 스폰 자리를 고를 때 피할 정적 충돌체(건물 · 소품). 없으면(nullptr) 지형만 본다. 행동 스레드를 띄우기 전에 한 번 준다.
		void SetCollision(const Shared::CollisionWorld* c) { collision = c; }

		// 시작 지점(spawnPos, 월드 좌표)에서 일정 거리 띠 안에 무리지어 count 마리를 깐다 (2026-10-29).
		void SpawnGroups(const Shared::Vec3& spawnPos, int count);

		size_t GroupCount() const { return groupCenters.size(); }

		// 충돌체에 막혀 빈자리를 끝내 못 찾고 그냥 세운 횟수(첫 배치 + 재등장 누계, 콘솔 표시용).
		int BlockedPlacements() const { return blockedPlacements; }

		// 한 틱: 가장 가까운 플레이어를 찾아 트리를 돌리고 결정대로 움직인다.
		// 쓰러진 NPC 의 재등장 시계도 여기서 돈다.
		void Tick(const std::vector<PlayerView>& players, float dt);

		// ── 사격 판정 ───────────────────────────────────────
		//  레이에 가장 먼저 맞는 살아 있는 NPC 를 찾는다. 지형 가림은 보지 않는다
		//  (부르는 쪽이 대상을 정한 뒤 한 번만 검사한다).
		bool Raycast(const Shared::Vec3& origin, const Shared::Vec3& direction,
			float maxDistance, uint32_t& outNpcId, float& outDistance) const;

		// 피해를 입힌다. 이번 피격으로 쓰러졌으면 true.
		bool Damage(uint32_t npcId, float damage);

		// 쓰러져서 사라진 번호를 가져간다 (가져가면 비운다).
		void TakeDespawned(std::vector<uint32_t>& out);

		// 보낼 것만 뽑아낸다. 쓰러진 NPC 는 빠진다.
		void Snapshot(std::vector<NpcView>& out) const;

		// 지금 몇 마리가 추격 중인가 (콘솔 표시용).
		int ChasingCount() const;

	private:
		struct Entry
		{
			uint32_t                npcId = 0;
			Shared::NPC             npc;
			Shared::BehaviorContext ctx;

			bool  alive = true;
			float respawnTimer = 0.0f;   // 쓰러진 뒤 남은 시간 (s)
			uint32_t group = 0;          // groupCenters 번호 — 재등장도 이 무리 자리에 한다
		};

		void BuildTree();

		// from 에서 지면을 따라 distance(m) 떨어진, angle(rad) 방향의 지면 위 한 점.
		//  구면 맵은 대원 거리, 평면 맵은 XZ 거리. 방향 기준축은 from 의 접평면에서 정한다.
		Shared::Vec3 SurfacePoint(const Shared::Vec3& from, float angle, float distance) const;

		// 그 방향의 지면 위(몸통 중심 높이)로 옮긴다.
		Shared::Vec3 OnGround(const Shared::Vec3& position) const;

		// 몸통 중심 position 에 선 캡슐(플레이어와 같은 크기)이 정적 충돌체와 가장 깊이 겹친 곳. 겹치지 않거나 충돌체가 없으면 false.
		bool ObjectContact(const Shared::Vec3& position, Shared::CapsuleContact& out) const;

		// ObjectContact 가 있으면 true — 스폰 자리 검사.
		bool Blocked(const Shared::Vec3& position) const;

		// 이동한 자리 position(지면 위)이 오브젝트와 겹치면 접평면으로 밀어낸다(벽을 따라 미끄러진다, 2026-10-29).
		//  다 못 풀면(구석) previous(이번 틱 전 자리)를 돌려준다.
		Shared::Vec3 PushOutOfObjects(const Shared::Vec3& previous, Shared::Vec3 position) const;

		// group 무리 원 안에서 빈자리를 고른다. 같은 무리 다른 NPC 와 kMemberSpacing 이상 띄운다(첫 배치 때만 의미가 있다).
		//  kPlacementTries 번 안에 못 찾으면 마지막 후보를 그대로 쓰고 blockedPlacements 를 센다.
		Shared::Vec3 PickMemberPosition(uint32_t group);

		// 0 이상 1 미만 균등 난수 — 배치를 매번 같게(kLayoutSeed) 하려고 rng 하나로 뽑는다.
		float Random01();

		const Shared::TerrainSampler* terrain = nullptr;   // 없으면 평평한 구
		const Shared::CollisionWorld* collision = nullptr; // 없으면 스폰 자리 검사를 건너뛴다

		std::vector<Shared::Vec3> groupCenters;            // 무리 중심(지면 위, 월드 좌표)
		std::mt19937              rng;                     // 생성자에서 kLayoutSeed 로 씨앗을 준다
		int                       blockedPlacements = 0;

		// 트리를 소유한다. 노드 수명은 여기서만 관리한다.
		std::vector<std::unique_ptr<Shared::BehaviorNode>> nodes;
		Shared::BehaviorNode* root = nullptr;
		uint16_t              nodeCount = 0;

		std::vector<Entry>    entries;
		std::vector<uint32_t> despawned;   // 이번에 쓰러져 알려야 할 번호
		uint32_t              nextNpcId = 1;
	};

} // namespace srv
