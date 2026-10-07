#include "ObjModelLoader.h"
#include "TextureLoader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <climits>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace swc {

	namespace {

		using namespace DirectX;
		namespace fs = std::filesystem;
		constexpr size_t kMaximumPolygonCorners = 65536;

		bool IsSpace(char value)
		{
			return value == ' ' || value == '\t' || value == '\r';
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
			if (text.empty()) return {};
			const size_t end = text.find_first_of(" \t\r");
			const std::string_view token = text.substr(0, end);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
			return token;
		}

		std::string_view Unquote(std::string_view text)
		{
			text = Trim(text);
			if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
				(text.front() == '\'' && text.back() == '\'')))
				text = text.substr(1, text.size() - 2);
			return text;
		}

		std::string_view WithoutComment(std::string_view text)
		{
			char quote = 0;
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char value = text[i];
				if (quote)
				{
					if (value == quote) quote = 0;
				}
				else if (value == '"' || value == '\'') quote = value;
				else if (value == '#') return text.substr(0, i);
			}
			return text;
		}

		std::wstring FromText(std::string_view text)
		{
			if (text.empty() || text.size() > INT_MAX) return {};
			UINT codePage = CP_UTF8;
			DWORD flags = MB_ERR_INVALID_CHARS;
			int count = MultiByteToWideChar(codePage, flags, text.data(), int(text.size()), nullptr, 0);
			if (count <= 0)
			{
				// 오래된 Windows OBJ 내보내기의 로컬 코드 페이지도 허용한다.
				codePage = CP_ACP;
				flags = 0;
				count = MultiByteToWideChar(codePage, flags, text.data(), int(text.size()), nullptr, 0);
			}
			if (count <= 0) return {};
			std::wstring result(size_t(count), L'\0');
			if (!MultiByteToWideChar(codePage, flags, text.data(), int(text.size()), result.data(), count))
				return {};
			return result;
		}

		bool Fail(std::wstring& error, const fs::path& path, size_t line, const std::wstring& reason)
		{
			error = path.wstring();
			if (line) error += L" (" + std::to_wstring(line) + L"행)";
			error += L": " + reason;
			return false;
		}

		bool ParseFloat(std::string_view token, float& value, bool requireFinite = true)
		{
			if (!token.empty() && token.front() == '+') token.remove_prefix(1);
			if (token.empty()) return false;
			const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
				(!requireFinite || std::isfinite(value));
		}

		bool ParseIndex(std::string_view token, size_t count, uint32_t& index)
		{
			if (!token.empty() && token.front() == '+') token.remove_prefix(1);
			if (token.empty()) return false;
			int64_t value = 0;
			const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
			if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value == 0)
				return false;
			if (value > 0)
			{
				if (uint64_t(value) > count) return false;
				index = uint32_t(value - 1);
			}
			else
			{
				if (value < -int64_t(count)) return false;
				index = uint32_t(int64_t(count) + value);
			}
			return true;
		}

		struct Vector
		{
			double x = 0, y = 0, z = 0;
			Vector operator+(const Vector& value) const { return { x + value.x, y + value.y, z + value.z }; }
			Vector operator-(const Vector& value) const { return { x - value.x, y - value.y, z - value.z }; }
			Vector operator*(double scale) const { return { x * scale, y * scale, z * scale }; }
			Vector& operator+=(const Vector& value) { x += value.x; y += value.y; z += value.z; return *this; }
		};

		Vector ToVector(const XMFLOAT3& value) { return { value.x, value.y, value.z }; }
		double Dot(const Vector& one, const Vector& two) { return one.x * two.x + one.y * two.y + one.z * two.z; }
		Vector Cross(const Vector& one, const Vector& two)
		{
			return { one.y * two.z - one.z * two.y, one.z * two.x - one.x * two.z,
				one.x * two.y - one.y * two.x };
		}

		bool Normalize(const Vector& value, XMFLOAT3& out)
		{
			const double lengthSq = Dot(value, value);
			if (!std::isfinite(lengthSq) || lengthSq < 1.0e-20) return false;
			const double inverseLength = 1.0 / std::sqrt(lengthSq);
			out = { float(value.x * inverseLength), float(value.y * inverseLength), float(value.z * inverseLength) };
			return true;
		}

		bool ValidNormal(const XMFLOAT3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
				Dot(ToVector(value), ToVector(value)) >= 1.0e-20;
		}

		fs::path FindFile(const fs::path& folder, std::string_view reference)
		{
			const std::wstring name = FromText(Unquote(reference));
			if (name.empty()) return {};
			const fs::path requested(name);
			const fs::path candidates[] = {
				requested.is_relative() ? folder / requested : fs::path(),
				folder / requested.filename(), requested.is_absolute() ? requested : fs::path() };
			for (const fs::path& candidate : candidates)
			{
				std::error_code ec;
				if (!candidate.empty() && fs::is_regular_file(candidate, ec)) return candidate.lexically_normal();
			}
			return {};
		}

		struct MapReference
		{
			std::string name;
			fs::path file;
			size_t line = 0;
			XMFLOAT2 scale{ 1, 1 };
			XMFLOAT2 offset{ 0, 0 };
		};

		struct MaterialRecord
		{
			bool defined = false;
			bool hasRoughness = false;
			bool used = false;
			bool textured = false;
			bool normalMapped = false;
			fs::path file;
			size_t line = 0;
			std::array<MapReference, kMaterialTextureCount> maps;
			XMFLOAT2 scale{ 1, 1 };
			XMFLOAT2 offset{ 0, 0 };
		};

		struct Corner
		{
			uint32_t position = kInvalidModelIndex;
			uint32_t uv = kInvalidModelIndex;
			uint32_t normal = kInvalidModelIndex;
		};

		struct VertexKey
		{
			uint32_t position, uv, normal;
			uint64_t smoothing;
			int handedness;
			bool operator==(const VertexKey& other) const
			{
				return position == other.position && uv == other.uv && normal == other.normal &&
					smoothing == other.smoothing && handedness == other.handedness;
			}
		};

		struct VertexHash
		{
			size_t operator()(const VertexKey& key) const
			{
				size_t value = size_t(key.position) * 0x9e3779b1u + key.uv;
				value = (value ^ size_t(key.normal)) * 0x85ebca6bu;
				value ^= size_t(key.smoothing) + (value << 6) + (value >> 2);
				return value ^ (size_t(key.handedness + 1) * 0xc2b2ae35u);
			}
		};

		struct NormalKey
		{
			uint32_t position;
			uint64_t smoothing;
			bool operator==(const NormalKey& other) const
			{
				return position == other.position && smoothing == other.smoothing;
			}
		};

		struct NormalHash
		{
			size_t operator()(const NormalKey& key) const
			{
				return size_t(key.position) * 0x9e3779b1u ^ (size_t(key.smoothing) * 0x85ebca6bu);
			}
		};

		struct NormalSum { Vector sum; Vector fallback; };
		struct NormalReference { uint32_t mesh, vertex, normal; };
		struct Point2 { double x, y; };

		double Turn(const Point2& one, const Point2& two, const Point2& three)
		{
			return (two.x - one.x) * (three.y - one.y) - (two.y - one.y) * (three.x - one.x);
		}

		// 평면에 투영해 볼록 면은 fan, 오목한 면은 ear clipping으로 삼각형화한다.
		// 면 전체의 코너 순서를 유지하므로 UV/법선 인덱스도 같은 코너를 따라간다.
		bool Triangulate(const std::vector<Corner>& polygon, const std::vector<XMFLOAT3>& positions,
			std::vector<std::array<size_t, 3>>& triangles, std::vector<Point2>& points,
			std::vector<size_t>& remaining)
		{
			triangles.clear();
			if (polygon.size() == 3) { triangles.push_back({ 0, 1, 2 }); return true; }
			Vector normal;
			for (size_t i = 0; i < polygon.size(); ++i)
			{
				const Vector one = ToVector(positions[polygon[i].position]);
				const Vector two = ToVector(positions[polygon[(i + 1) % polygon.size()].position]);
				normal.x += (one.y - two.y) * (one.z + two.z);
				normal.y += (one.z - two.z) * (one.x + two.x);
				normal.z += (one.x - two.x) * (one.y + two.y);
			}
			if (Dot(normal, normal) < 1.0e-20) return true;
			const double nx = std::fabs(normal.x), ny = std::fabs(normal.y), nz = std::fabs(normal.z);
			const int axis = nx > ny && nx > nz ? 0 : ny > nz ? 1 : 2;
			points.clear();
			for (const Corner& corner : polygon)
			{
				const XMFLOAT3& point = positions[corner.position];
				points.push_back(axis == 0 ? Point2{ point.y, point.z } :
					axis == 1 ? Point2{ point.x, point.z } : Point2{ point.x, point.y });
			}
			double area = 0;
			for (size_t i = 0; i < points.size(); ++i)
				area += points[i].x * points[(i + 1) % points.size()].y -
					points[(i + 1) % points.size()].x * points[i].y;
			if (std::fabs(area) < 1.0e-10) return true;
			const double sign = area < 0 ? -1.0 : 1.0;
			const double epsilon = std::fabs(area) * 1.0e-12;
			bool convex = true;
			for (size_t i = 0; i < points.size(); ++i)
				if (sign * Turn(points[(i + points.size() - 1) % points.size()], points[i],
					points[(i + 1) % points.size()]) < -epsilon) { convex = false; break; }
			if (convex)
			{
				for (size_t i = 1; i + 1 < polygon.size(); ++i) triangles.push_back({ 0, i, i + 1 });
				return true;
			}
			remaining.resize(points.size());
			for (size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
			while (remaining.size() > 3)
			{
				bool clipped = false;
				for (size_t i = 0; i < remaining.size(); ++i)
				{
					const size_t a = remaining[(i + remaining.size() - 1) % remaining.size()];
					const size_t b = remaining[i], c = remaining[(i + 1) % remaining.size()];
					const double turn = sign * Turn(points[a], points[b], points[c]);
					if (std::fabs(turn) <= epsilon)
					{
						remaining.erase(remaining.begin() + i); clipped = true; break;
					}
					if (turn < 0) continue;
					bool contains = false;
					for (size_t test : remaining)
					{
						if (test == a || test == b || test == c) continue;
						if (sign * Turn(points[a], points[b], points[test]) >= -epsilon &&
							sign * Turn(points[b], points[c], points[test]) >= -epsilon &&
							sign * Turn(points[c], points[a], points[test]) >= -epsilon)
						{ contains = true; break; }
					}
					if (contains) continue;
					triangles.push_back({ a, b, c });
					remaining.erase(remaining.begin() + i); clipped = true; break;
				}
				if (!clipped) return false;
			}
			if (remaining.size() == 3) triangles.push_back({ remaining[0], remaining[1], remaining[2] });
			return true;
		}

		class ImportContext
		{
		public:
			ImportContext(const fs::path& path, ModelData& output, std::wstring& message)
				: modelPath(path), data(output), error(message)
			{
				std::wstring filename = path.filename().wstring();
				std::transform(filename.begin(), filename.end(), filename.begin(),
					[](wchar_t value) { return wchar_t(std::towlower(value)); });
				isRuinsPlanet = filename == L"future_ruins_realistic.obj";
				ModelNodeData root;
				root.name = "OBJ";
				data.nodes.push_back(std::move(root));
			}

			bool Read()
			{
				// 원본 파일 전체나 모든 면을 복사하지 않고 한 줄씩 변환한다.
				std::array<char, 64 * 1024> buffer{};
				std::ifstream stream;
				stream.rdbuf()->pubsetbuf(buffer.data(), std::streamsize(buffer.size()));
				stream.open(modelPath, std::ios::binary);
				if (!stream) return Fail(error, modelPath, 0, L"OBJ 파일을 열 수 없습니다.");
				std::string line;
				std::vector<Corner> polygon;
				std::vector<std::array<size_t, 3>> triangles;
				std::vector<Point2> projected;
				std::vector<size_t> remaining;
				polygon.reserve(8); triangles.reserve(8); projected.reserve(8); remaining.reserve(8);
				while (std::getline(stream, line))
				{
					++lineNumber;
					if (!JoinContinuation(stream, line, modelPath, lineNumber)) return false;
					std::string_view text(line);
					if (lineNumber == 1 && text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
					text = Trim(WithoutComment(text));
					const std::string_view type = NextToken(text);
					if (type == "version" && lineNumber == 1)
						return Error(L"Git LFS 포인터입니다. 모델 원본을 내려받으세요.");
					if (type == "v") { if (!ReadPosition(text)) return false; }
					else if (type == "vt") { if (!ReadUv(text)) return false; }
					else if (type == "vn") { if (!ReadNormal(text)) return false; }
					else if (type == "mtllib") { if (!ReadLibraries(text)) return false; }
					else if (type == "o" || type == "g")
					{
						if (!FlushMesh()) return false;
						const std::string name(Unquote(text));
						if (type == "o")
						{
							++objectNumber;
							if (objectNumber >= 0x80000000u) return Error(L"객체 수가 지원 범위를 넘었습니다.");
							objectName = name; groupName.clear();
							if (!AddNode(name.empty() ? "Object" : name, 0, objectNode)) return false;
							activeNode = objectNode;
						}
						else
						{
							groupName = name;
							if (name.empty() || name == "off") activeNode = objectNode;
							else if (!AddNode(name, objectNode, activeNode)) return false;
						}
					}
					else if (type == "usemtl")
					{
						if (!FlushMesh()) return false;
						if (!GetMaterial(std::string(Unquote(text)), material)) return false;
					}
					else if (type == "s")
					{
						const std::string_view value = Trim(text);
						if (value == "off" || value == "0") smoothingGroup = 0;
						else if (value == "on") smoothingGroup = 1;
						else
						{
							const auto parsed = std::from_chars(value.data(), value.data() + value.size(), smoothingGroup);
							if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
								return Error(L"스무딩 그룹 번호가 올바르지 않습니다.");
						}
					}
					else if (type == "f")
					{
						++faceNumber;
						if (faceNumber >= (uint64_t(1) << 63)) return Error(L"면 수가 지원 범위를 넘었습니다.");
						if (SkipAtmosphere()) continue;
						polygon.clear();
						for (std::string_view token = NextToken(text); !token.empty(); token = NextToken(text))
						{
							Corner corner;
							if (!ReadCorner(token, corner)) return Error(L"면의 위치/UV/법선 인덱스가 올바르지 않습니다.");
							if (polygon.size() >= kMaximumPolygonCorners) return Error(L"면의 코너 수가 지원 범위를 넘었습니다.");
							polygon.push_back(corner);
						}
						if (polygon.size() < 3) return Error(L"면의 정점이 3개보다 적습니다.");
						if (!Triangulate(polygon, positions, triangles, projected, remaining))
							return Error(L"다각형을 삼각형으로 나눌 수 없습니다. 교차하거나 비평면인 면을 확인하세요.");
						for (const auto& triangle : triangles)
							if (!AddTriangle(polygon[triangle[0]], polygon[triangle[2]], polygon[triangle[1]])) return false;
					}
					else if (type == "curv" || type == "curv2" || type == "surf")
						return Error(L"곡선/곡면 OBJ는 지원하지 않습니다. 메시로 변환해서 내보내세요.");
				}
				if (stream.bad()) return Error(L"OBJ 파일을 읽는 도중 오류가 발생했습니다.");
				if (!FlushMesh()) return false;
				if (!hasBounds || data.meshes.empty()) return Error(L"표시할 유효한 삼각형이 없습니다.");
				// 큰 입력 배열과 해시 테이블은 텍스처/접선 처리 전에 해제한다.
				std::vector<XMFLOAT3>().swap(positions);
				std::vector<XMFLOAT3>().swap(normals);
				std::vector<XMFLOAT2>().swap(uvs);
				std::vector<XMFLOAT3>().swap(colors);
				vertexIndices.clear(); vertexIndices.rehash(0);
				for (const NormalReference& reference : normalReferences)
				{
					const NormalSum& sum = normalSums[reference.normal];
					XMFLOAT3& normal = data.meshes[reference.mesh].mesh.vertices[reference.vertex].normal;
					if (!Normalize(sum.sum, normal) && !Normalize(sum.fallback, normal))
						return Error(L"정점 법선을 복구할 수 없습니다.");
				}
				normalIndices.clear(); normalIndices.rehash(0);
				std::vector<NormalSum>().swap(normalSums);
				std::vector<NormalReference>().swap(normalReferences);
				if (!FinishMaterials()) return false;
				for (ModelMeshData& mesh : data.meshes)
					if (!FinishMesh(mesh)) return false;
				return true;
			}

		private:
			bool Error(const std::wstring& reason) { return Fail(error, modelPath, lineNumber, reason); }

			bool JoinContinuation(std::ifstream& stream, std::string& line, const fs::path& path, size_t& number)
			{
				while (!Trim(line).empty() && Trim(line).back() == '\\')
				{
					const size_t slash = line.find_last_not_of(" \t\r");
					line.resize(slash);
					std::string next;
					if (!std::getline(stream, next)) return Fail(error, path, number, L"줄 연속 기호 뒤의 데이터가 없습니다.");
					++number;
					line += ' '; line += next;
					if (line.size() > 16 * 1024 * 1024) return Fail(error, path, number, L"한 줄의 데이터가 너무 큽니다.");
				}
				return true;
			}

			bool ReadPosition(std::string_view text)
			{
				std::array<float, 7> values{};
				size_t count = 0;
				for (std::string_view token = NextToken(text); !token.empty(); token = NextToken(text))
				{
					if (count >= values.size() || !ParseFloat(token, values[count])) return Error(L"정점 위치/색상 값이 올바르지 않습니다.");
					++count;
				}
				if (count != 3 && count != 4 && count != 6 && count != 7)
					return Error(L"정점은 XYZ, XYZW 또는 XYZ(RGB/RGBA) 형식이어야 합니다.");
				const float w = count == 4 ? values[3] : 1.0f;
				if (w == 0 || !std::isfinite(values[0] / w) || !std::isfinite(values[1] / w) || !std::isfinite(values[2] / w))
					return Error(L"정점의 동차 좌표 W가 올바르지 않습니다.");
				if (positions.size() >= kInvalidModelIndex) return Error(L"정점 수가 인덱스 범위를 넘었습니다.");
				const bool colored = count >= 6;
				if (colored && colors.empty()) colors.resize(positions.size(), XMFLOAT3{ 1, 1, 1 });
				positions.push_back({ values[0] / w, values[1] / w, -values[2] / w });
				if (colored || !colors.empty())
				{
					// Vertex 색상은 RGB만 사용한다. RGBA 확장의 A는 재질 투명도를 대신하지 않는다.
					colors.push_back(colored ? XMFLOAT3{ values[3], values[4], values[5] } : XMFLOAT3{ 1, 1, 1 });
				}
				return true;
			}

			bool ReadUv(std::string_view text)
			{
				XMFLOAT2 value{};
				if (!ParseFloat(NextToken(text), value.x)) return Error(L"텍스처 좌표 U가 올바르지 않습니다.");
				const std::string_view v = NextToken(text);
				if (!v.empty() && !ParseFloat(v, value.y)) return Error(L"텍스처 좌표 V가 올바르지 않습니다.");
				const std::string_view w = NextToken(text);
				float unused = 0;
				if ((!w.empty() && !ParseFloat(w, unused)) || !NextToken(text).empty()) return Error(L"텍스처 좌표 형식이 올바르지 않습니다.");
				if (uvs.size() >= kInvalidModelIndex) return Error(L"텍스처 좌표 수가 인덱스 범위를 넘었습니다.");
				// MTL의 UV 변환을 적용한 뒤 FinishMesh에서 V를 반전한다.
				uvs.push_back(value);
				return true;
			}

			bool ReadNormal(std::string_view text)
			{
				XMFLOAT3 value{};
				if (!ParseFloat(NextToken(text), value.x, false) || !ParseFloat(NextToken(text), value.y, false) ||
					!ParseFloat(NextToken(text), value.z, false) || !NextToken(text).empty())
					return Error(L"법선 값의 형식이 올바르지 않습니다.");
				value.z = -value.z;
				XMFLOAT3 normalized{};
				Normalize(ToVector(value), normalized);
				if (normals.size() >= kInvalidModelIndex) return Error(L"법선 수가 인덱스 범위를 넘었습니다.");
				// 길이가 0이거나 비정상인 법선은 참조하는 면의 기하 정보로 복구한다.
				normals.push_back(normalized);
				return true;
			}

			bool ReadCorner(std::string_view token, Corner& corner)
			{
				const size_t slash = token.find('/');
				if (!ParseIndex(token.substr(0, slash), positions.size(), corner.position)) return false;
				if (slash == std::string_view::npos) return true;
				token.remove_prefix(slash + 1);
				const size_t second = token.find('/');
				const std::string_view uv = token.substr(0, second);
				if (!uv.empty() && !ParseIndex(uv, uvs.size(), corner.uv)) return false;
				if (second == std::string_view::npos) return !uv.empty();
				token.remove_prefix(second + 1);
				return !token.empty() && ParseIndex(token, normals.size(), corner.normal);
			}

			bool AddNode(const std::string& name, uint32_t parent, uint32_t& index)
			{
				if (data.nodes.size() >= kInvalidModelIndex) return Error(L"노드 수가 인덱스 범위를 넘었습니다.");
				ModelNodeData node; node.name = name; node.parent = parent;
				index = uint32_t(data.nodes.size());
				data.nodes.push_back(std::move(node));
				return true;
			}

			bool GetMaterial(const std::string& name, uint32_t& index)
			{
				if (auto it = materialIndices.find(name); it != materialIndices.end()) { index = it->second; return true; }
				if (data.materials.size() >= kInvalidModelIndex) return Error(L"재질 수가 인덱스 범위를 넘었습니다.");
				MaterialData value; value.name = name.empty() ? "Default" : name;
				MaterialRecord record; record.defined = name.empty(); record.file = modelPath; record.line = lineNumber;
				index = uint32_t(data.materials.size());
				data.materials.push_back(std::move(value)); records.push_back(std::move(record));
				materialIndices.emplace(name, index);
				return true;
			}

			bool SkipAtmosphere() const
			{
				return isRuinsPlanet && (objectName == "HQ_Subtle_Planetary_Dust_Atmosphere" ||
					groupName == "HQ_Subtle_Planetary_Dust_Atmosphere" ||
					(material != kInvalidModelIndex && data.materials[material].name == "HQ_Radial_Dust_Atmosphere"));
			}

			uint64_t SmoothingDomain() const
			{
				return smoothingGroup ? (uint64_t(objectNumber) << 32) | smoothingGroup :
					(uint64_t(1) << 63) | faceNumber;
			}

			bool AddTriangle(Corner one, Corner two, Corner three)
			{
				Corner corners[3]{ one, two, three };
				Vector faceNormal = Cross(ToVector(positions[two.position]) - ToVector(positions[one.position]),
					ToVector(positions[three.position]) - ToVector(positions[one.position]));
				const double areaSq = Dot(faceNormal, faceNormal);
				if (!std::isfinite(areaSq)) return Error(L"면의 좌표 범위가 너무 큽니다.");
				if (areaSq < 1.0e-20) return true;
				Vector averageNormal;
				for (const Corner& corner : corners)
					if (corner.normal != kInvalidModelIndex) averageNormal += ToVector(normals[corner.normal]);
				// 일부 제작 면의 코너 순서와 명시적 법선이 반대이면 기존 렌더링 기준을 유지한다.
				if (Dot(faceNormal, averageNormal) < 0)
				{
					std::swap(corners[1], corners[2]); faceNormal = faceNormal * -1;
				}
				if (material == kInvalidModelIndex && !GetMaterial({}, material)) return false;
				if (building.mesh.indices.size() > size_t(UINT32_MAX) - 3) return Error(L"메시 인덱스 수가 지원 범위를 넘었습니다.");
				int handedness = 0;
				if (corners[0].uv != kInvalidModelIndex && corners[1].uv != kInvalidModelIndex && corners[2].uv != kInvalidModelIndex)
				{
					const XMFLOAT2& a = uvs[corners[0].uv]; const XMFLOAT2& b = uvs[corners[1].uv]; const XMFLOAT2& c = uvs[corners[2].uv];
					const double determinant = (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
					if (std::fabs(determinant) > 1.0e-12) handedness = determinant < 0 ? -1 : 1;
				}
				for (const Corner& corner : corners)
				{
					const bool rebuild = corner.normal == kInvalidModelIndex || !ValidNormal(normals[corner.normal]);
					const uint64_t domain = rebuild ? SmoothingDomain() : 0;
					uint32_t repair = kInvalidModelIndex;
					if (rebuild)
					{
						const NormalKey normalKey{ corner.position, domain };
						const auto found = normalIndices.find(normalKey);
						if (found != normalIndices.end()) repair = found->second;
						else
						{
							if (normalSums.size() >= kInvalidModelIndex) return Error(L"복구할 법선 수가 지원 범위를 넘었습니다.");
							repair = uint32_t(normalSums.size());
							normalIndices.emplace(normalKey, repair);
							normalSums.push_back({ {}, faceNormal });
						}
						normalSums[repair].sum += faceNormal;
					}
					const VertexKey key{ corner.position, corner.uv, rebuild ? kInvalidModelIndex : corner.normal, domain, handedness };
					const auto found = vertexIndices.find(key);
					uint32_t index = 0;
					if (found != vertexIndices.end()) index = found->second;
					else
					{
						if (building.mesh.vertices.size() >= kInvalidModelIndex) return Error(L"메시 정점 수가 지원 범위를 넘었습니다.");
						Vertex vertex;
						vertex.position = positions[corner.position];
						if (!rebuild) vertex.normal = normals[corner.normal];
						if (corner.uv != kInvalidModelIndex) vertex.uv = uvs[corner.uv];
						else missingUvs = true;
						if (!colors.empty()) vertex.color = colors[corner.position];
						index = uint32_t(building.mesh.vertices.size());
						building.mesh.vertices.push_back(vertex);
						vertexIndices.emplace(key, index);
						if (rebuild) normalReferences.push_back({ uint32_t(data.meshes.size()), index, repair });
						IncludePoint(vertex.position);
					}
					building.mesh.indices.push_back(index);
				}
				return true;
			}

			bool FlushMesh()
			{
				// 객체/재질 부분을 마치면 검색용 노드를 먼저 반환한다.
				vertexIndices.clear(); vertexIndices.rehash(0);
				if (!building.mesh.indices.empty())
				{
					if (data.meshes.size() >= kInvalidModelIndex) return Error(L"메시 수가 지원 범위를 넘었습니다.");
					// 완성한 메시의 여유 용량을 다음 메시 로딩 중에도 보관하지 않는다.
					building.mesh.vertices.shrink_to_fit();
					building.mesh.indices.shrink_to_fit();
					building.material = material;
					records[material].used = true;
					data.nodes[activeNode].meshes.push_back(uint32_t(data.meshes.size()));
					data.meshes.push_back(std::move(building));
					meshMissingUvs.push_back(missingUvs);
					building = ModelMeshData{};
				}
				missingUvs = false;
				return true;
			}

			void IncludePoint(const XMFLOAT3& point)
			{
				if (!hasBounds) { data.boundsMin = data.boundsMax = point; hasBounds = true; return; }
				data.boundsMin.x = std::min(data.boundsMin.x, point.x); data.boundsMin.y = std::min(data.boundsMin.y, point.y);
				data.boundsMin.z = std::min(data.boundsMin.z, point.z); data.boundsMax.x = std::max(data.boundsMax.x, point.x);
				data.boundsMax.y = std::max(data.boundsMax.y, point.y); data.boundsMax.z = std::max(data.boundsMax.z, point.z);
			}

			bool ReadLibraries(std::string_view text)
			{
				text = Trim(text);
				if (text.empty()) return Error(L"MTL 파일 이름이 비어 있습니다.");
				// 공백이 있는 단일 이름을 먼저 찾고, 없으면 여러 mtllib 이름으로 해석한다.
				const fs::path whole = FindFile(modelPath.parent_path(), text);
				if (!whole.empty()) return ReadMaterialFile(whole);
				while (!text.empty())
				{
					text = Trim(text);
					std::string_view name;
					if (text.front() == '"' || text.front() == '\'')
					{
						const size_t end = text.find(text.front(), 1);
						if (end == std::string_view::npos) return Error(L"MTL 경로의 따옴표가 닫히지 않았습니다.");
						name = text.substr(1, end - 1); text.remove_prefix(end + 1);
					}
					else name = NextToken(text);
					const fs::path file = FindFile(modelPath.parent_path(), name);
					if (file.empty()) return Error(L"참조한 MTL 파일을 찾지 못했습니다: " + FromText(name));
					if (!ReadMaterialFile(file)) return false;
					text = Trim(text);
				}
				return true;
			}

			bool ReadMap(std::string_view text, MapReference& map, const fs::path& file, size_t line)
			{
				map = MapReference{}; map.file = file; map.line = line;
				while (!Trim(text).empty() && Trim(text).front() == '-')
				{
					const std::string_view option = NextToken(text);
					if (option == "-s" || option == "-o")
					{
						float values[3]{ option == "-s" ? 1.0f : 0.0f, option == "-s" ? 1.0f : 0.0f, option == "-s" ? 1.0f : 0.0f };
						size_t count = 0;
						while (count < 3)
						{
							std::string_view remaining = text;
							const std::string_view token = NextToken(remaining);
							float value = 0;
							if (!ParseFloat(token, value)) break;
							values[count++] = value; text = remaining;
						}
						if (!count || values[2] != (option == "-s" ? 1.0f : 0.0f))
							return Fail(error, file, line, L"텍스처 변환은 2D UV만 지원합니다.");
						if (option == "-s") map.scale = { values[0], values[1] };
						else map.offset = { values[0], values[1] };
					}
					else if (option == "-bm")
					{
						float amount = 0;
						if (!ParseFloat(NextToken(text), amount) || amount != 1.0f)
							return Fail(error, file, line, L"노멀 맵 배율은 1만 지원합니다. 높이 맵은 노멀 맵으로 변환하세요.");
					}
					else if (option == "-clamp" || option == "-blendu" || option == "-blendv" || option == "-cc" || option == "-imfchan")
					{
						const std::string_view value = NextToken(text);
						const std::string_view expected = option == "-imfchan" ? "r" :
							option == "-blendu" || option == "-blendv" ? "on" : "off";
						if (value != expected) return Fail(error, file, line, L"현재 재질에서 지원하지 않는 텍스처 옵션입니다: " + FromText(option));
					}
					else return Fail(error, file, line, L"지원하지 않는 텍스처 옵션입니다: " + FromText(option));
				}
				map.name = std::string(Unquote(text));
				if (map.name.empty()) return Fail(error, file, line, L"텍스처 이름이 비어 있습니다.");
				return true;
			}

			bool ReadMaterialFile(const fs::path& path)
			{
				if (loadedLibraries.find(path.wstring()) != loadedLibraries.end()) return true;
				std::ifstream stream(path, std::ios::binary);
				if (!stream) return Fail(error, path, 0, L"MTL 파일을 열 수 없습니다.");
				std::string line;
				size_t number = 0;
				uint32_t index = kInvalidModelIndex;
				while (std::getline(stream, line))
				{
					++number;
					if (!JoinContinuation(stream, line, path, number)) return false;
					std::string_view text(line);
					if (number == 1 && text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
					text = Trim(WithoutComment(text));
					const std::string_view type = NextToken(text);
					if (type.empty()) continue;
					if (type == "newmtl")
					{
						const std::string name(Unquote(text));
						if (name.empty()) return Fail(error, path, number, L"재질 이름이 비어 있습니다.");
						if (!GetMaterial(name, index)) return false;
						MaterialData value; value.name = name; data.materials[index] = std::move(value);
						const bool used = records[index].used;
						records[index] = MaterialRecord{}; records[index].defined = true; records[index].used = used;
						records[index].file = path; records[index].line = number;
						continue;
					}
					if (index == kInvalidModelIndex) return Fail(error, path, number, L"재질 값 앞에 newmtl이 필요합니다.");
					MaterialData& value = data.materials[index];
					MaterialRecord& record = records[index];
					if (type == "Kd" || type == "Ke")
					{
						XMFLOAT3 color{};
						if (!ParseFloat(NextToken(text), color.x) || !ParseFloat(NextToken(text), color.y) ||
							!ParseFloat(NextToken(text), color.z) || !NextToken(text).empty())
							return Fail(error, path, number, L"재질 색상은 유효한 RGB 값이어야 합니다.");
						if (type == "Kd") { value.baseColor.x = color.x; value.baseColor.y = color.y; value.baseColor.z = color.z; }
						else value.emissive = color;
					}
					else if (type == "Ns" || type == "Pr" || type == "Pm" || type == "d" || type == "Tr")
					{
						std::string_view token = NextToken(text);
						if (type == "d" && token == "-halo") return Fail(error, path, number, L"halo 투명도는 지원하지 않습니다.");
						float scalar = 0;
						if (!ParseFloat(token, scalar) || !NextToken(text).empty()) return Fail(error, path, number, L"재질 수치가 올바르지 않습니다.");
						if (type == "Ns" && !record.hasRoughness) value.roughness = std::clamp(float(std::sqrt(2.0 / (std::max(0.0, double(scalar)) + 2.0))), 0.04f, 1.0f);
						else if (type == "Pr") { value.roughness = std::clamp(scalar, 0.04f, 1.0f); record.hasRoughness = true; }
						else if (type == "Pm") value.metallic = std::clamp(scalar, 0.0f, 1.0f);
						else if (type == "d" || type == "Tr") value.baseColor.w = std::clamp(type == "Tr" ? 1.0f - scalar : scalar, 0.0f, 1.0f);
					}
					else if (type == "map_Kd" || type == "map_Ke" || type == "map_Pr" || type == "map_Pm" ||
						type == "norm" || type == "map_Kn" || type == "map_normal" || type == "map_Bump" || type == "map_bump" || type == "bump" || type == "map_Ns")
					{
						const TextureSlot slot = type == "map_Kd" ? TextureSlot::BaseColor : type == "map_Ke" ? TextureSlot::Emissive :
							type == "map_Pr" || type == "map_Ns" ? TextureSlot::Roughness : type == "map_Pm" ? TextureSlot::Metallic : TextureSlot::Normal;
						MapReference map;
						if (!ReadMap(text, map, path, number)) return false;
						const size_t slash = map.name.find_last_of("/\\");
						std::string filename = slash == std::string::npos ? map.name : map.name.substr(slash + 1);
						std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
						if (type == "map_Ns" && filename.find("roughness") == std::string::npos)
							return Fail(error, path, number, L"광택 맵은 지원하지 않습니다. 거칠기 맵(map_Pr)으로 내보내세요.");
						if ((type == "map_Bump" || type == "map_bump" || type == "bump") && filename.find("normal") == std::string::npos)
							return Fail(error, path, number, L"높이 맵은 지원하지 않습니다. 노멀 맵(norm)으로 내보내세요.");
						record.maps[size_t(slot)] = std::move(map);
					}
					else if (type.substr(0, 4) == "map_" || type == "disp" || type == "decal" || type == "refl")
						return Fail(error, path, number, L"현재 렌더러에서 지원하지 않는 재질 맵입니다: " + FromText(type));
					// Ka/Ks/Ni/illum은 현재 PBR 셰이더에 대응하는 별도 슬롯이 없다.
				}
				if (stream.bad()) return Fail(error, path, number, L"MTL 파일을 읽는 도중 오류가 발생했습니다.");
				loadedLibraries.emplace(path.wstring(), true);
				return true;
			}

			bool FinishMaterials()
			{
				for (size_t i = 0; i < records.size(); ++i)
				{
					MaterialRecord& record = records[i];
					MaterialData& materialData = data.materials[i];
					if (!record.used) continue;
					if (!record.defined) return Fail(error, record.file, record.line, L"MTL에 정의되지 않은 재질입니다: " + FromText(materialData.name));
					for (size_t slot = 0; slot < record.maps.size(); ++slot)
					{
						const MapReference& map = record.maps[slot];
						if (map.name.empty()) continue;
						if (record.textured && (record.scale.x != map.scale.x || record.scale.y != map.scale.y ||
							record.offset.x != map.offset.x || record.offset.y != map.offset.y))
							return Fail(error, map.file, map.line, L"한 재질의 모든 텍스처는 동일한 UV 변환을 사용해야 합니다.");
						record.textured = true; record.scale = map.scale; record.offset = map.offset;
						if (record.scale.x == 0 || record.scale.y == 0) return Fail(error, map.file, map.line, L"텍스처 UV 배율이 0입니다.");
						const fs::path texturePath = FindFile(map.file.parent_path(), map.name);
						if (texturePath.empty()) return Fail(error, map.file, map.line, L"참조한 텍스처를 찾지 못했습니다: " + FromText(map.name));
						const bool srgb = slot == size_t(TextureSlot::BaseColor) || slot == size_t(TextureSlot::Emissive);
						const bool normal = slot == size_t(TextureSlot::Normal);
						const std::wstring key = texturePath.wstring() + (srgb ? L"|srgb" : normal ? L"|normal" : L"|linear");
						if (auto found = textureIndices.find(key); found != textureIndices.end()) materialData.textures[slot] = found->second;
						else
						{
							TextureData image;
							std::wstring textureError;
							if (!LoadTextureImage(texturePath.c_str(), srgb, image, textureError)) return Fail(error, map.file, map.line, textureError);
							// OBJ의 +Y 노멀 맵은 V 반전으로 만든 접선 공간에 맞춰 G도 반전한다.
							if (normal) for (size_t channel = 1; channel < image.pixels.size(); channel += 4) image.pixels[channel] = uint8_t(255 - image.pixels[channel]);
							if (data.textures.size() >= kInvalidModelIndex) return Fail(error, map.file, map.line, L"텍스처 수가 인덱스 범위를 넘었습니다.");
							const uint32_t texture = uint32_t(data.textures.size());
							data.textures.push_back(std::move(image)); textureIndices.emplace(key, texture); materialData.textures[slot] = texture;
						}
						if (normal) record.normalMapped = true;
					}
					if (materialData.textures[size_t(TextureSlot::Roughness)] != kInvalidModelIndex) materialData.roughness = 1.0f;
					if (materialData.textures[size_t(TextureSlot::Metallic)] != kInvalidModelIndex) materialData.metallic = 1.0f;
					if (materialData.textures[size_t(TextureSlot::Emissive)] != kInvalidModelIndex &&
						materialData.emissive.x == 0 && materialData.emissive.y == 0 && materialData.emissive.z == 0)
						materialData.emissive = { 1, 1, 1 };
				}
				return true;
			}

			bool FinishMesh(ModelMeshData& modelMesh)
			{
				const size_t meshIndex = size_t(&modelMesh - data.meshes.data());
				const MaterialRecord& record = records[modelMesh.material];
				if (record.textured && meshMissingUvs[meshIndex])
					return Fail(error, record.file, record.line, L"텍스처 재질을 사용하는 면에 UV가 없습니다: " + FromText(data.materials[modelMesh.material].name));
				MeshData& mesh = modelMesh.mesh;
				for (Vertex& vertex : mesh.vertices)
				{
					vertex.uv = { vertex.uv.x * record.scale.x + record.offset.x, 1.0f - (vertex.uv.y * record.scale.y + record.offset.y) };
					if (!std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y)) return Error(L"텍스처 변환 후 UV가 유효하지 않습니다.");
				}
				std::vector<Vector> tangents, bitangents;
				if (record.normalMapped)
				{
					tangents.resize(mesh.vertices.size()); bitangents.resize(mesh.vertices.size());
					for (size_t i = 0; i < mesh.indices.size(); i += 3)
					{
						const Vertex& a = mesh.vertices[mesh.indices[i]]; const Vertex& b = mesh.vertices[mesh.indices[i + 1]]; const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
						const Vector one = ToVector(b.position) - ToVector(a.position), two = ToVector(c.position) - ToVector(a.position);
						const double u1 = double(b.uv.x) - a.uv.x, v1 = double(b.uv.y) - a.uv.y;
						const double u2 = double(c.uv.x) - a.uv.x, v2 = double(c.uv.y) - a.uv.y;
						const double determinant = u1 * v2 - u2 * v1;
						if (std::fabs(determinant) <= 1.0e-12) continue;
						const double area = std::sqrt(Dot(Cross(one, two), Cross(one, two)));
						const Vector tangent = (one * v2 - two * v1) * (area / determinant);
						const Vector bitangent = (two * u1 - one * u2) * (area / determinant);
						for (size_t corner = 0; corner < 3; ++corner) { tangents[mesh.indices[i + corner]] += tangent; bitangents[mesh.indices[i + corner]] += bitangent; }
					}
				}
				for (size_t i = 0; i < mesh.vertices.size(); ++i)
				{
					Vertex& vertex = mesh.vertices[i];
					const Vector normal = ToVector(vertex.normal);
					Vector tangent;
					if (record.normalMapped) tangent = tangents[i] - normal * Dot(normal, tangents[i]);
					XMFLOAT3 normalized{};
					if (!Normalize(tangent, normalized))
					{
						const Vector axis = std::fabs(normal.y) < 0.9 ? Vector{ 0, 1, 0 } : Vector{ 1, 0, 0 };
						if (!Normalize(Cross(axis, normal), normalized)) return Error(L"정점 접선을 만들 수 없습니다.");
					}
					const float sign = record.normalMapped && Dot(Cross(normal, ToVector(normalized)), bitangents[i]) < 0 ? -1.0f : 1.0f;
					vertex.tangent = { normalized.x, normalized.y, normalized.z, sign };
				}
				return true;
			}

			fs::path modelPath;
			ModelData& data;
			std::wstring& error;
			size_t lineNumber = 0;
			uint64_t faceNumber = 0;
			uint32_t objectNumber = 0;
			uint32_t objectNode = 0, activeNode = 0;
			uint32_t smoothingGroup = 0;
			uint32_t material = kInvalidModelIndex;
			std::string objectName, groupName;
			bool isRuinsPlanet = false;
			bool hasBounds = false, missingUvs = false;
			std::vector<XMFLOAT3> positions, normals, colors;
			std::vector<XMFLOAT2> uvs;
			ModelMeshData building;
			std::vector<bool> meshMissingUvs;
			std::unordered_map<VertexKey, uint32_t, VertexHash> vertexIndices;
			std::unordered_map<NormalKey, uint32_t, NormalHash> normalIndices;
			std::vector<NormalSum> normalSums;
			std::vector<NormalReference> normalReferences;
			std::vector<MaterialRecord> records;
			std::unordered_map<std::string, uint32_t> materialIndices;
			std::unordered_map<std::wstring, uint32_t> textureIndices;
			std::unordered_map<std::wstring, bool> loadedLibraries;
		};

	} // namespace

	bool ObjModelLoader::Load(const wchar_t* path, ModelData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path) { error = L"모델 경로가 비어 있습니다."; return false; }
		try
		{
			const fs::path modelPath(path);
			std::wstring extension = modelPath.extension().wstring();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value) { return wchar_t(std::towlower(value)); });
			if (extension != L".obj") return Fail(error, modelPath, 0, L"OBJ 모델만 지원합니다. 원본을 OBJ로 내보내세요.");
			ModelData loaded;
			ImportContext context(modelPath, loaded, error);
			if (!context.Read()) return false;
			out = std::move(loaded);
			return true;
		}
		catch (const std::bad_alloc&) { error = L"OBJ 모델을 읽을 메모리가 부족합니다. 모델의 정점/면 수를 줄여주세요."; }
		catch (const fs::filesystem_error&) { error = L"OBJ/MTL 파일 경로를 처리할 수 없습니다."; }
		catch (const std::length_error&) { error = L"OBJ 모델 데이터가 지원 범위를 넘었습니다."; }
		return false;
	}
}
