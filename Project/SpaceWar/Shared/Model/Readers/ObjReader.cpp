#include "ObjReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Shared {

	namespace {

		namespace fs = std::filesystem;

		// ── 파일 읽기 ───────────────────────────────────────
		//  OBJ·MTL 은 텍스트다. 통째로 읽고 줄 단위로 훑는다 — 한 줄씩 읽는 것보다 단순하다.
		//  ★ 1GB 로 올렸다 (2026-10-08)
		//    client2 의 행성 모델 future_ruins_realistic.obj 가 **687MB** 다. 256MB 제한에 걸려
		//    열리지 않았다. 다만 통째로 읽는 방식에는 한계가 있다 — 파일 크기만큼 메모리를 쓰고
		//    정점 중복 제거 해시까지 올라간다. 맵을 .glb(바이너리)로 내보내면 파일이 수십 MB 로
		//    줄고 이 경로를 아예 지나가지 않는다. 그쪽이 맞는 해법이다.
		inline constexpr size_t kMaxTextFileBytes = 1024u * 1024u * 1024u;

		bool ReadWholeFile(const fs::path& path, std::string& out, std::wstring& error)
		{
			FILE* file = nullptr;
			if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
			{
				error = L"모델 파일을 열 수 없습니다: " + path.wstring();
				return false;
			}

			if (::fseek(file, 0, SEEK_END) != 0)
			{
				::fclose(file);
				error = L"모델 파일 크기를 알 수 없습니다: " + path.wstring();
				return false;
			}
			const long long size = ::_ftelli64(file);
			::rewind(file);

			if (size < 0 || size > static_cast<long long>(kMaxTextFileBytes))
			{
				::fclose(file);
				error = L"모델 파일이 너무 큽니다(256MB 제한): " + path.wstring();
				return false;
			}

			out.resize(static_cast<size_t>(size));
			const size_t read = out.empty() ? 0 : ::fread(out.data(), 1, out.size(), file);
			::fclose(file);
			if (read != out.size())
			{
				error = L"모델 파일을 끝까지 읽지 못했습니다: " + path.wstring();
				return false;
			}

			// ★ 줄 끝을 '\0' 로 끊어 파싱하므로 마지막 줄에도 개행이 있어야 한다.
			if (out.empty() || out.back() != '\n') out.push_back('\n');
			return true;
		}

		// MTL 경로·텍스처 경로는 파일에 UTF-8 로 적혀 있다. ACP 를 거치지 않고 바로 올린다.
		fs::path Utf8Path(const std::string& text)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		}

		std::string ToUtf8(const fs::path& path)
		{
			const std::u8string u8 = path.u8string();
			return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
		}

		// ── 줄·토큰 파싱 ────────────────────────────────────
		inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

		const char* SkipSpaces(const char* p)
		{
			while (*p && IsSpace(*p)) ++p;
			return p;
		}

		// "vn" 이 "v" 로 걸리지 않게 뒤 한 글자까지 본다.
		bool Keyword(const char*& p, const char* word)
		{
			const size_t length = std::strlen(word);
			if (std::strncmp(p, word, length) != 0) return false;
			const char after = p[length];
			if (after != '\0' && !IsSpace(after)) return false;
			p += length;
			return true;
		}

		bool ReadFloat(const char*& p, float& value)
		{
			p = SkipSpaces(p);
			char* end = nullptr;
			const float parsed = std::strtof(p, &end);
			if (end == p || !std::isfinite(parsed)) return false;
			p = end;
			value = parsed;
			return true;
		}

		// 남은 토큰 중 «마지막» 것을 돌려준다.
		//  map_Bump 는 `-bm 1.0 rock_n.png` 처럼 옵션이 먼저 올 수 있다.
		//  옵션을 모두 해석하지 않고 파일명만 집는다 — 공백이 든 파일명은 지원하지 않는다.
		std::string LastToken(const char* p)
		{
			std::string last;
			while (true)
			{
				p = SkipSpaces(p);
				if (!*p) break;
				const char* start = p;
				while (*p && !IsSpace(*p)) ++p;
				last.assign(start, size_t(p - start));
			}
			return last;
		}

		std::string FirstToken(const char* p)
		{
			p = SkipSpaces(p);
			const char* start = p;
			while (*p && !IsSpace(*p)) ++p;
			return std::string(start, size_t(p - start));
		}

		// ── 면 모서리 ───────────────────────────────────────
		//  0 = 없음. 양수는 1-based, 음수는 «끝에서부터» 다 (OBJ 규약).
		struct Corner
		{
			long position = 0;
			long uv = 0;
			long normal = 0;
		};

		bool ReadCorner(const char*& p, Corner& corner)
		{
			p = SkipSpaces(p);
			if (!*p) return false;

			char* end = nullptr;
			corner = Corner{};
			corner.position = std::strtol(p, &end, 10);
			if (end == p) return false;
			p = end;

			if (*p == '/')
			{
				++p;
				if (*p != '/')
				{
					corner.uv = std::strtol(p, &end, 10);
					if (end == p) return false;
					p = end;
				}
				if (*p == '/')
				{
					++p;
					corner.normal = std::strtol(p, &end, 10);
					if (end == p) return false;
					p = end;
				}
			}
			return true;
		}

		bool Resolve(long raw, size_t count, size_t& out)
		{
			if (raw > 0)
			{
				if (static_cast<size_t>(raw) > count) return false;
				out = static_cast<size_t>(raw) - 1;
				return true;
			}
			if (raw < 0)
			{
				const size_t back = static_cast<size_t>(-static_cast<long long>(raw));
				if (back > count) return false;
				out = count - back;
				return true;
			}
			return false;   // 0 은 OBJ 에 없는 인덱스다
		}

		// ── 정점 중복 제거 ──────────────────────────────────
		//  같은 (v, vt, vn) 조합은 정점 하나로 모은다. 조합이 다르면 좌표가 같아도 따로 둔다
		//  (UV 이음매·하드 엣지가 그 경우다).
		struct VertexKey
		{
			uint32_t position = 0;
			uint32_t uv = 0;
			uint32_t normal = 0;

			bool operator==(const VertexKey& other) const
			{
				return position == other.position && uv == other.uv && normal == other.normal;
			}
		};

		struct VertexKeyHash
		{
			size_t operator()(const VertexKey& key) const
			{
				size_t hash = 1469598103934665603ull;
				for (uint32_t part : { key.position, key.uv, key.normal })
				{
					hash ^= part;
					hash *= 1099511628211ull;
				}
				return hash;
			}
		};

		// ── MTL ─────────────────────────────────────────────
		//  Ns(하이라이트 지수) → roughness 환산은 FbxModelLoader.cpp 와 같은 식을 쓴다.
		//  두 경로가 같은 재질을 다르게 보이게 하면 비교가 불가능해진다.
		float RoughnessFromShininess(float shininess)
		{
			const float ns = std::max(0.0f, shininess);
			return std::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.04f, 1.0f);
		}

		void SetTexture(SourceMaterial& material, SourceTextureSlot slot, std::string path)
		{
			material.texturePaths[static_cast<size_t>(slot)] = std::move(path);
		}

		bool LoadMtl(const fs::path& mtlPath, ModelSource& out,
			std::unordered_map<std::string, uint32_t>& byName, std::wstring& error)
		{
			std::string text;
			if (!ReadWholeFile(mtlPath, text, error)) return false;

			SourceMaterial* current = nullptr;

			char* cursor = text.data();
			char* const last = text.data() + text.size();
			while (cursor < last)
			{
				char* lineStart = cursor;
				char* lineEnd = lineStart;
				while (lineEnd < last && *lineEnd != '\n') ++lineEnd;
				cursor = lineEnd + 1;
				*lineEnd = '\0';

				const char* p = SkipSpaces(lineStart);
				if (!*p || *p == '#') continue;

				if (Keyword(p, "newmtl"))
				{
					if (out.materials.size() >= kMaxMaterials)
					{
						error = L"MTL 재질이 너무 많습니다: " + mtlPath.wstring();
						return false;
					}
					std::string name = FirstToken(p);
					if (name.size() > kMaxNameLength) name.resize(kMaxNameLength);

					const uint32_t index = static_cast<uint32_t>(out.materials.size());
					SourceMaterial material;
					material.name = name;
					out.materials.push_back(std::move(material));
					current = &out.materials.back();
					byName[name] = index;      // 같은 이름이 또 나오면 뒤에 나온 것을 쓴다
					continue;
				}

				if (!current) continue;        // newmtl 앞의 줄은 버린다

				float r = 0.0f, g = 0.0f, b = 0.0f;
				if (Keyword(p, "Kd"))
				{
					if (ReadFloat(p, r) && ReadFloat(p, g) && ReadFloat(p, b))
						current->baseColor = { r, g, b, current->baseColor.w };
				}
				else if (Keyword(p, "Ke"))
				{
					if (ReadFloat(p, r) && ReadFloat(p, g) && ReadFloat(p, b))
						current->emissive = { r, g, b };
				}
				else if (Keyword(p, "d"))
				{
					float alpha = 1.0f;
					if (ReadFloat(p, alpha)) current->baseColor.w = std::clamp(alpha, 0.0f, 1.0f);
				}
				else if (Keyword(p, "Tr"))
				{
					float transparency = 0.0f;
					if (ReadFloat(p, transparency))
						current->baseColor.w = std::clamp(1.0f - transparency, 0.0f, 1.0f);
				}
				else if (Keyword(p, "Ns"))
				{
					float shininess = 0.0f;
					if (ReadFloat(p, shininess)) current->roughness = RoughnessFromShininess(shininess);
				}
				else if (Keyword(p, "Pr"))      // PBR 확장 — 있으면 Ns 환산보다 믿는다
				{
					float roughness = 0.0f;
					if (ReadFloat(p, roughness)) current->roughness = std::clamp(roughness, 0.04f, 1.0f);
				}
				else if (Keyword(p, "Pm"))
				{
					float metallic = 0.0f;
					if (ReadFloat(p, metallic)) current->metallic = std::clamp(metallic, 0.0f, 1.0f);
				}
				else if (Keyword(p, "map_Kd"))
					SetTexture(*current, SourceTextureSlot::BaseColor, LastToken(p));
				else if (Keyword(p, "map_Bump") || Keyword(p, "bump") || Keyword(p, "norm"))
					SetTexture(*current, SourceTextureSlot::Normal, LastToken(p));
				else if (Keyword(p, "map_Pr") || Keyword(p, "map_Ns"))
					SetTexture(*current, SourceTextureSlot::Roughness, LastToken(p));
				else if (Keyword(p, "map_Pm") || Keyword(p, "map_refl"))
					SetTexture(*current, SourceTextureSlot::Metallic, LastToken(p));
				else if (Keyword(p, "map_Ke"))
					SetTexture(*current, SourceTextureSlot::Emissive, LastToken(p));
			}
			return true;
		}

	} // namespace

	bool ObjReader::Matches(const uint8_t*, size_t, const char* ext) const
	{
		return ext && std::strcmp(ext, ".obj") == 0;
	}

	bool ObjReader::Read(const wchar_t* path, const ReadOptions& options,
		ModelSource& out, std::wstring& error)
	{
		const fs::path objPath(path);
		std::string text;
		if (!ReadWholeFile(objPath, text, error)) return false;

		std::vector<Vec3> positions;
		std::vector<Vec2> uvs;
		std::vector<Vec3> normals;

		std::unordered_map<std::string, uint32_t> materialByName;
		uint32_t currentMaterial = kInvalidIndex;

		// ★ o / g 를 노드로 남긴다 (2026-10-08)
		//   예전에는 파일 하나 = 노드 하나였다. 그런데 맵 OBJ 는 오브젝트 이름으로 용도를 가른다 —
		//   `Shared::PlanetSurface` 가 «Asphalt·Sidewalk·Planet_Core …» 이름만 접지면으로 골라 쓴다.
		//   이름을 버리면 그 선별이 불가능하므로 오브젝트마다 노드를 만들고 이름을 남긴다.
		//   o/g 가 없는 파일은 파일 이름으로 노드 하나를 만든다(예전 동작과 같다).
		std::string currentObject;
		uint32_t    currentNode = kInvalidIndex;

		// 메시는 (노드, 재질) 조합마다 하나다. 면이 하나라도 올 때 만든다 —
		// 그래야 쓰이지 않은 usemtl·빈 오브젝트 때문에 빈 메시가 남지 않는다.
		std::unordered_map<uint64_t, uint32_t> submeshByKey;
		std::vector<std::unordered_map<VertexKey, uint32_t, VertexKeyHash>> dedupe;

		std::vector<Corner> corners;
		size_t lineNumber = 0;

		auto fail = [&](const wchar_t* what) {
			error = what;
			error += L" (";
			error += objPath.filename().wstring();
			error += L" ";
			error += std::to_wstring(lineNumber);
			error += L"번째 줄)";
			return false;
		};

		char* cursor = text.data();
		char* const last = text.data() + text.size();
		while (cursor < last)
		{
			++lineNumber;
			char* lineStart = cursor;
			char* lineEnd = lineStart;
			while (lineEnd < last && *lineEnd != '\n') ++lineEnd;
			cursor = lineEnd + 1;
			*lineEnd = '\0';          // 이 줄만 파싱하도록 끊는다

			const char* p = SkipSpaces(lineStart);
			if (!*p || *p == '#') continue;

			// ── 좌표 ────────────────────────────────────
			if (Keyword(p, "v"))
			{
				Vec3 position{};
				if (!ReadFloat(p, position.x) || !ReadFloat(p, position.y) || !ReadFloat(p, position.z))
					return fail(L"OBJ 정점 좌표를 읽지 못했습니다");
				if (positions.size() >= kMaxVertices)
					return fail(L"OBJ 정점이 너무 많습니다");
				positions.push_back(position);
				continue;
			}
			if (Keyword(p, "vt"))
			{
				Vec2 uv{};
				if (!ReadFloat(p, uv.x) || !ReadFloat(p, uv.y))
					return fail(L"OBJ 텍스처 좌표를 읽지 못했습니다");
				// ★ V 뒤집기 — OBJ 는 아래에서 위로(OpenGL), D3D 텍스처는 위에서 아래다.
				//   FBX 경로도 같은 처리를 한다(FbxModelLoader.cpp: 1.0f - uv[1]).
				uv.y = 1.0f - uv.y;
				if (uvs.size() >= kMaxVertices)
					return fail(L"OBJ 텍스처 좌표가 너무 많습니다");
				uvs.push_back(uv);
				continue;
			}
			if (Keyword(p, "vn"))
			{
				Vec3 normal{};
				if (!ReadFloat(p, normal.x) || !ReadFloat(p, normal.y) || !ReadFloat(p, normal.z))
					return fail(L"OBJ 법선을 읽지 못했습니다");
				if (normals.size() >= kMaxVertices)
					return fail(L"OBJ 법선이 너무 많습니다");
				normals.push_back(normal);
				continue;
			}

			// ── 재질 ────────────────────────────────────
			//  geometryOnly(서버) 면 재질·텍스처를 아예 읽지 않는다. 메시도 하나로 합친다.
			if (Keyword(p, "mtllib"))
			{
				if (options.geometryOnly) continue;
				const std::string name = LastToken(p);
				if (name.empty()) continue;
				const fs::path mtlPath = objPath.parent_path() / Utf8Path(name);
				if (!LoadMtl(mtlPath, out, materialByName, error)) return false;
				continue;
			}
			if (Keyword(p, "usemtl"))
			{
				if (options.geometryOnly) continue;
				const std::string name = FirstToken(p);
				if (auto it = materialByName.find(name); it != materialByName.end())
				{
					currentMaterial = it->second;
				}
				else if (out.materials.size() < kMaxMaterials)
				{
					// MTL 이 없거나 이름이 어긋났다. 기본 재질을 만들어 두어야
					// 재질별 메시 분리와 이름이 남는다 — 조용히 합치지 않는다.
					currentMaterial = static_cast<uint32_t>(out.materials.size());
					SourceMaterial material;
					material.name = name;
					out.materials.push_back(std::move(material));
					materialByName[name] = currentMaterial;
				}
				continue;
			}

			// o·g 는 오브젝트 경계다. 이름을 기억해 두고, 면이 올 때 노드를 만든다.
			// (OBJ 의 그룹은 계층이 아니라 평면 묶음이라 전부 루트 노드가 된다)
			if (Keyword(p, "o") || Keyword(p, "g"))
			{
				std::string name = FirstToken(p);
				if (name.size() > kMaxNameLength) name.resize(kMaxNameLength);
				if (name != currentObject)
				{
					currentObject = std::move(name);
					currentNode = kInvalidIndex;   // 다음 면에서 새로 만든다
				}
				continue;
			}
			if (Keyword(p, "s")) continue;   // 스무딩 그룹은 쓰지 않는다

			// ── 면 ─────────────────────────────────────
			if (Keyword(p, "f"))
			{
				corners.clear();
				Corner corner;
				while (ReadCorner(p, corner)) corners.push_back(corner);
				if (corners.size() < 3)
					return fail(L"OBJ 면의 모서리가 3개보다 적습니다");

				// 이 면이 들어갈 노드. o/g 가 없으면 파일 이름으로 하나만 만든다.
				if (currentNode == kInvalidIndex)
				{
					if (out.nodes.size() >= kMaxNodes)
						return fail(L"OBJ 오브젝트가 너무 많습니다");
					currentNode = static_cast<uint32_t>(out.nodes.size());
					SourceNode node;
					node.name = currentObject.empty() ? ToUtf8(objPath.stem()) : currentObject;
					node.parent = kInvalidIndex;
					out.nodes.push_back(std::move(node));
				}

				const uint32_t materialKey = options.geometryOnly ? kInvalidIndex : currentMaterial;
				const uint64_t submeshKey =
					(static_cast<uint64_t>(currentNode) << 32) | static_cast<uint64_t>(materialKey);

				uint32_t meshIndex;
				if (auto it = submeshByKey.find(submeshKey); it != submeshByKey.end())
				{
					meshIndex = it->second;
				}
				else
				{
					if (out.meshes.size() >= kMaxMeshes)
						return fail(L"OBJ 메시가 너무 많습니다");
					meshIndex = static_cast<uint32_t>(out.meshes.size());
					SourceMesh mesh;
					mesh.material = materialKey;
					out.meshes.push_back(std::move(mesh));
					dedupe.emplace_back();
					submeshByKey.emplace(submeshKey, meshIndex);
					out.nodes[currentNode].meshes.push_back(meshIndex);
				}

				SourceMesh& mesh = out.meshes[meshIndex];
				auto& keys = dedupe[meshIndex];

				// 다각형은 부채꼴로 쪼갠다(볼록 가정). 설계안 3장의 삼각화 단계다.
				for (size_t corner1 = 1; corner1 + 1 < corners.size(); ++corner1)
				{
					const Corner triangle[3] = { corners[0], corners[corner1], corners[corner1 + 1] };

					VertexKey key[3]{};
					SourceVertex vertex[3]{};
					bool hasFileNormals = true;

					for (int i = 0; i < 3; ++i)
					{
						size_t index = 0;
						if (!Resolve(triangle[i].position, positions.size(), index))
							return fail(L"OBJ 면의 정점 인덱스가 범위를 벗어났습니다");
						vertex[i].position = positions[index];
						key[i].position = static_cast<uint32_t>(index) + 1;

						if (triangle[i].uv != 0 && !options.geometryOnly)
						{
							if (!Resolve(triangle[i].uv, uvs.size(), index))
								return fail(L"OBJ 면의 UV 인덱스가 범위를 벗어났습니다");
							vertex[i].uv = uvs[index];
							key[i].uv = static_cast<uint32_t>(index) + 1;
						}

						if (triangle[i].normal != 0)
						{
							if (!Resolve(triangle[i].normal, normals.size(), index))
								return fail(L"OBJ 면의 법선 인덱스가 범위를 벗어났습니다");
							vertex[i].normal = normals[index];
							key[i].normal = static_cast<uint32_t>(index) + 1;
						}
						else
						{
							hasFileNormals = false;   // 0 으로 두면 후처리가 면 법선을 넣는다
						}
					}

					// ★ 와인딩 — 파일에 법선이 있으면 그것과 맞춘다
					//   면 법선(변의 외적)이 음영 법선과 반대로 보면 두 모서리를 맞바꾼다.
					//   FbxModelLoader 가 쓰는 방법과 같다. 파일의 와인딩 규약을 믿지 않아도 된다.
					//   법선이 없는 파일은 적힌 순서를 그대로 둔다 — 우리 월드는 왼손좌표계이고
					//   블렌더 내보내기(Forward -Z / Up Y)가 그 순서로 내보낸다.
					int order[3] = { 0, 1, 2 };
					if (hasFileNormals)
					{
						const Vec3& a = vertex[0].position;
						const Vec3& b = vertex[1].position;
						const Vec3& c = vertex[2].position;
						const Vec3 edge1{ b.x - a.x, b.y - a.y, b.z - a.z };
						const Vec3 edge2{ c.x - a.x, c.y - a.y, c.z - a.z };
						const Vec3 faceNormal{
							edge1.y * edge2.z - edge1.z * edge2.y,
							edge1.z * edge2.x - edge1.x * edge2.z,
							edge1.x * edge2.y - edge1.y * edge2.x };
						const Vec3 shading{
							vertex[0].normal.x + vertex[1].normal.x + vertex[2].normal.x,
							vertex[0].normal.y + vertex[1].normal.y + vertex[2].normal.y,
							vertex[0].normal.z + vertex[1].normal.z + vertex[2].normal.z };
						const float agreement = faceNormal.x * shading.x +
							faceNormal.y * shading.y + faceNormal.z * shading.z;
						if (agreement < 0.0f) std::swap(order[1], order[2]);
					}

					if (mesh.indices.size() + 3 > kMaxIndices)
						return fail(L"OBJ 인덱스가 너무 많습니다");

					for (int slot = 0; slot < 3; ++slot)
					{
						const int i = order[slot];
						uint32_t vertexIndex;
						if (auto it = keys.find(key[i]); it != keys.end())
						{
							vertexIndex = it->second;
						}
						else
						{
							if (mesh.vertices.size() >= kMaxVertices)
								return fail(L"OBJ 메시의 정점이 너무 많습니다");
							vertexIndex = static_cast<uint32_t>(mesh.vertices.size());
							mesh.vertices.push_back(vertex[i]);
							keys.emplace(key[i], vertexIndex);
						}
						mesh.indices.push_back(vertexIndex);
					}
				}
				continue;
			}

			// 모르는 키워드는 조용히 넘긴다. OBJ 는 확장이 많고, 모르는 줄이 오류일 이유가 없다.
		}

		if (out.meshes.empty())
		{
			error = L"OBJ 에 면(f)이 없습니다: " + objPath.filename().wstring();
			return false;
		}

		// 노드는 면을 읽는 동안 오브젝트(o/g)마다 만들었다. 전부 루트다 — OBJ 에 계층이 없다.
		return true;
	}

} // namespace Shared
