#include "ColliderDebugView.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Shared/GameLogic/PlayerMovement.h"   // 캡슐 크기 · kGroundOffset(PlanetConst.h)

namespace swc {

	namespace {

		uint32_t ColorOf(Shared::CollisionClass cls)
		{
			switch (cls)
			{
			case Shared::CollisionClass::Building: return ColliderDebugView::kBuildingColor;
			case Shared::CollisionClass::Rock:     return ColliderDebugView::kRockColor;
			case Shared::CollisionClass::Tree:     return ColliderDebugView::kTreeColor;
			default:                               return ColliderDebugView::kPropColor;
			}
		}

	} // namespace

	void ColliderDebugView::Clear()
	{
		built = nullptr;
		valid = false;
		vertices.clear();
		query.clear();
		staticVertexCount = 0;
		instanceCount = 0;
		capsuleCount = 0;
		truncated = false;
	}

	void ColliderDebugView::ClearCapsules()
	{
		vertices.resize(staticVertexCount);
		capsuleCount = 0;
	}

	/////////////////////////////////////////////////////////////////////
	//  캡슐 선 — Shared::PlayerMovement 와 같은 모양.
	//   아래 구 중심 = 몸통 중심 − up × (kGroundOffset − r), 위 구 중심 = 몸통 중심 + up × (키 − kGroundOffset − r).
	//   두 구 중심에 고리, 그 사이 세로선 4개, 위 · 아래 반구에 서로 직각인 반원 둘씩.
	void ColliderDebugView::AddCapsule(const Vec3d& mapPosition, const Vec3d& up, uint32_t color)
	{
		if (vertices.size() + kCapsuleVertices > kMaxVertices)
		{
			truncated = true;
			return;
		}

		const double r = Shared::PlayerMovement::kCapsuleRadius;
		const Vec3d bottom = mapPosition - up * (Shared::kGroundOffset - r);
		const Vec3d top = mapPosition + up * (double(Shared::PlayerMovement::kCapsuleHeight) - Shared::kGroundOffset - r);

		// up 에 수직인 두 축 — up 과 나란하지 않은 아무 벡터에서 만든다
		const Vec3d seed = std::fabs(up.z) < 0.9 ? Vec3d(0.0, 0.0, 1.0) : Vec3d(1.0, 0.0, 0.0);
		const Vec3d axisX = Normalize(Cross(up, seed));
		const Vec3d axisY = Cross(up, axisX);

		auto line = [&](const Vec3d& a, const Vec3d& b)
		{
			vertices.push_back({ ToFloat3(a), color });
			vertices.push_back({ ToFloat3(b), color });
		};

		constexpr double kTwoPi = 6.283185307179586;
		const double step = kTwoPi / double(kCapsuleSegments);
		for (int i = 0; i < kCapsuleSegments; ++i)
		{
			const double a0 = step * double(i), a1 = step * double(i + 1);
			const Vec3d d0 = axisX * std::cos(a0) + axisY * std::sin(a0);
			const Vec3d d1 = axisX * std::cos(a1) + axisY * std::sin(a1);
			line(bottom + d0 * r, bottom + d1 * r);   // 아래 고리
			line(top + d0 * r, top + d1 * r);         // 위 고리
		}

		const Vec3d sides[4] = { axisX, axisY, axisX * -1.0, axisY * -1.0 };
		for (const Vec3d& side : sides)
			line(bottom + side * r, top + side * r);  // 세로선

		// 반구 — 반원 하나는 고리 마디의 절반. 위는 up 쪽, 아래는 −up 쪽으로 둥글다.
		const int half = kCapsuleSegments / 2;
		const Vec3d planes[2] = { axisX, axisY };
		for (const Vec3d& planeAxis : planes)
		{
			for (int i = 0; i < half; ++i)
			{
				const double a0 = step * double(i), a1 = step * double(i + 1);
				line(top + (planeAxis * std::cos(a0) + up * std::sin(a0)) * r,
					 top + (planeAxis * std::cos(a1) + up * std::sin(a1)) * r);
				line(bottom + (planeAxis * std::cos(a0) - up * std::sin(a0)) * r,
					 bottom + (planeAxis * std::cos(a1) - up * std::sin(a1)) * r);
			}
		}
		++capsuleCount;
	}

	void ColliderDebugView::Update(const Shared::CollisionWorld* world, const Vec3d& eye, bool force)
	{
		if (!world || !world->Loaded() || world->InstanceCount() == 0)
		{
			Clear();
			built = world;
			return;
		}
		if (!force && valid && built == world && Length(eye - lastEye) < kRebuildDistance)
			return;
		built = world;
		lastEye = eye;
		valid = true;
		vertices.clear();
		staticVertexCount = 0;
		capsuleCount = 0;
		truncated = false;

		const Shared::Vec3 center{ float(eye.x), float(eye.y), float(eye.z) };
		world->QuerySphere(center, kRadius, query);

		// 가까운 것부터 — 상한을 넘으면 먼 것이 빠진다
		auto distanceSq = [&](uint32_t index)
		{
			const Shared::CollisionInstance& instance = world->Instance(index);
			const float dx = (std::max)({ instance.boundsMin.x - center.x, 0.0f, center.x - instance.boundsMax.x });
			const float dy = (std::max)({ instance.boundsMin.y - center.y, 0.0f, center.y - instance.boundsMax.y });
			const float dz = (std::max)({ instance.boundsMin.z - center.z, 0.0f, center.z - instance.boundsMax.z });
			return dx * dx + dy * dy + dz * dz;
		};
		std::vector<std::pair<float, uint32_t>> order;
		order.reserve(query.size());
		for (const uint32_t index : query) order.emplace_back(distanceSq(index), index);
		std::sort(order.begin(), order.end());

		instanceCount = 0;
		for (const auto& [unused, index] : order)
		{
			const Shared::CollisionInstance& instance = world->Instance(index);
			const Shared::CollisionMesh& mesh = world->Mesh(instance.mesh);
			if (vertices.size() + mesh.edges.size() > kMaxVertices - kCapsuleReserve)   // 캡슐 몫을 남긴다
			{
				truncated = true;
				break;
			}
			const uint32_t color = ColorOf(instance.cls);
			for (size_t e = 0; e + 1 < mesh.edges.size(); e += 2)
			{
				const Shared::Vec3 a = Shared::CollisionWorld::Transform(instance, mesh.vertices[mesh.edges[e]]);
				const Shared::Vec3 b = Shared::CollisionWorld::Transform(instance, mesh.vertices[mesh.edges[e + 1]]);
				vertices.push_back({ DirectX::XMFLOAT3(a.x, a.y, a.z), color });
				vertices.push_back({ DirectX::XMFLOAT3(b.x, b.y, b.z), color });
			}
			++instanceCount;
		}
		staticVertexCount = vertices.size();
	}

} // namespace swc
