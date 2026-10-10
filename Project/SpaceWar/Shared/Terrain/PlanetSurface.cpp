#include "PlanetSurface.h"
#include "../PlanetConst.h"
#include "../Resource/GlbDocument.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
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

	bool IsSpace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r';
	}

	std::string_view Trim(std::string_view text)
	{
		while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
		while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
		return text;
	}

	std::string_view NextToken(std::string_view& text)
	{
		text = Trim(text);
		if (text.empty() || text.front() == '#') return {};
		const size_t end = text.find_first_of(" \t\r");
		const std::string_view token = text.substr(0, end);
		text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
		return token;
	}

	// 지붕·벽까지 접지면으로 읽으면 그 아래를 걷는 플레이어도 지붕 위로 밀려난다.
	// 이 모델의 객체 이름으로 지면·도로·보도·입구만 고른다.
	bool IsGroundObject(std::string_view name)
	{
		if (name == "Planet_Core" || name == "Fragmented_Planet_Armor") return true;
		constexpr std::string_view keywords[] = {
			"Asphalt", "Sidewalk", "Paving", "Ramp", "Subgrade", "Approaches",
			"Foundations", "Curbs", "Lane_Lines", "Surface_Gravel"
		};
		for (const std::string_view keyword : keywords)
			if (name.find(keyword) != std::string_view::npos) return true;
		return false;
	}

	bool IsGlbGroundNode(std::string_view name)
	{
		if (name == Shared::kPlanetSurfaceNode) return true;
		if constexpr (Shared::kUsePlanarMap) return false;
		// 구면 자산의 보행로와 보이는 착륙 플랫폼은 지면보다 약간 높다.
		// COL_Landing 프록시는 잘못된 로컬 변환을 가지므로 선택하지 않는다.
		return name.compare(0, 14, "Walkable_Path_") == 0 ||
			(name.compare(0, 9, "Landing_L") == 0 && name.size() >= 9 &&
				name.substr(name.size() - 9) == "_Platform");
	}

	bool ParseFloat(std::string_view token, float& value)
	{
		if (!token.empty() && token.front() == '+') token.remove_prefix(1);
		if (token.empty()) return false;
		const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
		return result.ec == std::errc{} && result.ptr == token.data() + token.size() &&
			std::isfinite(value);
	}

	bool ParseIndex(std::string_view token, size_t positionCount, uint32_t& out)
	{
		// v/vt/vn, v//vn의 첫 성분만 필요하다. OBJ 인덱스는 파일 전체 기준 1부터다.
		token = token.substr(0, token.find('/'));
		if (!token.empty() && token.front() == '+') token.remove_prefix(1);
		if (token.empty()) return false;
		int64_t index = 0;
		const auto result = std::from_chars(token.data(), token.data() + token.size(), index);
		if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || index == 0)
			return false;

		if (index > 0)
		{
			if (uint64_t(index) > positionCount) return false;
			out = uint32_t(index - 1);
		}
		else
		{
			// 음수 인덱스는 현재까지 읽은 위치 배열의 끝에서 센다.
			if (index < -int64_t(positionCount)) return false;
			out = uint32_t(int64_t(positionCount) + index);
		}
		return true;
	}

} // namespace

namespace Shared {

	bool PlanetSurface::Load(const wchar_t* path, std::wstring& error)
	{
		error.clear();
		*this = PlanetSurface{};
		if (!path || !*path)
		{
			error = L"맵 모델 경로가 비어 있습니다.";
			return false;
		}

		try
		{
			std::wstring extension = std::filesystem::path(path).extension().wstring();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](wchar_t c) { return wchar_t(std::towlower(c)); });
			if (extension == L".glb") return LoadGlb(path, error);

			// UTF-16 파일 경로로 열고, 대용량 OBJ는 한 줄씩 읽는다.
			std::array<char, 64 * 1024> fileBuffer{};
			std::ifstream stream;
			stream.rdbuf()->pubsetbuf(fileBuffer.data(), std::streamsize(fileBuffer.size()));
			stream.open(std::filesystem::path(path), std::ios::binary);
			if (!stream)
			{
				error = L"행성 OBJ 파일을 열 수 없습니다.";
				return false;
			}

			PlanetSurface loaded;
			std::string line;
			std::vector<uint32_t> polygon;
			polygon.reserve(8);
			bool groundObject = false;
			bool coreObject = false;
			bool coreHasFaces = false;
			size_t lineNumber = 0;
			const auto fail = [&](const wchar_t* reason) -> bool
			{
				error = L"행성 OBJ " + std::to_wstring(lineNumber) + L"행: " + reason;
				return false;
			};

			while (std::getline(stream, line))
			{
				++lineNumber;
				std::string_view text(line);
				if (lineNumber == 1 && text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
				text = Trim(text);
				if (lineNumber == 1 && text.substr(0, 7) == "version")
					return fail(L"Git LFS 포인터입니다. 모델 원본을 내려받으세요.");
				const std::string_view type = NextToken(text);
				if (type == "o")
				{
					const std::string_view name = Trim(text.substr(0, text.find('#')));
					groundObject = IsGroundObject(name);
					coreObject = name == "Planet_Core";
				}
				else if (type == "v")
				{
					Position position{};
					if (!ParseFloat(NextToken(text), position.x) ||
						!ParseFloat(NextToken(text), position.y) ||
						!ParseFloat(NextToken(text), position.z))
						return fail(L"정점 위치가 올바르지 않습니다.");
					if (loaded.positions.size() >= std::numeric_limits<uint32_t>::max())
						return fail(L"정점 수가 지원 범위를 초과했습니다.");
					// OBJ/GLB 접지를 같은 DirectX 좌수계로 보관한다.
					position.z = -position.z;
					// 선택하지 않은 객체의 정점도 읽어야 뒤쪽 객체의 전역 인덱스가 맞는다.
					loaded.positions.push_back(position);
				}
				else if (type == "f" && groundObject)
				{
					polygon.clear();
					for (std::string_view token = NextToken(text); !token.empty(); token = NextToken(text))
					{
						uint32_t index = 0;
						if (!ParseIndex(token, loaded.positions.size(), index))
							return fail(L"면의 정점 인덱스가 올바르지 않습니다.");
						polygon.push_back(index);
					}
					if (polygon.size() < 3) return fail(L"면의 정점이 3개보다 적습니다.");

					// 이 OBJ의 삼각형·볼록 사각형은 첫 모서리 기준 fan으로 나눈다.
					for (size_t i = 1; i + 1 < polygon.size(); ++i)
					{
						const Triangle triangle{ { polygon[0], polygon[i], polygon[i + 1] } };
						const Position& a = loaded.positions[triangle.vertices[0]];
						const Position& b = loaded.positions[triangle.vertices[1]];
						const Position& c = loaded.positions[triangle.vertices[2]];
						const double bx = double(b.x) - a.x, by = double(b.y) - a.y, bz = double(b.z) - a.z;
						const double cx = double(c.x) - a.x, cy = double(c.y) - a.y, cz = double(c.z) - a.z;
						const double nx = by * cz - bz * cy;
						const double ny = bz * cx - bx * cz;
						const double nz = bx * cy - by * cx;
						if (nx * nx + ny * ny + nz * nz <= 1.0e-20) continue;
						if (loaded.triangles.size() >= std::numeric_limits<uint32_t>::max() / 2)
							return fail(L"삼각형 수가 지원 범위를 초과했습니다.");
						loaded.triangles.push_back(triangle);
						coreHasFaces = coreHasFaces || coreObject;
					}
				}
			}
			if (stream.bad()) return fail(L"파일을 읽는 도중 오류가 발생했습니다.");
			if (!coreHasFaces || loaded.triangles.empty())
				return fail(L"Planet_Core 지표면 또는 접지 삼각형이 없습니다.");

			// 파싱 중에는 OBJ 전역 인덱스가 필요하지만 조회에는 접지 정점만 필요하다.
			// remap 임시 배열은 CompactPositions가 끝나면 BVH를 만들기 전에 해제된다.
			loaded.CompactPositions();
			loaded.positions.shrink_to_fit();
			loaded.triangles.shrink_to_fit();
			loaded.nodes.reserve(NodeCount(uint32_t(loaded.triangles.size())));
			loaded.BuildNode(0, uint32_t(loaded.triangles.size()));
			*this = std::move(loaded);
			return true;
		}
		catch (const std::bad_alloc&)
		{
			error = L"맵 접지 데이터를 위한 메모리가 부족합니다.";
			return false;
		}
	}

	bool PlanetSurface::LoadGlb(const wchar_t* path, std::wstring& error)
	{
		PlanetSurface loaded;
		{
			// 렌더 장식물과 충돌용 지면을 구분한다. GLB의 BIN과 임시 정점은
			// 이 범위를 벗어나면 해제되며, 접지용 위치와 삼각형만 남는다.
			GlbDocument document;
			if (!document.Load(path, error)) return false;
			bool foundGround = false;
			std::vector<const GlbNode*> groundNodes;
			groundNodes.reserve(12);
			size_t vertexCount = 0;
			size_t triangleCount = 0;
			for (const GlbNode& node : document.Nodes())
			{
				if (!IsGlbGroundNode(node.name)) continue;
				if (node.mesh == kInvalidGlbIndex || node.mesh >= document.Meshes().size())
				{
					error = L"GLB 접지 노드의 메시가 올바르지 않습니다.";
					return false;
				}
				foundGround = foundGround || node.name == kPlanetSurfaceNode;
				for (const GlbPrimitiveInfo& primitive : document.Meshes()[node.mesh].primitives)
				{
					if (primitive.vertexCount > std::numeric_limits<uint32_t>::max() - vertexCount ||
						primitive.indexCount / 3 > std::numeric_limits<uint32_t>::max() / 2 - triangleCount)
					{
						error = L"GLB 접지 정점 또는 삼각형 수가 지원 범위를 초과했습니다.";
						return false;
					}
					vertexCount += primitive.vertexCount;
					triangleCount += primitive.indexCount / 3;
				}
				groundNodes.push_back(&node);
			}
			// 도로만 있는 파일을 행성 지면으로 잘못 받아들이지 않는다.
			if (!foundGround || triangleCount == 0)
			{
				error = L"GLB에 지정된 행성 접지 노드 또는 지면 삼각형이 없습니다.";
				return false;
			}
			// 노드별 변환은 서로 다르므로 접지 위치는 각 인스턴스 좌표로 저장한다.
			// 전체 개수를 먼저 합산해 대형 배열의 반복 재할당·복사를 피한다.
			loaded.positions.reserve(vertexCount);
			loaded.triangles.reserve(triangleCount);
			for (const GlbNode* groundNode : groundNodes)
			{
				const GlbNode& node = *groundNode;
				const GlbMesh& mesh = document.Meshes()[node.mesh];
				for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
				{
					GlbGeometry primitive;
					if (!document.ReadGeometry(node.mesh, uint32_t(primitiveIndex), primitive, error))
						return false;
					if (primitive.indices.size() % 3 != 0 || primitive.positions.size() >
						std::numeric_limits<uint32_t>::max() - loaded.positions.size())
					{
						error = L"GLB 접지 정점 또는 삼각형 수가 지원 범위를 초과했습니다.";
						return false;
					}
					if (primitive.indices.size() / 3 > std::numeric_limits<uint32_t>::max() / 2 -
						loaded.triangles.size())
					{
						error = L"GLB 접지 삼각형 수가 지원 범위를 초과했습니다.";
						return false;
					}
					const uint32_t firstVertex = uint32_t(loaded.positions.size());
					const auto& m = node.world;
					for (const auto& p : primitive.positions)
					{
						// 리더가 좌수계로 바꾼 좌표에 노드의 비균등 스케일·회전·이동을
						// 한 번 적용한다. 렌더 맵과 같은 자산 좌표의 BVH를 만든다.
						// 구면 표시 배율·중심 이동은 Height 반환과 Planet.PositionAt에서 적용한다.
						const Position position{
							p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12],
							p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
							p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14]
						};
						if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
							!std::isfinite(position.z))
						{
							error = L"GLB 접지 정점의 월드 위치가 유효하지 않습니다.";
							return false;
						}
						loaded.positions.push_back(position);
					}
					for (size_t i = 0; i < primitive.indices.size(); i += 3)
					{
						Triangle triangle{};
						for (size_t corner = 0; corner < 3; ++corner)
						{
							const uint32_t vertex = primitive.indices[i + corner];
							if (vertex >= primitive.positions.size())
							{
								error = L"GLB 접지 삼각형의 인덱스가 올바르지 않습니다.";
								return false;
							}
							triangle.vertices[corner] = firstVertex + vertex;
						}
						const Position& a = loaded.positions[triangle.vertices[0]];
						const Position& b = loaded.positions[triangle.vertices[1]];
						const Position& c = loaded.positions[triangle.vertices[2]];
						const double bx = double(b.x) - a.x, by = double(b.y) - a.y, bz = double(b.z) - a.z;
						const double cx = double(c.x) - a.x, cy = double(c.y) - a.y, cz = double(c.z) - a.z;
						if constexpr (kUsePlanarMap)
						{
							// 평면 높이 조회에서 수직 벽은 지면을 만들지 못한다.
							if (std::fabs(bx * cz - bz * cx) <= 1.0e-12) continue;
						}
						else
						{
							// 구체 옆면도 보행면이다. X/Z 투영 대신 3D 면적만 검사한다.
							const double nx = by * cz - bz * cy;
							const double ny = bz * cx - bx * cz;
							const double nz = bx * cy - by * cx;
							if (nx * nx + ny * ny + nz * nz <= 1.0e-20) continue;
						}
						if (loaded.triangles.size() >= std::numeric_limits<uint32_t>::max() / 2)
						{
							error = L"GLB 접지 삼각형 수가 지원 범위를 초과했습니다.";
							return false;
						}
						loaded.triangles.push_back(triangle);
					}
				}
			}
			if (loaded.triangles.empty())
			{
				error = L"GLB에 지정된 접지 노드 또는 유효한 지면 삼각형이 없습니다.";
				return false;
			}
		}
		loaded.CompactPositions();
		loaded.positions.shrink_to_fit();
		loaded.triangles.shrink_to_fit();
		loaded.nodes.reserve(NodeCount(uint32_t(loaded.triangles.size())));
		loaded.BuildNode(0, uint32_t(loaded.triangles.size()));
		*this = std::move(loaded);
		return true;
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

		// OBJ/GLB 접지를 이미 같은 좌수계로 보관하므로 조회 방향을 다시 반전하지 않는다.
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

	double PlanetSurface::HeightAt(double worldX, double worldZ) const
	{
		if (!Valid() || !std::isfinite(worldX) || !std::isfinite(worldZ)) return 0.0;
		double bestHeight = -std::numeric_limits<double>::infinity();
		std::array<uint32_t, 64> stack{};
		size_t stackSize = 1;
		stack[0] = 0;
		constexpr double tolerance = 1.0e-8;
		while (stackSize != 0)
		{
			const Node& node = nodes[stack[--stackSize]];
			if (worldX < double(node.lo.x) - tolerance || worldX > double(node.hi.x) + tolerance ||
				worldZ < double(node.lo.z) - tolerance || worldZ > double(node.hi.z) + tolerance ||
				node.hi.y <= bestHeight) continue;
			if (node.count != 0)
			{
				for (uint32_t i = node.first; i < node.first + node.count; ++i)
				{
					const Triangle& triangle = triangles[i];
					const Position& a = positions[triangle.vertices[0]];
					const Position& b = positions[triangle.vertices[1]];
					const Position& c = positions[triangle.vertices[2]];
					const double bx = double(b.x) - a.x, bz = double(b.z) - a.z;
					const double cx = double(c.x) - a.x, cz = double(c.z) - a.z;
					const double determinant = bx * cz - bz * cx;
					if (std::fabs(determinant) <= 1.0e-12) continue;
					const double dx = worldX - a.x, dz = worldZ - a.z;
					const double u = (dx * cz - dz * cx) / determinant;
					const double v = (bx * dz - bz * dx) / determinant;
					if (u < -tolerance || v < -tolerance || u + v > 1.0 + tolerance) continue;
					const double height = a.y + u * (double(b.y) - a.y) + v * (double(c.y) - a.y);
					bestHeight = std::max(bestHeight, height);
				}
			}
			else
			{
				stack[stackSize++] = node.left;
				stack[stackSize++] = node.right;
			}
		}
		return std::isfinite(bestHeight) ? bestHeight : 0.0;
	}

} // namespace Shared
