#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>
#include "Client/Vec3d.h"
#include "Client/Weapon/WeaponModels.h"
#include "Shared/GameLogic/WeaponPickup.h"

// ============================================================
//  Client/Weapon/WeaponScene.h — 화면에 놓인 총 (2026-11-15 줍기 · 떨구기)
//
//  사용자 지시(2026-11-15): «주우면 애니메이션 시에 적절한 위치에 총을 고정시키고, 쏠 수 있도록 해줘. 줍기, 떨구기 둘 다
//    구현하면 되는거야. 이때 공중에 있는것을 줍고, 떨구면 땅바닥에 떨어지도록. 공중에 있는 총은 사라지지 않게»
//  ★ 무엇을 들었는지 · 바닥 총 목록은 서버가 정한다(Shared/GameLogic/WeaponPickup.h). 이 클래스는 받은 대로 그리기만 한다.
//    오프라인이면 Engine 이 같은 계산(Shared)으로 직접 채운다.
//  ★ 어느 모델을 그리나(2026-11-14 사용자 — WeaponModels.h)
//    · 내가 든 총   → 1인칭 모델. 내 캐릭터 노드의 자식(손 자리 = Shared::HeldWeaponPose 와 같은 몸 공간 오프셋)
//    · 남이 든 총   → 3인칭 모델. 원격 캐릭터 자리 × 손 자리 — 매 프레임 놓는다
//    · 바닥 총     → 가까우면(kGroundFirstPersonDistance 안) 1인칭 · 멀면 3인칭
//  ★ 노드는 지우지 않고 다시 쓴다(Scene 에 삭제 API 가 없다 — 원격 플레이어 · 레이저탄과 같은 방식). 종류 × 시점마다 빈 노드 목록.
//  ★ 메인 스레드 소유(Engine).
// ============================================================

namespace swc {

	class LegacyScene;

	class WeaponScene
	{
	public:
		static constexpr double kGroundFirstPersonDistance = 10.0;   // m — 바닥 총이 이 안이면 1인칭 모델(2026-11-14 계획 «예: 10 m»)

		void Bind(const WeaponModels* models, LegacyScene* scene);

		// ── 내 손 ── 든 총이 바뀌면 그 종류의 1인칭 노드만 보이게 한다(종류마다 하나를 만들어 둔다).
		void SetLocalHeld(Shared::WeaponType type, NodeHandle playerNode);
		Shared::WeaponType LocalHeld() const { return localType; }

		// ── 남의 손 ── 프레임마다 Begin → (보이는 사람마다) Place → End. End 는 이번에 안 놓인 사람의 총을 치운다.
		void BeginRemoteFrame();
		void PlaceRemote(uint32_t playerId, Shared::WeaponType type, const DirectX::XMMATRIX& character);
		void EndRemoteFrame();
		void ClearRemotes();

		// ── 바닥 ──
		struct Ground
		{
			uint32_t           id = 0;
			Shared::WeaponType type = Shared::WeaponType::None;
			Shared::WeaponPose start;          // 손에서 떨어지기 시작한 자세
			Shared::WeaponPose rest;           // 땅에 누운 자세
			float              fallSeconds = 0.0f;
			float              age = 0.0f;     // 생긴 뒤 흐른 시간(s)
			NodeHandle         node = kInvalidNode;
			WeaponView         view = WeaponView::ThirdPerson;
		};
		void SpawnGround(uint32_t id, Shared::WeaponType type, const Shared::WeaponPose& start,
			const Shared::WeaponPose& rest, float fallSeconds);
		void RemoveGround(uint32_t id);
		void ClearGround();
		const std::vector<Ground>& GroundWeapons() const { return ground; }
		static Shared::WeaponPose CurrentPose(const Ground& g);   // 떨어지는 중이면 그 순간 자세

		// 떨어지는 바닥 총을 옮기고, viewer(내 몸통 중심)에서의 거리로 1인칭 ↔ 3인칭을 바꾼다.
		void Update(float dt, const Vec3d& viewer);

		// 총 자세 → 행 벡터 월드 행렬(행 = 총의 X · Y · Z 축 · 위치).
		static DirectX::XMMATRIX PoseMatrix(const Shared::WeaponPose& pose);

	private:
		NodeHandle Acquire(Shared::WeaponType type, WeaponView view);
		void       Release(Shared::WeaponType type, WeaponView view, NodeHandle node);

		const WeaponModels* models = nullptr;
		LegacyScene*        scene = nullptr;

		// 내 손 — 종류마다 1인칭 노드 하나(내 캐릭터 노드의 자식)
		NodeHandle          localParent = kInvalidNode;
		NodeHandle          localNodes[Shared::kWeaponTypeCount] = { kInvalidNode, kInvalidNode, kInvalidNode };
		Shared::WeaponType  localType = Shared::WeaponType::None;

		// 남의 손
		struct Remote
		{
			Shared::WeaponType type = Shared::WeaponType::None;
			NodeHandle         node = kInvalidNode;
			bool               placed = false;
		};
		std::unordered_map<uint32_t, Remote> remotes;

		std::vector<Ground> ground;

		// 빈 노드 — 종류 × 시점(장면 맨 위 노드, 화면 밖에 치워 둔 것)
		std::vector<NodeHandle> freeNodes[Shared::kWeaponTypeCount][kWeaponViewCount];
	};

	static_assert(Shared::kWeaponTypeCount == 3, "총 종류 수가 바뀌면 WeaponScene::localNodes 초기값도 늘린다");

} // namespace swc
