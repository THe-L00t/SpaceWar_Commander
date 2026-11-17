#include "Client/Weapon/WeaponScene.h"

#include <algorithm>
#include "Client/LegacyScene.h"

using namespace DirectX;

// 머리 주석은 WeaponScene.h.

namespace swc {

	namespace {

		XMMATRIX Parked()
		{
			return XMMatrixTranslation(0.0f, -1.0e7f, 0.0f);   // Engine ParkedTransform 과 같은 자리(화면 밖)
		}

		// 손 자리 — 캐릭터 몸 공간(행 = 오른쪽 · 위 · 앞, PlayerController::WorldMatrix · CharacterTransform 과 같은 배치).
		//  총 축 = 몸 축(총구 = 앞). Shared::HeldWeaponPose 와 같은 값이다.
		XMMATRIX HeldLocal()
		{
			return XMMatrixTranslation(float(Shared::kHeldWeaponRight), float(Shared::kHeldWeaponUp),
				float(Shared::kHeldWeaponForward));
		}

		Vec3d RowOf(const XMMATRIX& m, int row)
		{
			XMFLOAT3 v;
			XMStoreFloat3(&v, m.r[row]);
			return Vec3d(double(v.x), double(v.y), double(v.z));
		}
	}

	void WeaponScene::Bind(const WeaponModels* weaponModels, LegacyScene* legacyScene)
	{
		models = weaponModels;
		scene = legacyScene;
	}

	XMMATRIX WeaponScene::PoseMatrix(const Shared::WeaponPose& pose)
	{
		XMMATRIX m;
		m.r[0] = XMVectorSet(float(pose.axisX.x), float(pose.axisX.y), float(pose.axisX.z), 0.0f);
		m.r[1] = XMVectorSet(float(pose.axisY.x), float(pose.axisY.y), float(pose.axisY.z), 0.0f);
		m.r[2] = XMVectorSet(float(pose.axisZ.x), float(pose.axisZ.y), float(pose.axisZ.z), 0.0f);
		m.r[3] = XMVectorSet(float(pose.position.x), float(pose.position.y), float(pose.position.z), 1.0f);
		return m;
	}

	// ── 노드 빌려 쓰기 ──
	NodeHandle WeaponScene::Acquire(Shared::WeaponType type, WeaponView view)
	{
		if (!models || !scene || !Shared::IsWeaponType(type)) return kInvalidNode;
		std::vector<NodeHandle>& pool = freeNodes[size_t(type)][size_t(view)];
		NodeHandle node = kInvalidNode;
		if (!pool.empty())
		{
			node = pool.back();
			pool.pop_back();
		}
		else
		{
			node = models->Instantiate(type, view, *scene);   // 모델을 아직 못 올렸으면 kInvalidNode — 그 총은 안 그린다
		}
		if (node != kInvalidNode) scene->SetVisible(node, true);
		return node;
	}

	void WeaponScene::Release(Shared::WeaponType type, WeaponView view, NodeHandle node)
	{
		if (node == kInvalidNode || !scene || !Shared::IsWeaponType(type)) return;
		scene->SetLocalTransform(node, Parked());
		scene->SetVisible(node, false);
		freeNodes[size_t(type)][size_t(view)].push_back(node);
	}

	// ── 내 손 ──
	void WeaponScene::SetLocalHeld(Shared::WeaponType type, NodeHandle playerNode)
	{
		if (!scene || !models || playerNode == kInvalidNode) return;
		if (type == localType && playerNode == localParent) return;

		if (Shared::IsWeaponType(localType) && localNodes[size_t(localType)] != kInvalidNode)
			scene->SetVisible(localNodes[size_t(localType)], false);

		localType = type;
		localParent = playerNode;
		if (!Shared::IsWeaponType(type)) return;

		NodeHandle& node = localNodes[size_t(type)];
		if (node == kInvalidNode)
		{
			node = models->Instantiate(type, WeaponView::FirstPerson, *scene, playerNode);
			if (node == kInvalidNode) return;
			scene->SetLocalTransform(node, HeldLocal());
		}
		scene->SetVisible(node, true);
	}

	// ── 남의 손 ──
	void WeaponScene::BeginRemoteFrame()
	{
		for (auto& entry : remotes) entry.second.placed = false;
	}

	void WeaponScene::PlaceRemote(uint32_t playerId, Shared::WeaponType type, const XMMATRIX& character)
	{
		Remote& r = remotes[playerId];
		r.placed = true;
		if (r.type != type)
		{
			Release(r.type, WeaponView::ThirdPerson, r.node);
			r.node = kInvalidNode;
			r.type = type;
			if (Shared::IsWeaponType(type))
				r.node = Acquire(type, WeaponView::ThirdPerson);
		}
		if (r.node != kInvalidNode && scene)
			scene->SetLocalTransform(r.node, HeldLocal() * character);
	}

	void WeaponScene::EndRemoteFrame()
	{
		for (auto it = remotes.begin(); it != remotes.end(); )
		{
			if (it->second.placed) { ++it; continue; }
			Release(it->second.type, WeaponView::ThirdPerson, it->second.node);
			it = remotes.erase(it);
		}
	}

	void WeaponScene::ClearRemotes()
	{
		for (auto& entry : remotes)
			Release(entry.second.type, WeaponView::ThirdPerson, entry.second.node);
		remotes.clear();
	}

	// ── 바닥 ──
	void WeaponScene::SpawnGround(uint32_t id, Shared::WeaponType type, const Shared::WeaponPose& start,
		const Shared::WeaponPose& rest, float fallSeconds)
	{
		if (!Shared::IsWeaponType(type)) return;
		RemoveGround(id);   // 같은 번호가 또 오면(맵에 다시 들어옴) 새 것으로
		Ground g;
		g.id = id;
		g.type = type;
		g.start = start;
		g.rest = rest;
		g.fallSeconds = (std::max)(fallSeconds, 0.0f);
		ground.push_back(g);
	}

	void WeaponScene::RemoveGround(uint32_t id)
	{
		for (size_t i = 0; i < ground.size(); ++i)
		{
			if (ground[i].id != id) continue;
			Release(ground[i].type, ground[i].view, ground[i].node);
			ground.erase(ground.begin() + std::ptrdiff_t(i));
			return;
		}
	}

	void WeaponScene::ClearGround()
	{
		for (const Ground& g : ground)
			Release(g.type, g.view, g.node);
		ground.clear();
	}

	// 떨어지는 동안 — 위치는 자유 낙하처럼(시간²), 자세는 손 자세 → 누운 자세로 고르게 돌린다. 다 떨어지면 누운 자세.
	Shared::WeaponPose WeaponScene::CurrentPose(const Ground& g)
	{
		if (g.fallSeconds <= 0.0f || g.age >= g.fallSeconds) return g.rest;
		const float t = (std::max)(0.0f, g.age / g.fallSeconds);
		const double s = double(t) * double(t);

		Shared::WeaponPose pose;
		pose.position = g.start.position + (g.rest.position - g.start.position) * s;

		const XMVECTOR q0 = XMQuaternionRotationMatrix(PoseMatrix(g.start));
		const XMVECTOR q1 = XMQuaternionRotationMatrix(PoseMatrix(g.rest));
		const XMMATRIX r = XMMatrixRotationQuaternion(XMQuaternionSlerp(q0, q1, t));
		pose.axisX = RowOf(r, 0);
		pose.axisY = RowOf(r, 1);
		pose.axisZ = RowOf(r, 2);
		return pose;
	}

	void WeaponScene::Update(float dt, const Vec3d& viewer)
	{
		if (!scene) return;
		for (Ground& g : ground)
		{
			g.age += dt;
			const Shared::WeaponPose pose = CurrentPose(g);
			const WeaponView want = Length(pose.position - viewer) <= kGroundFirstPersonDistance
				? WeaponView::FirstPerson : WeaponView::ThirdPerson;
			if (g.node == kInvalidNode || g.view != want)
			{
				Release(g.type, g.view, g.node);
				g.view = want;
				g.node = Acquire(g.type, want);
			}
			if (g.node != kInvalidNode)
				scene->SetLocalTransform(g.node, PoseMatrix(pose));
		}
	}

} // namespace swc
