#include "MeshPostProcess.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

namespace Shared {

	namespace {

		// ── 행렬·벡터 헬퍼 (이 파일 안에서만 쓴다) ─────────────
		//  Vec3.h 는 연산자 없는 순수 데이터다. 공용 수학 API 를 늘리지 않고 여기서만 계산한다.

		Mat4 Multiply(const Mat4& a, const Mat4& b)
		{
			Mat4 r;
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 4; ++col)
				{
					float sum = 0.0f;
					for (int k = 0; k < 4; ++k)
						sum += a.m[row * 4 + k] * b.m[k * 4 + col];
					r.m[row * 4 + col] = sum;
				}
			}
			return r;
		}

		// 행 벡터 규약(DirectXMath 와 같다): p' = p * M
		Vec3 TransformPoint(const Mat4& m, const Vec3& p)
		{
			return {
				p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12],
				p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13],
				p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14] };
		}

		Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		Vec3 Scale(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

		Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return { a.y * b.z - a.z * b.y,
					 a.z * b.x - a.x * b.z,
					 a.x * b.y - a.y * b.x };
		}

		float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

		Vec3 Normalize(const Vec3& a)
		{
			const float len = Length(a);
			return len > 1.0e-8f ? Scale(a, 1.0f / len) : Vec3{ 0.0f, 1.0f, 0.0f };
		}

		bool Finite(const Vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// 오류 메시지에 이름(파일에 적힌 UTF-8)을 넣기 위한 변환.
		// <Windows.h> 를 끌어오지 않으려고 filesystem 의 u8 경로 변환을 쓴다.
		std::wstring ToWide(const std::string& utf8)
		{
			return std::filesystem::path(
				std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())).wstring();
		}

	} // namespace

	bool ValidateModelSource(const ModelSource& source, std::wstring& error)
	{
		// ★ 애니메이션 클립만 든 파일이 정상 입력이다 (명세 §4 — 메시와 애니메이션은 다른 리소스).
		//   동작 하나당 파일 하나로 내보내면 메시·노드가 비어 있다.
		const bool hasGeometry = !source.meshes.empty() && !source.nodes.empty();
		const bool hasAnimation = !source.animations.empty() || !source.skeleton.Empty();
		if (!hasGeometry && !hasAnimation)
		{
			error = L"모델에 메시·노드도 애니메이션도 없습니다.";
			return false;
		}
		if (!source.meshes.empty() && source.nodes.empty())
		{
			error = L"메시는 있는데 노드가 없습니다.";
			return false;
		}

		for (size_t i = 0; i < source.nodes.size(); ++i)
		{
			const SourceNode& node = source.nodes[i];

			// ★ 부모가 먼저 와야 한다. Scene 이 배열 앞쪽부터 월드 행렬을 계산하기 때문이다.
			if (node.parent != kInvalidIndex && node.parent >= i)
			{
				error = L"모델 노드가 부모보다 먼저 배치되어 있습니다.";
				return false;
			}
			for (uint32_t mesh : node.meshes)
			{
				if (mesh >= source.meshes.size())
				{
					error = L"노드의 메시 참조가 범위를 벗어났습니다.";
					return false;
				}
			}
		}

		for (const SourceMesh& mesh : source.meshes)
		{
			if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
			{
				error = L"메시의 정점·인덱스 수가 올바르지 않습니다(삼각형 목록이어야 합니다).";
				return false;
			}
			if (mesh.material != kInvalidIndex && mesh.material >= source.materials.size())
			{
				error = L"메시의 재질 참조가 범위를 벗어났습니다.";
				return false;
			}
			for (uint32_t index : mesh.indices)
			{
				if (index >= mesh.vertices.size())
				{
					error = L"메시의 인덱스가 정점 수를 넘었습니다.";
					return false;
				}
			}
		}

		// ── 스켈레톤 (본) ───────────────────────────────
		if (source.skeleton.joints.size() > kMaxJoints)
		{
			error = L"스켈레톤의 조인트가 너무 많습니다(" + std::to_wstring(kMaxJoints) + L" 제한).";
			return false;
		}
		for (size_t i = 0; i < source.skeleton.joints.size(); ++i)
		{
			// 포즈 계산이 배열 앞쪽부터 한 번에 돌 수 있어야 한다(메시 노드와 같은 규칙).
			const SourceJoint& joint = source.skeleton.joints[i];
			if (joint.parent != kInvalidIndex && joint.parent >= i)
			{
				error = L"스켈레톤 조인트가 부모보다 먼저 배치되어 있습니다.";
				return false;
			}
		}

		// ── 스킨 가중치 ─────────────────────────────────
		for (const SourceMesh& mesh : source.meshes)
		{
			if (!mesh.Skinned()) continue;
			if (source.skeleton.Empty())
			{
				error = L"스킨 메시인데 스켈레톤이 없습니다.";
				return false;
			}
			if (mesh.skin.size() != mesh.vertices.size())
			{
				error = L"스킨 배열 길이가 정점 수와 다릅니다.";
				return false;
			}
			const uint16_t jointCount = static_cast<uint16_t>(source.skeleton.joints.size());
			for (const SkinVertex& vertex : mesh.skin)
			{
				for (size_t slot = 0; slot < kJointsPerVertex; ++slot)
				{
					// 가중치가 0 인 슬롯의 조인트 번호는 보지 않는다(glTF 가 쓰레기를 넣어도 된다).
					if (vertex.weights[slot] > 0.0f && vertex.joints[slot] >= jointCount)
					{
						error = L"스킨 정점의 조인트 번호가 스켈레톤 범위를 벗어났습니다.";
						return false;
					}
				}
			}
		}

		// ── 애니메이션 클립 ─────────────────────────────
		if (source.animations.size() > kMaxAnimations)
		{
			error = L"애니메이션 클립이 너무 많습니다.";
			return false;
		}
		for (const AnimationSource& clip : source.animations)
		{
			for (const AnimationChannel& channel : clip.channels)
			{
				if (channel.joint >= source.skeleton.joints.size())
				{
					error = L"애니메이션 채널이 가리키는 조인트가 없습니다: " + ToWide(clip.name);
					return false;
				}
				const size_t stride = channel.path == AnimationPath::Rotation ? 4u : 3u;
				if (channel.times.empty() || channel.values.size() != channel.times.size() * stride)
				{
					error = L"애니메이션 채널의 키 개수와 값 개수가 맞지 않습니다: " + ToWide(clip.name);
					return false;
				}
				if (channel.times.size() > kMaxKeyframes)
				{
					error = L"애니메이션 키프레임이 너무 많습니다: " + ToWide(clip.name);
					return false;
				}
				for (size_t i = 1; i < channel.times.size(); ++i)
				{
					if (!(channel.times[i] >= channel.times[i - 1]))
					{
						error = L"애니메이션 키 시간이 오름차순이 아닙니다: " + ToWide(clip.name);
						return false;
					}
				}
			}
		}

		return true;
	}

	void GenerateNormals(SourceMesh& mesh)
	{
		// 0 인 법선만 채운다. 파일에 법선이 있으면 그것을 믿는다.
		// ★ 채울 정점이 하나도 없으면 임시 배열을 만들지 않고 바로 끝낸다 (2026-10-09)
		//   행성 OBJ 처럼 법선이 다 있는 파일에서 정점 1천만 × 12B 누적 배열과 면 순회를 아낀다.
		bool anyMissing = false;
		for (const Vertex& vertex : mesh.vertices)
		{
			if (Length(vertex.normal) < 1.0e-6f) { anyMissing = true; break; }
		}
		if (!anyMissing) return;

		std::vector<Vec3> accumulated(mesh.vertices.size(), Vec3{});
		std::vector<bool> needs(mesh.vertices.size(), false);

		for (size_t i = 0; i < mesh.vertices.size(); ++i)
			needs[i] = Length(mesh.vertices[i].normal) < 1.0e-6f;

		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			const uint32_t i0 = mesh.indices[i];
			const uint32_t i1 = mesh.indices[i + 1];
			const uint32_t i2 = mesh.indices[i + 2];

			const Vec3& p0 = mesh.vertices[i0].position;
			const Vec3 faceNormal = Cross(Sub(mesh.vertices[i1].position, p0),
										  Sub(mesh.vertices[i2].position, p0));

			accumulated[i0] = Add(accumulated[i0], faceNormal);
			accumulated[i1] = Add(accumulated[i1], faceNormal);
			accumulated[i2] = Add(accumulated[i2], faceNormal);
		}

		for (size_t i = 0; i < mesh.vertices.size(); ++i)
		{
			if (needs[i])
				mesh.vertices[i].normal = Normalize(accumulated[i]);
		}
	}

	void GenerateTangents(SourceMesh& mesh)
	{
		// 삼각형마다 UV 기울기로 접선을 구해 정점에 누적한다.
		std::vector<Vec3> tangents(mesh.vertices.size(), Vec3{});
		std::vector<Vec3> bitangents(mesh.vertices.size(), Vec3{});

		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			const uint32_t idx[3] = { mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2] };

			const Vec3& p0 = mesh.vertices[idx[0]].position;
			const Vec3 e1 = Sub(mesh.vertices[idx[1]].position, p0);
			const Vec3 e2 = Sub(mesh.vertices[idx[2]].position, p0);

			const Vec2& uv0 = mesh.vertices[idx[0]].uv;
			const float du1 = mesh.vertices[idx[1]].uv.x - uv0.x;
			const float dv1 = mesh.vertices[idx[1]].uv.y - uv0.y;
			const float du2 = mesh.vertices[idx[2]].uv.x - uv0.x;
			const float dv2 = mesh.vertices[idx[2]].uv.y - uv0.y;

			const float det = du1 * dv2 - du2 * dv1;
			if (std::fabs(det) < 1.0e-12f)
				continue;   // UV 가 퇴화한 삼각형. 건너뛴다

			const float inv = 1.0f / det;
			const Vec3 tangent = Scale(Sub(Scale(e1, dv2), Scale(e2, dv1)), inv);
			const Vec3 bitangent = Scale(Sub(Scale(e2, du1), Scale(e1, du2)), inv);

			if (!Finite(tangent) || !Finite(bitangent))
				continue;

			for (int k = 0; k < 3; ++k)
			{
				tangents[idx[k]] = Add(tangents[idx[k]], tangent);
				bitangents[idx[k]] = Add(bitangents[idx[k]], bitangent);
			}
		}

		for (size_t i = 0; i < mesh.vertices.size(); ++i)
		{
			const Vec3 n = Normalize(mesh.vertices[i].normal);
			Vec3 t = tangents[i];

			// 그람-슈미트. 접선이 퇴화하면 법선과 직교하는 아무 축을 만든다.
			t = Sub(t, Scale(n, Dot(n, t)));
			if (Length(t) < 1.0e-8f)
			{
				const Vec3 seed = (std::fabs(n.y) < 0.9f) ? Vec3{ 0.0f, 1.0f, 0.0f }
														  : Vec3{ 1.0f, 0.0f, 0.0f };
				t = Cross(n, seed);
			}
			t = Normalize(t);

			// 종법선 부호 — 거울상 UV 를 살린다.
			const float sign = Dot(Cross(n, t), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
			mesh.vertices[i].tangent = { t.x, t.y, t.z, sign };
		}
	}

	void BuildBoundsAndCollision(ModelSource& source, bool generateCollision)
	{
		// 노드 계층을 한 번 훑어 글로벌 변환을 만든다 (부모가 앞에 있음은 검사로 보장).
		std::vector<Mat4> global(source.nodes.size());
		for (size_t i = 0; i < source.nodes.size(); ++i)
		{
			const SourceNode& node = source.nodes[i];
			global[i] = (node.parent == kInvalidIndex)
				? node.local
				: Multiply(node.local, global[node.parent]);
		}

		float lo[3] = { (std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)() };
		float hi[3] = { -(std::numeric_limits<float>::max)(),
						-(std::numeric_limits<float>::max)(),
						-(std::numeric_limits<float>::max)() };
		bool any = false;

		const bool fillCollision = generateCollision && source.collision.Empty();

		for (size_t n = 0; n < source.nodes.size(); ++n)
		{
			for (uint32_t meshIndex : source.nodes[n].meshes)
			{
				const SourceMesh& mesh = source.meshes[meshIndex];
				const uint32_t base = static_cast<uint32_t>(source.collision.vertices.size());

				for (const Vertex& vertex : mesh.vertices)
				{
					const Vec3 p = TransformPoint(global[n], vertex.position);
					if (!Finite(p)) continue;

					if (p.x < lo[0]) lo[0] = p.x;
					if (p.y < lo[1]) lo[1] = p.y;
					if (p.z < lo[2]) lo[2] = p.z;
					if (p.x > hi[0]) hi[0] = p.x;
					if (p.y > hi[1]) hi[1] = p.y;
					if (p.z > hi[2]) hi[2] = p.z;
					any = true;

					if (fillCollision)
						source.collision.vertices.push_back(p);
				}

				if (fillCollision)
				{
					for (uint32_t index : mesh.indices)
						source.collision.indices.push_back(base + index);
				}
			}
		}

		if (!any)
		{
			source.boundsMin = {};
			source.boundsMax = {};
			return;
		}

		source.boundsMin = { lo[0], lo[1], lo[2] };
		source.boundsMax = { hi[0], hi[1], hi[2] };

		// ★ 우리가 채운 경우에만 충돌 바운드를 쓴다.
		//   파일(.swm COLL)이 준 충돌은 렌더 메시보다 작을 수 있어 덮어쓰면 안 된다.
		if (fillCollision && !source.collision.Empty())
		{
			source.collision.boundsMin = source.boundsMin;
			source.collision.boundsMax = source.boundsMax;
		}
	}

} // namespace Shared
