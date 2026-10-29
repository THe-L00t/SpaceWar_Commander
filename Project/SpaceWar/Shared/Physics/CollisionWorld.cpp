#include "CollisionWorld.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "../Model/ModelReader.h"
#include "../Model/Readers/JsonValue.h"

// 머리 주석은 CollisionWorld.h. 데이터를 만드는 쪽은 Resource_Work/_tools/collision/col_build.py,
// 검증은 같은 폴더의 col_verify.py · col_verify2.py(원본 glb 좌표와 겹침 대조).

namespace Shared {

	namespace {

		constexpr uint32_t kNone = 0xFFFFFFFFu;
		constexpr double   kWeldEpsilon = 1.0e-4;          // m — col_build.py weld 와 같다
		constexpr float    kFeatureCos = 0.985f;           // 이웃 두 면 법선이 이보다 가까우면(약 10° 안) 같은 평면 — 그 모서리는 선을 긋지 않는다
		constexpr int64_t  kMaxCellsPerInstance = 512;     // 이보다 많은 격자 칸을 덮는 인스턴스는 large 목록(늘 후보)
		constexpr int64_t  kCellBias = int64_t(1) << 20;   // 칸 번호를 21비트로 담을 때의 치우침(±16,000 km 까지)

		std::wstring FromUtf8(const std::string& text)
		{
			try
			{
				return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
			}
			catch (...)
			{
				return std::wstring(text.begin(), text.end());
			}
		}

		bool ReadText(const wchar_t* path, std::string& out)
		{
			std::ifstream file(std::filesystem::path(path), std::ios::binary);
			if (!file) return false;
			out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			return true;
		}

		CollisionClass ClassOf(const std::string& name)
		{
			if (name == "rock") return CollisionClass::Rock;
			if (name == "building") return CollisionClass::Building;
			if (name == "tree") return CollisionClass::Tree;
			return CollisionClass::Prop;
		}

		// 행 벡터 규약 행렬 곱 — GltfReader.cpp Multiply 와 같은 식(노드 월드 = 로컬 · 부모 월드)
		Mat4 Multiply(const Mat4& a, const Mat4& b)
		{
			Mat4 r;
			for (int row = 0; row < 4; ++row)
				for (int col = 0; col < 4; ++col)
				{
					float sum = 0.0f;
					for (int k = 0; k < 4; ++k)
						sum += a.m[row * 4 + k] * b.m[k * 4 + col];
					r.m[row * 4 + col] = sum;
				}
			return r;
		}

		Vec3 TransformPoint(const Mat4& matrix, const Vec3& p)
		{
			const float* m = matrix.m;
			return { p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
			         p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
			         p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14] };
		}

		void Append(CollisionMesh& mesh, const SourceMesh& source, const Mat4& matrix)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.reserve(mesh.vertices.size() + source.vertices.size());
			for (const Vertex& vertex : source.vertices)
				mesh.vertices.push_back(TransformPoint(matrix, vertex.position));
			mesh.indices.reserve(mesh.indices.size() + source.indices.size());
			for (const uint32_t index : source.indices)
				mesh.indices.push_back(base + index);
		}

		struct Quantized
		{
			int64_t x = 0, y = 0, z = 0;
			bool operator==(const Quantized&) const = default;
		};
		struct QuantizedHash
		{
			size_t operator()(const Quantized& q) const noexcept
			{
				uint64_t h = uint64_t(q.x) * 0x9E3779B97F4A7C15ull;
				h ^= uint64_t(q.y) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
				h ^= uint64_t(q.z) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
				return static_cast<size_t>(h);
			}
		};

		Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		Vec3 Scale(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec3 MinOf(const Vec3& a, const Vec3& b) { return { (std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z) }; }
		Vec3 MaxOf(const Vec3& a, const Vec3& b) { return { (std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z) }; }
		bool Overlaps(const Vec3& aMin, const Vec3& aMax, const Vec3& bMin, const Vec3& bMax)
		{
			return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y && aMin.z <= bMax.z && aMax.z >= bMin.z;
		}

		// ── 종류 메시 삼각형 BVH (2026-10-29) ──
		//  위에서 아래로 — 무게중심이 가장 길게 퍼진 축의 가운데에서 나눈다. 잎은 삼각형 kBvhLeafSize 개 이하.
		//  왼쪽 자식은 부모 바로 다음 마디, 오른쪽 자식 번호는 right 에 둔다.
		constexpr uint32_t kBvhLeafSize = 4;

		uint32_t BuildBvhNode(CollisionMesh& mesh, const std::vector<Vec3>& centroids, uint32_t begin, uint32_t end)
		{
			const uint32_t index = static_cast<uint32_t>(mesh.bvh.size());
			mesh.bvh.emplace_back();
			Vec3 lo{ 3.0e38f, 3.0e38f, 3.0e38f }, hi{ -3.0e38f, -3.0e38f, -3.0e38f };
			Vec3 cLo = lo, cHi = hi;
			for (uint32_t i = begin; i < end; ++i)
			{
				const uint32_t t = mesh.bvhTris[i];
				for (int k = 0; k < 3; ++k)
				{
					const Vec3& p = mesh.vertices[mesh.indices[t * 3 + k]];
					lo = MinOf(lo, p);
					hi = MaxOf(hi, p);
				}
				cLo = MinOf(cLo, centroids[t]);
				cHi = MaxOf(cHi, centroids[t]);
			}
			mesh.bvh[index].boundsMin = lo;
			mesh.bvh[index].boundsMax = hi;
			if (end - begin <= kBvhLeafSize)
			{
				mesh.bvh[index].first = begin;
				mesh.bvh[index].count = end - begin;
				return index;
			}
			const Vec3 extent = Sub(cHi, cLo);
			const int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0 : (extent.y >= extent.z ? 1 : 2);
			auto key = [&](uint32_t t) { return axis == 0 ? centroids[t].x : (axis == 1 ? centroids[t].y : centroids[t].z); };
			const uint32_t mid = begin + (end - begin) / 2;
			std::nth_element(mesh.bvhTris.begin() + begin, mesh.bvhTris.begin() + mid, mesh.bvhTris.begin() + end,
				[&](uint32_t a, uint32_t b) { return key(a) < key(b); });
			BuildBvhNode(mesh, centroids, begin, mid);
			const uint32_t right = BuildBvhNode(mesh, centroids, mid, end);
			mesh.bvh[index].right = right;
			return index;
		}

		void BuildBvh(CollisionMesh& mesh)
		{
			mesh.bvh.clear();
			mesh.bvhTris.clear();
			const uint32_t triangles = static_cast<uint32_t>(mesh.indices.size() / 3);
			if (triangles == 0) return;
			std::vector<Vec3> centroids(triangles);
			mesh.bvhTris.resize(triangles);
			for (uint32_t t = 0; t < triangles; ++t)
			{
				const Vec3& a = mesh.vertices[mesh.indices[t * 3]];
				const Vec3& b = mesh.vertices[mesh.indices[t * 3 + 1]];
				const Vec3& c = mesh.vertices[mesh.indices[t * 3 + 2]];
				centroids[t] = Scale(Add(Add(a, b), c), 1.0f / 3.0f);
				mesh.bvhTris[t] = t;
			}
			mesh.bvh.reserve(size_t(triangles) * 2 / kBvhLeafSize + 1);
			BuildBvhNode(mesh, centroids, 0, triangles);
		}

		// ── 점 · 선분 · 삼각형 최근접 (Ericson, 「Real-Time Collision Detection」 5.1.5 · 5.1.9) ──
		Vec3 ClosestPointTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
		{
			const Vec3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
			const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
			if (d1 <= 0.0f && d2 <= 0.0f) return a;
			const Vec3 bp = Sub(p, b);
			const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
			if (d3 >= 0.0f && d4 <= d3) return b;
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
			{
				const float v = d1 / (d1 - d3);
				return Add(a, Scale(ab, v));
			}
			const Vec3 cp = Sub(p, c);
			const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
			if (d6 >= 0.0f && d5 <= d6) return c;
			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
			{
				const float w = d2 / (d2 - d6);
				return Add(a, Scale(ac, w));
			}
			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			{
				const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				return Add(b, Scale(Sub(c, b), w));
			}
			const float denom = 1.0f / (va + vb + vc);
			return Add(a, Add(Scale(ab, vb * denom), Scale(ac, vc * denom)));
		}

		float ClosestSegmentSegment(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2)
		{
			constexpr float kEps = 1.0e-12f;
			const Vec3 d1 = Sub(q1, p1), d2 = Sub(q2, p2), r = Sub(p1, p2);
			const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
			float s = 0.0f, t = 0.0f;
			if (a <= kEps && e <= kEps)
			{
				c1 = p1; c2 = p2;
				return Dot(Sub(c1, c2), Sub(c1, c2));
			}
			if (a <= kEps)
			{
				t = std::clamp(f / e, 0.0f, 1.0f);
			}
			else
			{
				const float c = Dot(d1, r);
				if (e <= kEps)
				{
					s = std::clamp(-c / a, 0.0f, 1.0f);
				}
				else
				{
					const float b = Dot(d1, d2);
					const float denom = a * e - b * b;
					s = denom != 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
					t = (b * s + f) / e;
					if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
					else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
				}
			}
			c1 = Add(p1, Scale(d1, s));
			c2 = Add(p2, Scale(d2, t));
			return Dot(Sub(c1, c2), Sub(c1, c2));
		}

		// 선분 pq 와 삼각형 abc 의 최근접 거리². 선분이 삼각형을 뚫으면 crosses = true · 0.
		float SegmentTriangleDistanceSq(const Vec3& p, const Vec3& q, const Vec3& a, const Vec3& b, const Vec3& c,
			Vec3& onSegment, Vec3& onTriangle, bool& crosses)
		{
			crosses = false;
			const Vec3 n = Cross(Sub(b, a), Sub(c, a));
			const float dp = Dot(n, Sub(p, a)), dq = Dot(n, Sub(q, a));
			if (dp * dq <= 0.0f && dp != dq)
			{
				const float t = dp / (dp - dq);
				const Vec3 x = Add(p, Scale(Sub(q, p), t));
				const Vec3 onPlane = ClosestPointTriangle(x, a, b, c);
				const Vec3 gap = Sub(x, onPlane);
				if (Dot(gap, gap) <= 1.0e-10f)
				{
					crosses = true;
					onSegment = onTriangle = x;
					return 0.0f;
				}
			}
			float best = 3.0e38f;
			auto take = [&](const Vec3& s, const Vec3& tPoint)
			{
				const Vec3 d = Sub(s, tPoint);
				const float distSq = Dot(d, d);
				if (distSq < best) { best = distSq; onSegment = s; onTriangle = tPoint; }
			};
			take(p, ClosestPointTriangle(p, a, b, c));
			take(q, ClosestPointTriangle(q, a, b, c));
			const Vec3* edges[3][2] = { { &a, &b }, { &b, &c }, { &c, &a } };
			for (const auto& edge : edges)
			{
				Vec3 c1, c2;
				ClosestSegmentSegment(p, q, *edge[0], *edge[1], c1, c2);
				take(c1, c2);
			}
			return best;
		}

		// xf(메시 → 맵)의 역을 inv 에 둔다 — 3 × 3 역행렬(행 벡터) + 이동은 그대로
		void ComputeInverse(CollisionInstance& instance)
		{
			const float* m = instance.xf;
			const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
			const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
			const float det = a * A + b * B + c * C;
			float* r = instance.inv;
			if (std::fabs(det) < 1.0e-20f)
			{
				for (int k = 0; k < 9; ++k) r[k] = (k % 4 == 0) ? 1.0f : 0.0f;
			}
			else
			{
				const float s = 1.0f / det;
				r[0] = A * s;                    r[1] = -(b * i - c * h) * s;     r[2] = (b * f - c * e) * s;
				r[3] = B * s;                    r[4] = (a * i - c * g) * s;      r[5] = -(a * f - c * d) * s;
				r[6] = C * s;                    r[7] = -(a * h - b * g) * s;     r[8] = (a * e - b * d) * s;
			}
			r[9] = m[9];
			r[10] = m[10];
			r[11] = m[11];
		}

		Vec3 InverseTransform(const CollisionInstance& instance, const Vec3& p)
		{
			const float* m = instance.inv;
			const Vec3 d{ p.x - m[9], p.y - m[10], p.z - m[11] };
			return { d.x * m[0] + d.y * m[3] + d.z * m[6],
			         d.x * m[1] + d.y * m[4] + d.z * m[7],
			         d.x * m[2] + d.y * m[5] + d.z * m[8] };
		}

		// 용접 · 퇴화 삼각형 제거 · 경계 상자 · 디버그 선
		void FinishMesh(CollisionMesh& mesh)
		{
			// ① 용접 — 같은 자리(kWeldEpsilon 격자)의 정점을 하나로. 파서가 정점을 나눴어도 모서리가 다시 이어진다.
			std::unordered_map<Quantized, uint32_t, QuantizedHash> slot;
			slot.reserve(mesh.vertices.size());
			std::vector<Vec3> welded;
			welded.reserve(mesh.vertices.size());
			std::vector<uint32_t> remap(mesh.vertices.size());
			for (size_t i = 0; i < mesh.vertices.size(); ++i)
			{
				const Vec3& p = mesh.vertices[i];
				const Quantized q{ std::llround(double(p.x) / kWeldEpsilon), std::llround(double(p.y) / kWeldEpsilon),
				                   std::llround(double(p.z) / kWeldEpsilon) };
				const auto found = slot.find(q);
				if (found != slot.end())
				{
					remap[i] = found->second;
					continue;
				}
				const uint32_t index = static_cast<uint32_t>(welded.size());
				slot.emplace(q, index);
				welded.push_back(p);
				remap[i] = index;
			}

			// ② 퇴화 삼각형(같은 정점이 둘 이상)을 뺀다
			std::vector<uint32_t> indices;
			indices.reserve(mesh.indices.size());
			for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
			{
				const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
				if (a >= remap.size() || b >= remap.size() || c >= remap.size()) continue;
				const uint32_t ra = remap[a], rb = remap[b], rc = remap[c];
				if (ra == rb || rb == rc || ra == rc) continue;
				indices.push_back(ra);
				indices.push_back(rb);
				indices.push_back(rc);
			}
			mesh.vertices = std::move(welded);
			mesh.indices = std::move(indices);

			// ③ 경계 상자
			if (!mesh.vertices.empty())
			{
				mesh.boundsMin = mesh.boundsMax = mesh.vertices.front();
				for (const Vec3& p : mesh.vertices)
				{
					mesh.boundsMin = { (std::min)(mesh.boundsMin.x, p.x), (std::min)(mesh.boundsMin.y, p.y), (std::min)(mesh.boundsMin.z, p.z) };
					mesh.boundsMax = { (std::max)(mesh.boundsMax.x, p.x), (std::max)(mesh.boundsMax.y, p.y), (std::max)(mesh.boundsMax.z, p.z) };
				}
			}

			// ④ 디버그 선 — 경계(면 하나)와 꺾인 모서리만. 평면을 삼각형으로 나눈 대각선은 긋지 않는다(상자가 상자로 보이게).
			const size_t triangles = mesh.indices.size() / 3;
			std::vector<Vec3> normals(triangles);
			for (size_t t = 0; t < triangles; ++t)
			{
				const Vec3& a = mesh.vertices[mesh.indices[t * 3]];
				const Vec3 n = Cross(Sub(mesh.vertices[mesh.indices[t * 3 + 1]], a), Sub(mesh.vertices[mesh.indices[t * 3 + 2]], a));
				const float length = std::sqrt(Dot(n, n));
				normals[t] = length > 1.0e-12f ? Vec3{ n.x / length, n.y / length, n.z / length } : Vec3{};
			}
			struct EdgeUse { uint32_t first = kNone; uint32_t second = kNone; uint32_t count = 0; };
			std::unordered_map<uint64_t, EdgeUse> uses;
			uses.reserve(mesh.indices.size());
			for (size_t t = 0; t < triangles; ++t)
			{
				for (int e = 0; e < 3; ++e)
				{
					uint32_t a = mesh.indices[t * 3 + e];
					uint32_t b = mesh.indices[t * 3 + (e + 1) % 3];
					if (a > b) std::swap(a, b);
					EdgeUse& use = uses[(uint64_t(a) << 32) | b];
					if (use.count == 0) use.first = static_cast<uint32_t>(t);
					else if (use.count == 1) use.second = static_cast<uint32_t>(t);
					++use.count;
				}
			}
			mesh.edges.clear();
			mesh.edges.reserve(uses.size());
			for (const auto& [key, use] : uses)
			{
				bool keep = use.count != 2;   // 경계(1) · 세 면 이상 만나는 모서리
				if (!keep)
				{
					const Vec3& n0 = normals[use.first];
					const Vec3& n1 = normals[use.second];
					keep = Dot(n0, n0) == 0.0f || Dot(n1, n1) == 0.0f || Dot(n0, n1) < kFeatureCos;
				}
				if (!keep) continue;
				mesh.edges.push_back(static_cast<uint32_t>(key >> 32));
				mesh.edges.push_back(static_cast<uint32_t>(key & 0xFFFFFFFFull));
			}

			BuildBvh(mesh);
		}

		// 배치 인스턴스 → 행 벡터 3 × 4. PlanetDebugScene.cpp InstanceMatrix(XMMatrixScaling · RotationQuaternion · Translation)와 같다.
		void MakeInstanceTransform(const PlacedInstance& instance, float xf[12])
		{
			float x = instance.rotation[0], y = instance.rotation[1], z = instance.rotation[2], w = instance.rotation[3];
			const float lengthSq = x * x + y * y + z * z + w * w;
			if (lengthSq > 1.0e-12f)   // JSON 은 소수 셋째 자리로 반올림돼 있다 — 정규화한다
			{
				const float inv = 1.0f / std::sqrt(lengthSq);
				x *= inv; y *= inv; z *= inv; w *= inv;
			}
			else
			{
				x = y = z = 0.0f; w = 1.0f;
			}
			// 행 벡터 회전 행렬 — GltfReader.cpp FromQuaternion · XMMatrixRotationQuaternion 과 같은 배치
			const float r[9] = {
				1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z),        2.0f * (x * z - w * y),
				2.0f * (x * y - w * z),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x),
				2.0f * (x * z + w * y),        2.0f * (y * z - w * x),        1.0f - 2.0f * (x * x + y * y) };
			// 행 벡터에서 S · R = 회전 행마다 배율을 곱한 것(균일 배율)
			for (int i = 0; i < 9; ++i) xf[i] = r[i] * instance.scale;
			xf[9] = instance.position[0];
			xf[10] = instance.position[1];
			xf[11] = instance.position[2];
		}

		void InstanceBounds(const CollisionMesh& mesh, CollisionInstance& instance)
		{
			bool first = true;
			for (int corner = 0; corner < 8; ++corner)
			{
				const Vec3 local{ (corner & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
				                  (corner & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
				                  (corner & 4) ? mesh.boundsMax.z : mesh.boundsMin.z };
				const Vec3 p = CollisionWorld::Transform(instance, local);
				if (first)
				{
					instance.boundsMin = instance.boundsMax = p;
					first = false;
					continue;
				}
				instance.boundsMin = { (std::min)(instance.boundsMin.x, p.x), (std::min)(instance.boundsMin.y, p.y), (std::min)(instance.boundsMin.z, p.z) };
				instance.boundsMax = { (std::max)(instance.boundsMax.x, p.x), (std::max)(instance.boundsMax.y, p.y), (std::max)(instance.boundsMax.z, p.z) };
			}
		}

		int64_t CellOf(float value)
		{
			const int64_t cell = static_cast<int64_t>(std::floor(double(value) / CollisionWorld::kCellSize));
			return std::clamp(cell, -kCellBias + 1, kCellBias - 1);
		}

		uint64_t CellKey(int64_t x, int64_t y, int64_t z)
		{
			return (uint64_t(x + kCellBias) << 42) | (uint64_t(y + kCellBias) << 21) | uint64_t(z + kCellBias);
		}

	} // namespace

	bool CollisionWorld::LoadPack(const wchar_t* packPath, const wchar_t* manifestPath, std::wstring& error)
	{
		*this = CollisionWorld{};
		error.clear();

		// ① 설명 JSON — kind 와 항목별 class · min_scale
		std::string text;
		if (!manifestPath || !ReadText(manifestPath, text))
		{
			error = L"충돌체 설명 JSON 을 열지 못했습니다: " + std::wstring(manifestPath ? manifestPath : L"");
			return false;
		}
		JsonValue root;
		std::string jsonError;
		if (!JsonValue::Parse(text.data(), text.size(), root, jsonError))
		{
			error = L"충돌체 설명 JSON 해석 실패: " + FromUtf8(jsonError);
			return false;
		}
		const std::string& kind = root["kind"].AsString();
		if (kind != "types" && kind != "placed")
		{
			error = L"충돌체 설명 JSON 의 kind 를 모릅니다(types · placed): " + FromUtf8(kind);
			return false;
		}
		placed = (kind == "placed");
		const JsonValue& items = root[placed ? "items" : "types"];

		// ② 묶음 glb — COL_ 노드가 메시를 그대로 갖도록 보조 노드 숨기기를 끈다. 위치만 읽는다(서버와 같은 길).
		ReadOptions options;
		options.geometryOnly = true;
		options.generateCollision = false;
		options.generateTangents = false;
		options.readAnimation = false;
		options.hideAuthoringHelpers = false;
		ModelSource source;
		if (!ModelReader().Load(packPath, source, error, options))
			return false;

		// 노드 월드 = 로컬 · 부모 월드 (부모가 먼저 온다 — ValidateModelSource 가 검사한다)
		std::vector<Mat4> world(source.nodes.size());
		for (size_t i = 0; i < source.nodes.size(); ++i)
		{
			const SourceNode& node = source.nodes[i];
			world[i] = node.parent == kInvalidIndex || node.parent >= i ? node.local : Multiply(node.local, world[node.parent]);
		}

		std::vector<uint32_t> meshOfSource(source.meshes.size(), kNone);
		for (size_t i = 0; i < source.nodes.size(); ++i)
		{
			const SourceNode& node = source.nodes[i];
			if (node.meshes.empty() || node.name.rfind("COL_", 0) != 0) continue;
			const std::string name = node.name.substr(4);
			const JsonValue& item = items[name.c_str()];
			const CollisionClass cls = ClassOf(item["class"].AsString());

			if (!placed)
			{
				// 종류 메시 — 노드 변환을 정점에 굽는다(col_build 는 단위 행렬로 쓴다)
				CollisionMesh mesh;
				mesh.name = name;
				mesh.cls = cls;
				mesh.minScale = static_cast<float>(item["min_scale"].AsNumber(0.0));
				for (const uint32_t m : node.meshes)
					if (m < source.meshes.size()) Append(mesh, source.meshes[m], world[i]);
				FinishMesh(mesh);
				if (mesh.indices.empty()) continue;
				meshOfName[name] = static_cast<uint32_t>(meshes.size());
				meshes.push_back(std::move(mesh));
				continue;
			}

			// 맵 오브젝트 — 메시는 묶음의 glTF 메시마다 하나(같은 모양 여러 노드가 같이 쓴다), 노드마다 인스턴스
			uint32_t meshIndex = kNone;
			if (node.meshes.size() == 1 && node.meshes[0] < source.meshes.size())
			{
				uint32_t& shared = meshOfSource[node.meshes[0]];
				if (shared == kNone)
				{
					CollisionMesh mesh;
					mesh.name = name;
					mesh.cls = cls;
					Append(mesh, source.meshes[node.meshes[0]], Mat4{});
					FinishMesh(mesh);
					if (!mesh.indices.empty())
					{
						shared = static_cast<uint32_t>(meshes.size());
						meshes.push_back(std::move(mesh));
					}
				}
				meshIndex = shared;
			}
			else
			{
				CollisionMesh mesh;
				mesh.name = name;
				mesh.cls = cls;
				for (const uint32_t m : node.meshes)
					if (m < source.meshes.size()) Append(mesh, source.meshes[m], Mat4{});
				FinishMesh(mesh);
				if (!mesh.indices.empty())
				{
					meshIndex = static_cast<uint32_t>(meshes.size());
					meshes.push_back(std::move(mesh));
				}
			}
			if (meshIndex == kNone) continue;

			CollisionInstance instance;
			instance.mesh = meshIndex;
			instance.cls = cls;
			const float* m = world[i].m;
			const float xf[12] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14] };
			std::copy(std::begin(xf), std::end(xf), instance.xf);
			ComputeInverse(instance);
			InstanceBounds(meshes[meshIndex], instance);
			instances.push_back(instance);
		}

		if (meshes.empty())
		{
			error = L"충돌체 묶음에 COL_ 노드가 없습니다: " + std::wstring(packPath ? packPath : L"");
			return false;
		}
		loaded = true;
		return true;
	}

	size_t CollisionWorld::AddPlacement(const std::vector<std::string>& typeNames, const std::vector<PlacedInstance>& list)
	{
		if (!loaded || placed) return 0;

		std::vector<uint32_t> meshOfType(typeNames.size(), kNone);
		for (size_t t = 0; t < typeNames.size(); ++t)
		{
			const auto found = meshOfName.find(typeNames[t]);
			if (found != meshOfName.end()) meshOfType[t] = found->second;
		}

		size_t added = 0;
		instances.reserve(instances.size() + list.size());
		for (const PlacedInstance& in : list)
		{
			const uint32_t meshIndex = in.type < meshOfType.size() ? meshOfType[in.type] : kNone;
			if (meshIndex == kNone)
			{
				++skippedNoCollider;   // 작은 식물 등 충돌체가 없는 종류
				continue;
			}
			const CollisionMesh& mesh = meshes[meshIndex];
			if (mesh.cls == CollisionClass::Rock && std::fabs(in.scale) < mesh.minScale)
			{
				++skippedSmallRocks;   // 높이 × 배율 < 1.8 m × 15 %
				continue;
			}
			CollisionInstance instance;
			instance.mesh = meshIndex;
			instance.cls = mesh.cls;
			MakeInstanceTransform(in, instance.xf);
			ComputeInverse(instance);
			InstanceBounds(mesh, instance);
			instances.push_back(instance);
			++added;
		}
		return added;
	}

	void CollisionWorld::Finalize()
	{
		grid.clear();
		large.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i)
		{
			const CollisionInstance& instance = instances[i];
			const int64_t x0 = CellOf(instance.boundsMin.x), x1 = CellOf(instance.boundsMax.x);
			const int64_t y0 = CellOf(instance.boundsMin.y), y1 = CellOf(instance.boundsMax.y);
			const int64_t z0 = CellOf(instance.boundsMin.z), z1 = CellOf(instance.boundsMax.z);
			if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > kMaxCellsPerInstance)
			{
				large.push_back(i);
				continue;
			}
			for (int64_t x = x0; x <= x1; ++x)
				for (int64_t y = y0; y <= y1; ++y)
					for (int64_t z = z0; z <= z1; ++z)
						grid[CellKey(x, y, z)].push_back(i);
		}
	}

	size_t CollisionWorld::MeshTriangleCount() const
	{
		size_t total = 0;
		for (const CollisionMesh& mesh : meshes) total += mesh.indices.size() / 3;
		return total;
	}

	Vec3 CollisionWorld::Transform(const CollisionInstance& instance, const Vec3& p)
	{
		const float* m = instance.xf;
		return { p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9],
		         p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
		         p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11] };
	}

	void CollisionWorld::QuerySphere(const Vec3& center, float radius, std::vector<uint32_t>& out) const
	{
		out.clear();
		if (instances.empty() || radius <= 0.0f) return;

		const int64_t x0 = CellOf(center.x - radius), x1 = CellOf(center.x + radius);
		const int64_t y0 = CellOf(center.y - radius), y1 = CellOf(center.y + radius);
		const int64_t z0 = CellOf(center.z - radius), z1 = CellOf(center.z + radius);
		for (int64_t x = x0; x <= x1; ++x)
			for (int64_t y = y0; y <= y1; ++y)
				for (int64_t z = z0; z <= z1; ++z)
				{
					const auto found = grid.find(CellKey(x, y, z));
					if (found != grid.end())
						out.insert(out.end(), found->second.begin(), found->second.end());
				}
		out.insert(out.end(), large.begin(), large.end());
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());

		// 경계 상자까지의 거리로 거른다
		const float radiusSq = radius * radius;
		size_t kept = 0;
		for (const uint32_t index : out)
		{
			const CollisionInstance& instance = instances[index];
			const float dx = (std::max)({ instance.boundsMin.x - center.x, 0.0f, center.x - instance.boundsMax.x });
			const float dy = (std::max)({ instance.boundsMin.y - center.y, 0.0f, center.y - instance.boundsMax.y });
			const float dz = (std::max)({ instance.boundsMin.z - center.z, 0.0f, center.z - instance.boundsMax.z });
			if (dx * dx + dy * dy + dz * dz <= radiusSq)
				out[kept++] = index;
		}
		out.resize(kept);
	}

	// ── 캡슐 겹침 (2026-10-29 — Shared::PlayerMovement 가 이동 뒤에 부른다) ──
	//  ① 캡슐 경계 상자가 닿는 격자 칸 → 인스턴스 후보 ② 인스턴스마다 캡슐 상자를 메시 공간으로 옮겨 삼각형 BVH 를 내려간다
	//  ③ 후보 삼각형을 맵 공간으로 옮겨(세 꼭짓점) 선분 ab 와의 최근접 거리를 잰다 — 반지름보다 가까우면 겹침.
	//  계산은 a 를 원점으로 옮긴 좌표에서 한다(맵 공간 값이 수백 m 라 float 뺄셈 오차를 줄이려고).
	//  선분이 삼각형을 뚫은 경우(거리 0)는 면 법선을 «더 멀리 있는 끝» 쪽으로 돌리고, 가까운 끝까지의 거리 + 반지름만큼 민다.
	bool CollisionWorld::DeepestCapsuleContact(const Vec3& a, const Vec3& b, float radius, CapsuleContact& out) const
	{
		if (instances.empty() || radius <= 0.0f) return false;

		const Vec3 lo = Sub(MinOf(a, b), Vec3{ radius, radius, radius });
		const Vec3 hi = Add(MaxOf(a, b), Vec3{ radius, radius, radius });

		std::vector<uint32_t> candidates;
		const int64_t x0 = CellOf(lo.x), x1 = CellOf(hi.x);
		const int64_t y0 = CellOf(lo.y), y1 = CellOf(hi.y);
		const int64_t z0 = CellOf(lo.z), z1 = CellOf(hi.z);
		for (int64_t x = x0; x <= x1; ++x)
			for (int64_t y = y0; y <= y1; ++y)
				for (int64_t z = z0; z <= z1; ++z)
				{
					const auto found = grid.find(CellKey(x, y, z));
					if (found != grid.end())
						candidates.insert(candidates.end(), found->second.begin(), found->second.end());
				}
		candidates.insert(candidates.end(), large.begin(), large.end());
		if (candidates.empty()) return false;
		std::sort(candidates.begin(), candidates.end());
		candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

		const Vec3 segment = Sub(b, a);          // a 기준 좌표의 선분 (0 → segment)
		const Vec3 origin{ 0.0f, 0.0f, 0.0f };
		const float radiusSq = radius * radius;
		bool found = false;
		float bestDepth = 0.0f;
		Vec3 bestNormal{};
		std::vector<uint32_t> stack;

		for (const uint32_t index : candidates)
		{
			const CollisionInstance& instance = instances[index];
			if (!Overlaps(instance.boundsMin, instance.boundsMax, lo, hi)) continue;
			const CollisionMesh& mesh = meshes[instance.mesh];
			if (mesh.bvh.empty()) continue;

			// 캡슐 상자를 메시 공간으로 — 여덟 꼭짓점을 옮긴 경계(보수적)
			Vec3 mLo{ 3.0e38f, 3.0e38f, 3.0e38f }, mHi{ -3.0e38f, -3.0e38f, -3.0e38f };
			for (int corner = 0; corner < 8; ++corner)
			{
				const Vec3 p{ (corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z };
				const Vec3 m = InverseTransform(instance, p);
				mLo = MinOf(mLo, m);
				mHi = MaxOf(mHi, m);
			}

			stack.clear();
			stack.push_back(0);
			while (!stack.empty())
			{
				const CollisionBvhNode& node = mesh.bvh[stack.back()];
				const uint32_t nodeIndex = stack.back();
				stack.pop_back();
				if (!Overlaps(node.boundsMin, node.boundsMax, mLo, mHi)) continue;
				if (node.count == 0)
				{
					stack.push_back(node.right);
					stack.push_back(nodeIndex + 1);
					continue;
				}
				for (uint32_t k = node.first; k < node.first + node.count; ++k)
				{
					const uint32_t t = mesh.bvhTris[k];
					const Vec3 va = Sub(Transform(instance, mesh.vertices[mesh.indices[t * 3]]), a);
					const Vec3 vb = Sub(Transform(instance, mesh.vertices[mesh.indices[t * 3 + 1]]), a);
					const Vec3 vc = Sub(Transform(instance, mesh.vertices[mesh.indices[t * 3 + 2]]), a);

					Vec3 onSegment, onTriangle;
					bool crosses = false;
					const float distSq = SegmentTriangleDistanceSq(origin, segment, va, vb, vc, onSegment, onTriangle, crosses);
					if (distSq >= radiusSq) continue;

					Vec3 normal{};
					float depth = 0.0f;
					const float dist = std::sqrt(distSq);
					if (!crosses && dist > 1.0e-5f)
					{
						normal = Scale(Sub(onSegment, onTriangle), 1.0f / dist);
						depth = radius - dist;
					}
					else
					{
						Vec3 n = Cross(Sub(vb, va), Sub(vc, va));
						const float length = std::sqrt(Dot(n, n));
						if (length < 1.0e-12f) continue;
						n = Scale(n, 1.0f / length);
						const float sideA = Dot(n, Sub(origin, va));
						const float sideB = Dot(n, Sub(segment, va));
						// ★ 이름을 far 로 두지 않는다 — windows.h 의 far 매크로(빈 문자열)와 겹친다
						const float fartherSide = std::fabs(sideA) >= std::fabs(sideB) ? sideA : sideB;
						normal = fartherSide >= 0.0f ? n : Scale(n, -1.0f);
						depth = radius + (std::min)(std::fabs(sideA), std::fabs(sideB));
					}
					if (!found || depth > bestDepth)
					{
						found = true;
						bestDepth = depth;
						bestNormal = normal;
					}
				}
			}
		}

		if (!found) return false;
		out.normal = bestNormal;
		out.depth = bestDepth;
		return true;
	}

} // namespace Shared
