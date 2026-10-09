#include "PlanetSurface.h"
#include "../PlanetConst.h"
// ★ OBJ 파싱을 직접 하지 않는다 — 저장소의 모델 파서는 Shared/Model 하나다(2026-10-08).
#include "../Model/ModelReader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

	constexpr uint32_t kLeafTriangles = 8;

	size_t NodeCount(uint32_t triangleCount)
	{
		// BuildNode는 매번 절반으로 나눈다. 마지막 층에서 9개가 남는
		// 구간만 한 번 더 나누므로, 실제 생성될 노드 수를 미리 구할 수 있다.
		size_t leafCount = 1;
		while (triangleCount / leafCount > kLeafTriangles) leafCount *= 2;
		if (triangleCount / leafCount == kLeafTriangles)
			leafCount += triangleCount % leafCount;
		return leafCount * 2 - 1;
	}

} // namespace

namespace Shared {

	bool PlanetSurface::IsGroundObject(std::string_view name)
	{
		// 이 모델의 객체 이름으로 지면·도로·보도·입구만 고른다.
		if (name == "Planet_Core" || name == "Fragmented_Planet_Armor") return true;
		constexpr std::string_view keywords[] = {
			"Asphalt", "Sidewalk", "Paving", "Ramp", "Subgrade", "Approaches",
			"Foundations", "Curbs", "Lane_Lines", "Surface_Gravel"
		};
		for (const std::string_view keyword : keywords)
			if (name.find(keyword) != std::string_view::npos) return true;
		return false;
	}

	bool PlanetSurface::Load(const wchar_t* path, std::wstring& error)
	{
		error.clear();
		*this = PlanetSurface{};
		if (!path || !*path)
		{
			error = L"행성 모델 경로가 비어 있습니다.";
			return false;
		}

		// ★ 파싱은 Shared::ModelReader 가 한다 (2026-10-08 통합)
		//   OBJ 를 직접 읽던 코드(ifstream + 자체 토크나이저)를 지웠다 —
		//   저장소에 모델 파서가 둘이 되지 않게 한다. 지원 포맷이 늘면 이 클래스도 따라온다.
		//   여기 남는 것은 파싱이 아니라 «맵 규약» 이다: 어떤 오브젝트를 접지면으로 볼 것인가.
		ReadOptions options;
		options.geometryOnly = true;        // 재질·UV·탄젠트를 읽지 않는다(서버도 같은 길)
		options.generateTangents = false;
		options.generateCollision = false;  // 접지 삼각형은 collectObject 로 «고른 오브젝트» 만 모은다
		options.readAnimation = false;
		// ★ 오브젝트를 노드로 쪼개지 않는다 (2026-10-09)
		//   예전에는 splitByObject 로 쪼갠 뒤 노드 이름으로 골랐다. 이제 리더가 o/g 이름을 보며
		//   바로 골라 collected 에 모은다 — 클라가 렌더용 파싱 한 번으로 같은 결과를 얻는 길과 같다.
		options.collectObject = &PlanetSurface::IsGroundObject;

		ModelSource source;
		if (!ModelReader().Load(path, source, error, options))
			return false;

		return Build(source.collected, error);
	}

	bool PlanetSurface::Build(const CollectedGeometry& ground, std::wstring& error)
	{
		error.clear();
		*this = PlanetSurface{};

		try
		{
			PlanetSurface built;

			const bool coreHasFaces = std::find(ground.objects.begin(), ground.objects.end(),
				std::string("Planet_Core")) != ground.objects.end();

			if (ground.positions.size() >= std::numeric_limits<uint32_t>::max())
			{
				error = L"행성 접지 정점 수가 지원 범위를 초과했습니다.";
				return false;
			}
			built.positions.reserve(ground.positions.size());
			for (const Vec3& p : ground.positions)
				built.positions.push_back({ p.x, p.y, p.z });

			// 리더가 삼각형 목록을 보장한다(다각형 분할·인덱스 범위 검사까지 끝난 상태).
			built.triangles.reserve(ground.indices.size() / 3);
			for (size_t i = 0; i + 2 < ground.indices.size(); i += 3)
			{
				const Triangle triangle{ {
					ground.indices[i], ground.indices[i + 1], ground.indices[i + 2] } };
				if (triangle.vertices[0] >= built.positions.size() ||
					triangle.vertices[1] >= built.positions.size() ||
					triangle.vertices[2] >= built.positions.size())
				{
					error = L"행성 접지 삼각형의 정점 번호가 범위를 벗어났습니다.";
					return false;
				}
				const Position& a = built.positions[triangle.vertices[0]];
				const Position& b = built.positions[triangle.vertices[1]];
				const Position& c = built.positions[triangle.vertices[2]];
				const double bx = double(b.x) - a.x, by = double(b.y) - a.y, bz = double(b.z) - a.z;
				const double cx = double(c.x) - a.x, cy = double(c.y) - a.y, cz = double(c.z) - a.z;
				const double nx = by * cz - bz * cy;
				const double ny = bz * cx - bx * cz;
				const double nz = bx * cy - by * cx;
				if (nx * nx + ny * ny + nz * nz <= 1.0e-20) continue;   // 퇴화 삼각형
				if (built.triangles.size() >= std::numeric_limits<uint32_t>::max() / 2)
				{
					error = L"행성 접지 삼각형 수가 지원 범위를 초과했습니다.";
					return false;
				}
				built.triangles.push_back(triangle);
			}

			if (!coreHasFaces || built.triangles.empty())
			{
				error = L"Planet_Core 지표면 또는 접지 삼각형이 없습니다.";
				return false;
			}

			// 리더는 고른 면이 쓰는 위치만 넘긴다. 퇴화 삼각형을 뺀 뒤 남는 위치만 다시 추린다.
			built.CompactPositions();
			built.positions.shrink_to_fit();
			built.triangles.shrink_to_fit();
			built.nodes.reserve(NodeCount(uint32_t(built.triangles.size())));
			built.BuildNode(0, uint32_t(built.triangles.size()));
			*this = std::move(built);
			return true;
		}
		catch (const std::bad_alloc&)
		{
			error = L"행성 접지 데이터를 위한 메모리가 부족합니다.";
			return false;
		}
	}

	void PlanetSurface::CompactPositions()
	{
		constexpr uint32_t unused = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> remap(positions.size(), unused);
		for (const Triangle& triangle : triangles)
			for (const uint32_t vertex : triangle.vertices)
				remap[vertex] = 0;

		uint32_t nextIndex = 0;
		for (size_t index = 0; index < positions.size(); ++index)
		{
			if (remap[index] == unused) continue;
			remap[index] = nextIndex;
			positions[nextIndex++] = positions[index];
		}
		for (Triangle& triangle : triangles)
			for (uint32_t& vertex : triangle.vertices)
				vertex = remap[vertex];
		positions.resize(nextIndex);
	}

	uint32_t PlanetSurface::BuildNode(uint32_t first, uint32_t count)
	{
		const float infinity = std::numeric_limits<float>::infinity();
		Node node;
		node.lo = { infinity, infinity, infinity };
		node.hi = { -infinity, -infinity, -infinity };
		Position centroidLo = node.lo;
		Position centroidHi = node.hi;
		for (uint32_t i = first; i < first + count; ++i)
		{
			Position centroid{};
			for (const uint32_t vertex : triangles[i].vertices)
			{
				const Position& p = positions[vertex];
				node.lo.x = std::min(node.lo.x, p.x); node.hi.x = std::max(node.hi.x, p.x);
				node.lo.y = std::min(node.lo.y, p.y); node.hi.y = std::max(node.hi.y, p.y);
				node.lo.z = std::min(node.lo.z, p.z); node.hi.z = std::max(node.hi.z, p.z);
				centroid.x += p.x / 3.0f; centroid.y += p.y / 3.0f; centroid.z += p.z / 3.0f;
			}
			centroidLo.x = std::min(centroidLo.x, centroid.x); centroidHi.x = std::max(centroidHi.x, centroid.x);
			centroidLo.y = std::min(centroidLo.y, centroid.y); centroidHi.y = std::max(centroidHi.y, centroid.y);
			centroidLo.z = std::min(centroidLo.z, centroid.z); centroidHi.z = std::max(centroidHi.z, centroid.z);
		}

		const uint32_t index = uint32_t(nodes.size());
		nodes.push_back(node);
		if (count <= kLeafTriangles)
		{
			nodes[index].first = first;
			nodes[index].count = count;
			return index;
		}

		const float extent[3] = {
			centroidHi.x - centroidLo.x, centroidHi.y - centroidLo.y, centroidHi.z - centroidLo.z
		};
		int axis = extent[1] > extent[0] ? 1 : 0;
		if (extent[2] > extent[axis]) axis = 2;
		const auto centroid = [&](const Triangle& triangle) -> double
		{
			double value = 0.0;
			for (const uint32_t vertex : triangle.vertices)
			{
				const Position& p = positions[vertex];
				value += axis == 0 ? p.x : axis == 1 ? p.y : p.z;
			}
			return value;   // 비교만 하므로 1/3은 생략한다.
		};
		const uint32_t leftCount = count / 2;
		std::nth_element(triangles.begin() + first, triangles.begin() + first + leftCount,
			triangles.begin() + first + count,
			[&](const Triangle& a, const Triangle& b) { return centroid(a) < centroid(b); });

		// 중간값으로 나누므로 최대 깊이는 32보다 작다. 프레임 조회 때 재귀하지 않는다.
		const uint32_t left = BuildNode(first, leftCount);
		const uint32_t right = BuildNode(first + leftCount, count - leftCount);
		nodes[index].left = left;
		nodes[index].right = right;
		return index;
	}

	bool PlanetSurface::Intersects(const Node& node, const double direction[3], double bestDistance) const
	{
		const double lo[3] = { node.lo.x, node.lo.y, node.lo.z };
		const double hi[3] = { node.hi.x, node.hi.y, node.hi.z };
		double nearDistance = 0.0;
		double farDistance = std::numeric_limits<double>::infinity();
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::fabs(direction[axis]) < 1.0e-15)
			{
				if (lo[axis] > 0.0 || hi[axis] < 0.0) return false;
				continue;
			}
			double a = lo[axis] / direction[axis];
			double b = hi[axis] / direction[axis];
			if (a > b) std::swap(a, b);
			nearDistance = std::max(nearDistance, a);
			farDistance = std::min(farDistance, b);
			if (nearDistance > farDistance) return false;
		}
		return farDistance > 0.0 && farDistance >= bestDistance;
	}

	double PlanetSurface::IntersectTriangle(const Triangle& triangle, const double direction[3]) const
	{
		const Position& a = positions[triangle.vertices[0]];
		const Position& b = positions[triangle.vertices[1]];
		const Position& c = positions[triangle.vertices[2]];
		const double bx = double(b.x) - a.x, by = double(b.y) - a.y, bz = double(b.z) - a.z;
		const double cx = double(c.x) - a.x, cy = double(c.y) - a.y, cz = double(c.z) - a.z;
		const double px = direction[1] * cz - direction[2] * cy;
		const double py = direction[2] * cx - direction[0] * cz;
		const double pz = direction[0] * cy - direction[1] * cx;
		const double determinant = bx * px + by * py + bz * pz;
		if (std::fabs(determinant) < 1.0e-12) return 0.0;
		const double inverse = 1.0 / determinant;
		const double u = (-double(a.x) * px - double(a.y) * py - double(a.z) * pz) * inverse;
		constexpr double tolerance = 1.0e-8;
		if (u < -tolerance || u > 1.0 + tolerance) return 0.0;
		const double qx = -double(a.y) * bz + double(a.z) * by;
		const double qy = -double(a.z) * bx + double(a.x) * bz;
		const double qz = -double(a.x) * by + double(a.y) * bx;
		const double v = (direction[0] * qx + direction[1] * qy + direction[2] * qz) * inverse;
		if (v < -tolerance || u + v > 1.0 + tolerance) return 0.0;
		const double distance = (cx * qx + cy * qy + cz * qz) * inverse;
		return distance > 0.0 ? distance : 0.0;
	}

	double PlanetSurface::Height(double upX, double upY, double upZ, double planetRadius) const
	{
		if (!Valid() || !std::isfinite(planetRadius) || planetRadius <= 0.0) return 0.0;
		const double length = std::sqrt(upX * upX + upY * upY + upZ * upZ);
		if (!std::isfinite(length) || length <= 1.0e-12) return 0.0;

		// ★ Z 를 반전하지 않는다 (2026-10-08 통합)
		//   client2 의 ObjModelLoader 는 렌더 정점의 Z 를 뒤집어 올렸고, 그래서 여기서도
		//   질의 방향의 Z 를 되뒤집어 «원본 OBJ 공간» 으로 맞췄다.
		//   지금 파서(Shared/Model)는 좌표를 변환하지 않는다 — 렌더 메시와 이 접지 삼각형이
		//   **같은 공간**에 있다. 여기서 Z 를 뒤집으면 보이는 지면과 밟는 지면이 Z 축으로 어긋난다.
		//   (파서가 좌표를 그대로 두는 이유는 Readers/GltfReader.cpp 머리의 «좌표계» 주석에 있다)
		const double direction[3] = { upX / length, upY / length, upZ / length };
		double bestDistance = 0.0;
		std::array<uint32_t, 64> stack{};
		size_t stackSize = 1;
		stack[0] = 0;
		while (stackSize != 0)
		{
			const Node& node = nodes[stack[--stackSize]];
			if (!Intersects(node, direction, bestDistance)) continue;
			if (node.count != 0)
			{
				for (uint32_t i = node.first; i < node.first + node.count; ++i)
					bestDistance = std::max(bestDistance, IntersectTriangle(triangles[i], direction));
			}
			else
			{
				stack[stackSize++] = node.left;
				stack[stackSize++] = node.right;
			}
		}
		return bestDistance > 0.0
			? bestDistance * (planetRadius / kPlanetModelReferenceRadius) - planetRadius : 0.0;
	}

} // namespace Shared
