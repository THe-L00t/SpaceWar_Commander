#include "ColliderDebugView.h"

#include <algorithm>
#include <utility>

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
		instanceCount = 0;
		truncated = false;
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
			if (vertices.size() + mesh.edges.size() > kMaxVertices)
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
	}

} // namespace swc
