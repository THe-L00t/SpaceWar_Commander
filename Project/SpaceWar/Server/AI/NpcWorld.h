#pragma once
#include <cstdint>
#include <memory>
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
//
//  ★ 전투 판정도 여기서 받는다 (2026-09-24)
//    사격 판정은 «행동 스레드 한 곳» 에서만 부른다. IOCP 워커가 NPC 를 직접 깎으면
//    위의 «한 스레드만 건드린다» 규약이 깨진다. 워커는 발사 요청을 큐에 넣기만 한다.
//    쓰러진 NPC 는 목록에서 빠지고(Snapshot 제외), 10초 뒤 플레이어 주변에 다시 선다.
// ============================================================

namespace Shared { class TerrainSampler; }

namespace srv {

	// 한 번에 깔 NPC 수. 실험 스위치는 실행 인자가 아니라 코드 상수로 둔다.
	inline constexpr int kNpcSpawnCount = 3;

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

		// 플레이어 주변 고리 모양으로 NPC 를 깔아둔다.
		void SpawnAround(const Shared::Vec3& playerPos, int count);

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
		};

		void BuildTree();

		// 플레이어 둘레 고리 위의 한 점을 지면 위로 올려 돌려준다.
		// 첫 배치와 재등장이 같은 계산을 쓴다.
		Shared::Vec3 RingPosition(const Shared::Vec3& playerPos, float angle) const;

		// 그 방향의 지면 위(몸통 중심 높이)로 옮긴다.
		Shared::Vec3 OnGround(const Shared::Vec3& position) const;

		const Shared::TerrainSampler* terrain = nullptr;   // 없으면 평평한 구

		// 트리를 소유한다. 노드 수명은 여기서만 관리한다.
		std::vector<std::unique_ptr<Shared::BehaviorNode>> nodes;
		Shared::BehaviorNode* root = nullptr;
		uint16_t              nodeCount = 0;

		std::vector<Entry>    entries;
		std::vector<uint32_t> despawned;   // 이번에 쓰러져 알려야 할 번호
		uint32_t              nextNpcId = 1;
	};

} // namespace srv
