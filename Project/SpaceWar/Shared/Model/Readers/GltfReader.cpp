#include "GltfReader.h"
#include "JsonValue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ============================================================
//  좌표계 — 변환하지 않는다 (2026-10-07 결정)
//
//  glTF 는 오른손·Y-up·전방 −Z·미터다. 우리 월드는 **왼손**(Camera 가 XMMatrixLookAtLH)·Y-up·미터다.
//  좌표를 그대로 올리면 Z 축이 거울처럼 뒤집힌 것과 같아지는데, 그 결과
//    · glTF 의 전방 −Z 가 우리 전방 +Z 가 된다 → 엔진 전방(+Z)·스폰 방향 {0,0,1}·Model.cpp 의
//      kModelYaw = 0 과 그대로 맞는다. FBX 경로가 kModelYaw 0 으로 멀쩡히 보였던 이유도 같다.
//    · 삼각형 와인딩은 glTF 의 CCW(오른손)가 왼손 해석에서 CW 로 보여 D3D 기본 전면과 맞는다.
//  좌표를 변환하려면 위치·법선·탄젠트·노드 변환·역바인드 행렬·와인딩을 **전부** 한 번에 뒤집어야 하고
//  한 군데만 빠뜨려도 모델이 안쪽에서 보이거나 스키닝이 어긋난다. 그래서 변환을 넣지 않는다.
//  스킨도 역바인드·조인트 변환이 모두 같은(변환 안 된) 공간이라 자기 일관성이 유지된다.
//  ※ 블렌더에서 내보낼 때는 glTF 기본 축(+Y Up / −Z Forward)을 그대로 쓴다.
// ============================================================

namespace Shared {

	namespace {

		namespace fs = std::filesystem;

		inline constexpr size_t   kMaxFileBytes = 512ull * 1024ull * 1024ull;
		inline constexpr uint32_t kGlbMagic = 0x46546C67u;   // 'g','l','T','F'
		inline constexpr uint32_t kGlbChunkJson = 0x4E4F534Au;   // 'J','S','O','N'
		inline constexpr uint32_t kGlbChunkBin = 0x004E4942u;   // 'B','I','N',0

		// ── 파일·문자열 ─────────────────────────────────
		bool ReadWholeFile(const fs::path& path, std::vector<uint8_t>& out, std::wstring& error)
		{
			FILE* file = nullptr;
			if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
			{
				error = L"파일을 열 수 없습니다: " + path.wstring();
				return false;
			}
			if (::fseek(file, 0, SEEK_END) != 0)
			{
				::fclose(file);
				error = L"파일 크기를 알 수 없습니다: " + path.wstring();
				return false;
			}
			const long long size = ::_ftelli64(file);
			::rewind(file);
			if (size < 0 || static_cast<unsigned long long>(size) > kMaxFileBytes)
			{
				::fclose(file);
				error = L"파일이 너무 큽니다(512MB 제한): " + path.wstring();
				return false;
			}
			out.resize(static_cast<size_t>(size));
			const size_t read = out.empty() ? 0 : ::fread(out.data(), 1, out.size(), file);
			::fclose(file);
			if (read != out.size())
			{
				error = L"파일을 끝까지 읽지 못했습니다: " + path.wstring();
				return false;
			}
			return true;
		}

		fs::path Utf8Path(const std::string& text)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		}

		std::wstring ToWide(const std::string& utf8)
		{
			return Utf8Path(utf8).wstring();
		}

		// URI 의 %XX 를 되돌린다. 블렌더가 공백·한글을 퍼센트로 적어 내보낼 수 있다.
		std::string PercentDecode(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '%' && i + 2 < text.size())
				{
					const auto digit = [](char c) -> int {
						if (c >= '0' && c <= '9') return c - '0';
						if (c >= 'a' && c <= 'f') return c - 'a' + 10;
						if (c >= 'A' && c <= 'F') return c - 'A' + 10;
						return -1;
					};
					const int hi = digit(text[i + 1]);
					const int lo = digit(text[i + 2]);
					if (hi >= 0 && lo >= 0)
					{
						out.push_back(static_cast<char>(hi * 16 + lo));
						i += 2;
						continue;
					}
				}
				out.push_back(text[i]);
			}
			return out;
		}

		bool DecodeBase64(const char* text, size_t length, std::vector<uint8_t>& out)
		{
			const auto value = [](char c) -> int {
				if (c >= 'A' && c <= 'Z') return c - 'A';
				if (c >= 'a' && c <= 'z') return c - 'a' + 26;
				if (c >= '0' && c <= '9') return c - '0' + 52;
				if (c == '+') return 62;
				if (c == '/') return 63;
				return -1;
			};

			out.clear();
			out.reserve(length / 4 * 3 + 3);

			uint32_t group = 0;
			int collected = 0;
			for (size_t i = 0; i < length; ++i)
			{
				const char c = text[i];
				if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
				const int v = value(c);
				if (v < 0) return false;
				group = (group << 6) | static_cast<uint32_t>(v);
				if (++collected == 4)
				{
					out.push_back(static_cast<uint8_t>((group >> 16) & 0xFF));
					out.push_back(static_cast<uint8_t>((group >> 8) & 0xFF));
					out.push_back(static_cast<uint8_t>(group & 0xFF));
					group = 0;
					collected = 0;
				}
			}
			if (collected == 3)
			{
				group <<= 6;
				out.push_back(static_cast<uint8_t>((group >> 16) & 0xFF));
				out.push_back(static_cast<uint8_t>((group >> 8) & 0xFF));
			}
			else if (collected == 2)
			{
				group <<= 12;
				out.push_back(static_cast<uint8_t>((group >> 16) & 0xFF));
			}
			else if (collected == 1)
			{
				return false;   // base64 는 6비트 한 조각만 남을 수 없다
			}
			return true;
		}

		// "data:image/png;base64,...." 를 가른다.
		bool DecodeDataUri(const std::string& uri, std::vector<uint8_t>& out, std::string& mime)
		{
			if (uri.rfind("data:", 0) != 0) return false;
			const size_t comma = uri.find(',');
			if (comma == std::string::npos) return false;

			const std::string header = uri.substr(5, comma - 5);
			const size_t semicolon = header.find(';');
			mime = semicolon == std::string::npos ? header : header.substr(0, semicolon);

			if (header.find("base64") == std::string::npos)
				return false;   // 평문 data URI 는 쓰지 않는다
			return DecodeBase64(uri.data() + comma + 1, uri.size() - comma - 1, out);
		}

		// ── 행렬 ────────────────────────────────────────
		//  Mat4 는 행 우선 + 행 벡터(p' = p·M)다. glTF 는 열 우선 + 열 벡터(p' = M·p)인데
		//  ★ 둘은 메모리 배치가 같다(한쪽의 전치가 다른 쪽이다). 그래서 16개를 그대로 복사한다.
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

		// 쿼터니언(x,y,z,w) → 행 벡터 규약 회전 행렬 (DirectXMath 와 같은 배치)
		Mat4 FromQuaternion(float x, float y, float z, float w)
		{
			const float length = std::sqrt(x * x + y * y + z * z + w * w);
			if (length > 1.0e-8f)
			{
				const float inv = 1.0f / length;
				x *= inv; y *= inv; z *= inv; w *= inv;
			}
			else
			{
				x = y = z = 0.0f; w = 1.0f;
			}

			Mat4 r;
			r.m[0] = 1.0f - 2.0f * (y * y + z * z);
			r.m[1] = 2.0f * (x * y + w * z);
			r.m[2] = 2.0f * (x * z - w * y);
			r.m[3] = 0.0f;
			r.m[4] = 2.0f * (x * y - w * z);
			r.m[5] = 1.0f - 2.0f * (x * x + z * z);
			r.m[6] = 2.0f * (y * z + w * x);
			r.m[7] = 0.0f;
			r.m[8] = 2.0f * (x * z + w * y);
			r.m[9] = 2.0f * (y * z - w * x);
			r.m[10] = 1.0f - 2.0f * (x * x + y * y);
			r.m[11] = 0.0f;
			r.m[12] = r.m[13] = r.m[14] = 0.0f;
			r.m[15] = 1.0f;
			return r;
		}

		// 노드의 로컬 변환. matrix 가 있으면 그것을, 없으면 T·R·S 를 합친다(glTF: M = T·R·S).
		Mat4 NodeLocal(const JsonValue& node)
		{
			const JsonValue& matrix = node["matrix"];
			if (matrix.IsArray() && matrix.Size() == 16)
			{
				Mat4 m;
				for (size_t i = 0; i < 16; ++i)
					m.m[i] = static_cast<float>(matrix[i].AsNumber());
				return m;
			}

			Mat4 result;   // 단위행렬
			const JsonValue& scale = node["scale"];
			if (scale.IsArray() && scale.Size() == 3)
			{
				Mat4 s;
				s.m[0] = static_cast<float>(scale.At(0).AsNumber(1.0));
				s.m[5] = static_cast<float>(scale[1].AsNumber(1.0));
				s.m[10] = static_cast<float>(scale[2].AsNumber(1.0));
				result = s;
			}
			const JsonValue& rotation = node["rotation"];
			if (rotation.IsArray() && rotation.Size() == 4)
			{
				const Mat4 r = FromQuaternion(
					static_cast<float>(rotation.At(0).AsNumber()),
					static_cast<float>(rotation[1].AsNumber()),
					static_cast<float>(rotation[2].AsNumber()),
					static_cast<float>(rotation[3].AsNumber(1.0)));
				result = Multiply(result, r);   // 행 벡터이므로 S 다음 R
			}
			const JsonValue& translation = node["translation"];
			if (translation.IsArray() && translation.Size() == 3)
			{
				Mat4 t;
				t.m[12] = static_cast<float>(translation.At(0).AsNumber());
				t.m[13] = static_cast<float>(translation[1].AsNumber());
				t.m[14] = static_cast<float>(translation[2].AsNumber());
				result = Multiply(result, t);
			}
			return result;
		}

		// ── glTF 문서 ───────────────────────────────────
		struct Gltf
		{
			JsonValue json;
			std::vector<std::vector<uint8_t>> buffers;   // 해석 완료된 바이트
		};

		struct AccessorView
		{
			const uint8_t* base = nullptr;   // 첫 원소의 시작
			size_t stride = 0;               // 원소 간 바이트 간격
			size_t count = 0;
			size_t components = 0;           // SCALAR 1 … MAT4 16
			int    componentType = 0;
			bool   normalized = false;
		};

		size_t ComponentSize(int componentType)
		{
			switch (componentType)
			{
			case 5120: case 5121: return 1;   // BYTE / UNSIGNED_BYTE
			case 5122: case 5123: return 2;   // SHORT / UNSIGNED_SHORT
			case 5125: case 5126: return 4;   // UNSIGNED_INT / FLOAT
			default: return 0;
			}
		}

		size_t ComponentCount(const std::string& type)
		{
			if (type == "SCALAR") return 1;
			if (type == "VEC2")   return 2;
			if (type == "VEC3")   return 3;
			if (type == "VEC4")   return 4;
			if (type == "MAT4")   return 16;
			return 0;   // MAT2·MAT3 는 쓰지 않는다
		}

		// 접근자 하나를 가리키는 뷰를 만든다. 범위 검사를 여기서 모두 끝낸다.
		bool OpenAccessor(const Gltf& gltf, uint32_t index, AccessorView& view, std::wstring& error)
		{
			const JsonValue& accessors = gltf.json["accessors"];
			if (index >= accessors.Size())
			{
				error = L"glTF 접근자 번호가 범위를 벗어났습니다.";
				return false;
			}
			const JsonValue& accessor = accessors[index];

			if (accessor.Has("sparse"))
			{
				error = L"glTF 희소 접근자(sparse)는 지원하지 않습니다.";
				return false;
			}

			view.componentType = accessor["componentType"].AsInt();
			view.components = ComponentCount(accessor["type"].AsString());
			view.count = accessor["count"].AsUint();
			view.normalized = accessor["normalized"].AsBool();

			const size_t componentSize = ComponentSize(view.componentType);
			if (componentSize == 0 || view.components == 0)
			{
				error = L"glTF 접근자의 자료형을 모릅니다.";
				return false;
			}
			if (view.count == 0)
			{
				error = L"glTF 접근자가 비어 있습니다.";
				return false;
			}

			if (!accessor.Has("bufferView"))
			{
				error = L"glTF 접근자에 bufferView 가 없습니다(0으로 채워진 접근자는 지원하지 않습니다).";
				return false;
			}

			const uint32_t viewIndex = accessor["bufferView"].AsUint();
			const JsonValue& views = gltf.json["bufferViews"];
			if (viewIndex >= views.Size())
			{
				error = L"glTF bufferView 번호가 범위를 벗어났습니다.";
				return false;
			}
			const JsonValue& bufferView = views[viewIndex];

			const uint32_t bufferIndex = bufferView["buffer"].AsUint();
			if (bufferIndex >= gltf.buffers.size())
			{
				error = L"glTF buffer 번호가 범위를 벗어났습니다.";
				return false;
			}
			const std::vector<uint8_t>& buffer = gltf.buffers[bufferIndex];

			const size_t viewOffset = bufferView["byteOffset"].AsUint(0);
			const size_t viewLength = bufferView["byteLength"].AsUint(0);
			const size_t byteStride = bufferView["byteStride"].AsUint(0);
			const size_t accessorOffset = accessor["byteOffset"].AsUint(0);

			const size_t packed = view.components * componentSize;
			view.stride = byteStride ? byteStride : packed;
			if (view.stride < packed)
			{
				error = L"glTF bufferView 의 byteStride 가 원소 크기보다 작습니다.";
				return false;
			}

			// 범위 검사: view 가 buffer 안에, 접근자가 view 안에 들어와야 한다.
			if (viewOffset > buffer.size() || viewLength > buffer.size() - viewOffset)
			{
				error = L"glTF bufferView 가 buffer 범위를 벗어났습니다.";
				return false;
			}
			const size_t need = accessorOffset + (view.count - 1) * view.stride + packed;
			if (need > viewLength)
			{
				error = L"glTF 접근자가 bufferView 범위를 벗어났습니다.";
				return false;
			}

			view.base = buffer.data() + viewOffset + accessorOffset;
			return true;
		}

		float ReadComponentAsFloat(const uint8_t* p, int componentType, bool normalized)
		{
			switch (componentType)
			{
			case 5126:
			{
				float v = 0.0f;
				std::memcpy(&v, p, sizeof(v));
				return v;
			}
			case 5121:
			{
				const uint8_t v = *p;
				return normalized ? float(v) / 255.0f : float(v);
			}
			case 5123:
			{
				uint16_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return normalized ? float(v) / 65535.0f : float(v);
			}
			case 5120:
			{
				const int8_t v = static_cast<int8_t>(*p);
				return normalized ? (std::max)(float(v) / 127.0f, -1.0f) : float(v);
			}
			case 5122:
			{
				int16_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return normalized ? (std::max)(float(v) / 32767.0f, -1.0f) : float(v);
			}
			case 5125:
			{
				uint32_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return float(v);
			}
			default: return 0.0f;
			}
		}

		uint32_t ReadComponentAsUint(const uint8_t* p, int componentType)
		{
			switch (componentType)
			{
			case 5121: return *p;
			case 5123:
			{
				uint16_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return v;
			}
			case 5125:
			{
				uint32_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return v;
			}
			case 5120: return static_cast<uint32_t>((std::max)(int(static_cast<int8_t>(*p)), 0));
			case 5122:
			{
				int16_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				return static_cast<uint32_t>((std::max)(int(v), 0));
			}
			default: return 0;
			}
		}

		// 접근자를 float 배열로 펼친다. 결과는 count × components 개다.
		bool ReadFloats(const Gltf& gltf, uint32_t index, std::vector<float>& out,
			size_t& components, size_t& count, std::wstring& error)
		{
			AccessorView view;
			if (!OpenAccessor(gltf, index, view, error)) return false;

			const size_t componentSize = ComponentSize(view.componentType);
			components = view.components;
			count = view.count;

			out.resize(view.count * view.components);
			for (size_t i = 0; i < view.count; ++i)
			{
				const uint8_t* element = view.base + i * view.stride;
				for (size_t c = 0; c < view.components; ++c)
					out[i * view.components + c] =
						ReadComponentAsFloat(element + c * componentSize, view.componentType, view.normalized);
			}
			return true;
		}

		bool ReadUints(const Gltf& gltf, uint32_t index, std::vector<uint32_t>& out,
			size_t& components, size_t& count, std::wstring& error)
		{
			AccessorView view;
			if (!OpenAccessor(gltf, index, view, error)) return false;

			const size_t componentSize = ComponentSize(view.componentType);
			components = view.components;
			count = view.count;

			out.resize(view.count * view.components);
			for (size_t i = 0; i < view.count; ++i)
			{
				const uint8_t* element = view.base + i * view.stride;
				for (size_t c = 0; c < view.components; ++c)
					out[i * view.components + c] =
						ReadComponentAsUint(element + c * componentSize, view.componentType);
			}
			return true;
		}

		// ── 버퍼 해석 ───────────────────────────────────
		bool ResolveBuffers(Gltf& gltf, const fs::path& modelDirectory,
			std::vector<uint8_t>&& glbBin, std::wstring& error)
		{
			const JsonValue& buffers = gltf.json["buffers"];
			gltf.buffers.resize(buffers.Size());

			for (size_t i = 0; i < buffers.Size(); ++i)
			{
				const JsonValue& buffer = buffers[i];
				const std::string uri = buffer["uri"].AsString();

				if (uri.empty())
				{
					// uri 없는 버퍼 = .glb 의 BIN 청크. 규격상 0번 하나뿐이다.
					if (i != 0 || glbBin.empty())
					{
						error = L"glTF buffer 에 uri 가 없는데 .glb 의 BIN 청크가 아닙니다.";
						return false;
					}
					gltf.buffers[i] = std::move(glbBin);
				}
				else if (uri.rfind("data:", 0) == 0)
				{
					std::string mime;
					if (!DecodeDataUri(uri, gltf.buffers[i], mime))
					{
						error = L"glTF buffer 의 data URI 를 해석하지 못했습니다.";
						return false;
					}
				}
				else
				{
					const fs::path path = modelDirectory / Utf8Path(PercentDecode(uri));
					if (!ReadWholeFile(path, gltf.buffers[i], error)) return false;
				}

				const size_t declared = buffer["byteLength"].AsUint(0);
				if (declared != 0 && gltf.buffers[i].size() < declared)
				{
					error = L"glTF buffer 가 byteLength 보다 짧습니다.";
					return false;
				}
			}
			return true;
		}

		// ── 노드 계층 펼치기 ────────────────────────────
		//  부모가 반드시 자기보다 앞에 오도록 깊이 우선으로 훑는다(ValidateModelSource 가 검사한다).
		struct NodeOrder
		{
			std::vector<uint32_t> gltfIndex;    // 우리 순서 → glTF 노드 번호
			std::vector<uint32_t> ourIndex;     // glTF 노드 번호 → 우리 순서 (없으면 kInvalidIndex)
			std::vector<uint32_t> parent;       // 우리 순서 기준 부모
		};

		bool FlattenNodes(const Gltf& gltf, NodeOrder& order, std::wstring& error)
		{
			const JsonValue& nodes = gltf.json["nodes"];
			const size_t nodeCount = nodes.Size();
			if (nodeCount > kMaxNodes)
			{
				error = L"glTF 노드가 너무 많습니다.";
				return false;
			}
			order.ourIndex.assign(nodeCount, kInvalidIndex);
			if (nodeCount == 0) return true;

			// 루트 찾기: scene 이 있으면 그 목록, 없으면 «부모가 없는 노드» 전부.
			std::vector<uint32_t> roots;
			const JsonValue& scenes = gltf.json["scenes"];
			const uint32_t sceneIndex = gltf.json["scene"].AsUint(0);
			if (sceneIndex < scenes.Size())
			{
				const JsonValue& sceneNodes = scenes[sceneIndex]["nodes"];
				for (size_t i = 0; i < sceneNodes.Size(); ++i)
					roots.push_back(sceneNodes[i].AsUint());
			}
			if (roots.empty())
			{
				std::vector<bool> isChild(nodeCount, false);
				for (size_t i = 0; i < nodeCount; ++i)
				{
					const JsonValue& children = nodes[i]["children"];
					for (size_t c = 0; c < children.Size(); ++c)
					{
						const uint32_t child = children[c].AsUint();
						if (child < nodeCount) isChild[child] = true;
					}
				}
				for (size_t i = 0; i < nodeCount; ++i)
					if (!isChild[i]) roots.push_back(static_cast<uint32_t>(i));
			}

			// 명시적 스택으로 깊이 우선. 순환이 있어도 ourIndex 검사로 한 번만 방문한다.
			struct Pending { uint32_t node; uint32_t parent; };
			std::vector<Pending> stack;
			for (size_t i = roots.size(); i-- > 0; )
				stack.push_back({ roots[i], kInvalidIndex });

			while (!stack.empty())
			{
				const Pending item = stack.back();
				stack.pop_back();
				if (item.node >= nodeCount) continue;
				if (order.ourIndex[item.node] != kInvalidIndex) continue;   // 이미 넣었다(순환·중복 참조)

				const uint32_t ours = static_cast<uint32_t>(order.gltfIndex.size());
				order.ourIndex[item.node] = ours;
				order.gltfIndex.push_back(item.node);
				order.parent.push_back(item.parent);

				const JsonValue& children = nodes[item.node]["children"];
				for (size_t c = children.Size(); c-- > 0; )
					stack.push_back({ children[c].AsUint(), ours });
			}
			return true;
		}

		// ── 와인딩 ──────────────────────────────────────
		//  파일의 와인딩 규약을 믿지 않고 법선과 맞춘다(ObjReader·옛 FBX 경로와 같은 방법).
		void OrientTriangle(SourceMesh& mesh, size_t first)
		{
			const SourceVertex& a = mesh.vertices[mesh.indices[first]];
			const SourceVertex& b = mesh.vertices[mesh.indices[first + 1]];
			const SourceVertex& c = mesh.vertices[mesh.indices[first + 2]];

			const Vec3 edge1{ b.position.x - a.position.x, b.position.y - a.position.y, b.position.z - a.position.z };
			const Vec3 edge2{ c.position.x - a.position.x, c.position.y - a.position.y, c.position.z - a.position.z };
			const Vec3 faceNormal{
				edge1.y * edge2.z - edge1.z * edge2.y,
				edge1.z * edge2.x - edge1.x * edge2.z,
				edge1.x * edge2.y - edge1.y * edge2.x };
			const Vec3 shading{
				a.normal.x + b.normal.x + c.normal.x,
				a.normal.y + b.normal.y + c.normal.y,
				a.normal.z + b.normal.z + c.normal.z };

			const float agreement = faceNormal.x * shading.x + faceNormal.y * shading.y + faceNormal.z * shading.z;
			if (agreement < 0.0f) std::swap(mesh.indices[first + 1], mesh.indices[first + 2]);
		}

	} // namespace

	bool GltfReader::Matches(const uint8_t* head, size_t headSize, const char* ext) const
	{
		if (head && headSize >= 4 &&
			head[0] == 'g' && head[1] == 'l' && head[2] == 'T' && head[3] == 'F')
			return true;
		return ext && (std::strcmp(ext, ".glb") == 0 || std::strcmp(ext, ".gltf") == 0);
	}

	bool GltfReader::Read(const wchar_t* path, const ReadOptions& options,
		ModelSource& out, std::wstring& error)
	{
		const fs::path modelPath(path);
		const fs::path modelDirectory = modelPath.parent_path();

		std::vector<uint8_t> bytes;
		if (!ReadWholeFile(modelPath, bytes, error)) return false;
		if (bytes.size() < 4)
		{
			error = L"glTF 파일이 너무 짧습니다.";
			return false;
		}

		// ── 1) .glb 면 청크를 가른다 ────────────────────
		const char* jsonText = nullptr;
		size_t jsonLength = 0;
		std::vector<uint8_t> glbBin;

		uint32_t magic = 0;
		std::memcpy(&magic, bytes.data(), sizeof(magic));
		if (magic == kGlbMagic)
		{
			if (bytes.size() < 12)
			{
				error = L"glb 헤더가 짧습니다.";
				return false;
			}
			uint32_t version = 0, totalLength = 0;
			std::memcpy(&version, bytes.data() + 4, sizeof(version));
			std::memcpy(&totalLength, bytes.data() + 8, sizeof(totalLength));
			if (version != 2)
			{
				error = L"glb 버전 2 만 읽습니다 (파일 버전: " + std::to_wstring(version) + L").";
				return false;
			}
			if (totalLength > bytes.size()) totalLength = static_cast<uint32_t>(bytes.size());

			size_t cursor = 12;
			while (cursor + 8 <= totalLength)
			{
				uint32_t chunkLength = 0, chunkType = 0;
				std::memcpy(&chunkLength, bytes.data() + cursor, sizeof(chunkLength));
				std::memcpy(&chunkType, bytes.data() + cursor + 4, sizeof(chunkType));
				cursor += 8;
				if (chunkLength > totalLength - cursor)
				{
					error = L"glb 청크 길이가 파일 범위를 벗어났습니다.";
					return false;
				}
				if (chunkType == kGlbChunkJson && !jsonText)
				{
					jsonText = reinterpret_cast<const char*>(bytes.data() + cursor);
					jsonLength = chunkLength;
				}
				else if (chunkType == kGlbChunkBin && glbBin.empty())
				{
					glbBin.assign(bytes.data() + cursor, bytes.data() + cursor + chunkLength);
				}
				cursor += chunkLength;
				cursor = (cursor + 3) & ~size_t(3);   // 청크는 4바이트 정렬이다
			}
			if (!jsonText)
			{
				error = L"glb 에 JSON 청크가 없습니다.";
				return false;
			}
		}
		else
		{
			jsonText = reinterpret_cast<const char*>(bytes.data());
			jsonLength = bytes.size();
		}

		// ── 2) JSON 파싱 ───────────────────────────────
		Gltf gltf;
		std::string jsonError;
		if (!JsonValue::Parse(jsonText, jsonLength, gltf.json, jsonError))
		{
			error = L"glTF JSON 을 읽지 못했습니다: " + ToWide(jsonError);
			return false;
		}

		const std::string assetVersion = gltf.json["asset"]["version"].AsString();
		if (!assetVersion.empty() && assetVersion[0] != '2')
		{
			error = L"glTF 2.x 만 읽습니다 (파일: " + ToWide(assetVersion) + L").";
			return false;
		}

		if (!ResolveBuffers(gltf, modelDirectory, std::move(glbBin), error)) return false;

		// ── 3) 노드 계층 ───────────────────────────────
		NodeOrder order;
		if (!FlattenNodes(gltf, order, error)) return false;

		const JsonValue& nodes = gltf.json["nodes"];

		// ── 4) 스킨(본) ────────────────────────────────
		//  스킨은 파일당 하나만 읽는다. 캐릭터 한 명 = 파일 하나 규칙이고,
		//  맵·소품은 스킨이 없다. 둘 이상이면 첫 번째만 쓴다.
		std::unordered_map<uint32_t, uint32_t> jointOfNode;   // glTF 노드 번호 → 조인트 번호
		const bool wantAnimation = options.readAnimation && !options.geometryOnly;

		if (wantAnimation)
		{
			const JsonValue& skins = gltf.json["skins"];
			if (skins.Size() > 0)
			{
				const JsonValue& skin = skins.At(0);
				const JsonValue& joints = skin["joints"];
				if (joints.Size() > kMaxJoints)
				{
					error = L"glTF 스킨의 조인트가 너무 많습니다(" + std::to_wstring(kMaxJoints) + L" 제한).";
					return false;
				}

				out.skeleton.name = skin["name"].AsString();
				out.skeleton.joints.resize(joints.Size());

				for (size_t i = 0; i < joints.Size(); ++i)
					jointOfNode[joints[i].AsUint()] = static_cast<uint32_t>(i);

				// 역바인드 행렬 (없으면 단위행렬로 둔다 — 규격이 허용한다)
				std::vector<float> inverseBind;
				if (skin.Has("inverseBindMatrices"))
				{
					size_t components = 0, count = 0;
					if (!ReadFloats(gltf, skin["inverseBindMatrices"].AsUint(), inverseBind, components, count, error))
						return false;
					if (components != 16 || count < joints.Size())
					{
						error = L"glTF 역바인드 행렬의 개수가 조인트 수와 맞지 않습니다.";
						return false;
					}
				}

				for (size_t i = 0; i < joints.Size(); ++i)
				{
					const uint32_t nodeIndex = joints[i].AsUint();
					if (nodeIndex >= nodes.Size())
					{
						error = L"glTF 스킨이 없는 노드를 조인트로 가리킵니다.";
						return false;
					}
					SourceJoint& joint = out.skeleton.joints[i];
					joint.name = nodes[nodeIndex]["name"].AsString();
					joint.localRest = NodeLocal(nodes[nodeIndex]);
					if (!inverseBind.empty())
						std::memcpy(joint.inverseBind.m, inverseBind.data() + i * 16, sizeof(float) * 16);
				}

				// 부모 — 조인트 집합 안에서만 잇는다. 집합 밖(루트 노드)이면 kInvalidIndex 다.
				for (size_t i = 0; i < nodes.Size(); ++i)
				{
					const JsonValue& children = nodes[i]["children"];
					for (size_t c = 0; c < children.Size(); ++c)
					{
						const auto child = jointOfNode.find(children[c].AsUint());
						if (child == jointOfNode.end()) continue;
						const auto parent = jointOfNode.find(static_cast<uint32_t>(i));
						if (parent == jointOfNode.end()) continue;
						out.skeleton.joints[child->second].parent = parent->second;
					}
				}

				// ★ 부모 선행 규칙을 지키도록 다시 세운다.
				//   glTF 의 joints 배열 순서는 계층 순서라는 보장이 없다.
				{
					const size_t count = out.skeleton.joints.size();
					std::vector<uint32_t> newIndex(count, kInvalidIndex);
					std::vector<SourceJoint> sorted;
					sorted.reserve(count);

					bool progress = true;
					while (sorted.size() < count && progress)
					{
						progress = false;
						for (size_t i = 0; i < count; ++i)
						{
							if (newIndex[i] != kInvalidIndex) continue;
							const uint32_t parent = out.skeleton.joints[i].parent;
							if (parent != kInvalidIndex && newIndex[parent] == kInvalidIndex) continue;
							newIndex[i] = static_cast<uint32_t>(sorted.size());
							sorted.push_back(out.skeleton.joints[i]);
							progress = true;
						}
					}
					if (sorted.size() != count)
					{
						error = L"glTF 스켈레톤에 순환이 있습니다.";
						return false;
					}
					for (SourceJoint& joint : sorted)
						if (joint.parent != kInvalidIndex) joint.parent = newIndex[joint.parent];

					out.skeleton.joints = std::move(sorted);
					for (auto& pair : jointOfNode) pair.second = newIndex[pair.second];
				}
			}
		}

		// ── 5) 메시(프리미티브) ────────────────────────
		const JsonValue& meshes = gltf.json["meshes"];
		// glTF 메시 하나가 프리미티브 여러 개를 갖는다. 우리 쪽은 프리미티브 하나 = 메시 하나다.
		std::vector<std::vector<uint32_t>> primitivesOfMesh(meshes.Size());

		for (size_t m = 0; m < meshes.Size(); ++m)
		{
			const JsonValue& primitives = meshes[m]["primitives"];
			for (size_t p = 0; p < primitives.Size(); ++p)
			{
				const JsonValue& primitive = primitives[p];

				const int mode = primitive.Has("mode") ? primitive["mode"].AsInt() : 4;
				if (mode != 4) continue;   // 삼각형 목록만 읽는다(STRIP·FAN·LINES·POINTS 무시)

				const JsonValue& attributes = primitive["attributes"];
				if (!attributes.Has("POSITION")) continue;

				if (out.meshes.size() >= kMaxMeshes)
				{
					error = L"glTF 메시가 너무 많습니다.";
					return false;
				}

				std::vector<float> positions, normals, uvs, colors, tangents, weights;
				std::vector<uint32_t> joints;
				size_t components = 0, count = 0;

				if (!ReadFloats(gltf, attributes["POSITION"].AsUint(), positions, components, count, error))
					return false;
				if (components != 3)
				{
					error = L"glTF POSITION 이 VEC3 가 아닙니다.";
					return false;
				}
				const size_t vertexCount = count;
				if (vertexCount > kMaxVertices)
				{
					error = L"glTF 메시의 정점이 너무 많습니다.";
					return false;
				}

				const auto readAttribute = [&](const char* name, size_t expected,
					std::vector<float>& target) -> bool
				{
					if (!attributes.Has(name)) return true;
					size_t attributeComponents = 0, attributeCount = 0;
					if (!ReadFloats(gltf, attributes[name].AsUint(), target,
						attributeComponents, attributeCount, error))
						return false;
					if (attributeCount != vertexCount || attributeComponents != expected)
					{
						target.clear();   // 개수가 어긋나면 조용히 버린다(기본값을 쓴다)
					}
					return true;
				};

				if (!options.geometryOnly)
				{
					if (!readAttribute("NORMAL", 3, normals)) return false;
					if (!readAttribute("TEXCOORD_0", 2, uvs)) return false;
					if (!readAttribute("TANGENT", 4, tangents)) return false;
					// COLOR_0 는 VEC3 또는 VEC4 다.
					if (attributes.Has("COLOR_0"))
					{
						size_t colorComponents = 0, colorCount = 0;
						if (!ReadFloats(gltf, attributes["COLOR_0"].AsUint(), colors,
							colorComponents, colorCount, error))
							return false;
						if (colorCount != vertexCount || (colorComponents != 3 && colorComponents != 4))
							colors.clear();
						else if (colorComponents == 4)
						{
							// 알파를 버리고 RGB 만 쓴다(Vertex 에 정점 알파가 없다).
							std::vector<float> rgb(vertexCount * 3);
							for (size_t i = 0; i < vertexCount; ++i)
							{
								rgb[i * 3 + 0] = colors[i * 4 + 0];
								rgb[i * 3 + 1] = colors[i * 4 + 1];
								rgb[i * 3 + 2] = colors[i * 4 + 2];
							}
							colors = std::move(rgb);
						}
					}
				}
				else if (!readAttribute("NORMAL", 3, normals))
				{
					return false;   // 와인딩 보정에 쓰므로 법선은 서버도 읽는다
				}

				bool skinned = false;
				if (wantAnimation && !out.skeleton.Empty() &&
					attributes.Has("JOINTS_0") && attributes.Has("WEIGHTS_0"))
				{
					size_t jointComponents = 0, jointCount = 0;
					if (!ReadUints(gltf, attributes["JOINTS_0"].AsUint(), joints, jointComponents, jointCount, error))
						return false;
					size_t weightComponents = 0, weightCount = 0;
					if (!ReadFloats(gltf, attributes["WEIGHTS_0"].AsUint(), weights, weightComponents, weightCount, error))
						return false;
					skinned = jointComponents == 4 && weightComponents == 4 &&
						jointCount == vertexCount && weightCount == vertexCount;
					if (!skinned)
					{
						joints.clear();
						weights.clear();
					}
				}

				// 인덱스 — 없으면 0,1,2,… 순서다.
				std::vector<uint32_t> indices;
				if (primitive.Has("indices"))
				{
					size_t indexComponents = 0, indexCount = 0;
					if (!ReadUints(gltf, primitive["indices"].AsUint(), indices, indexComponents, indexCount, error))
						return false;
					if (indexComponents != 1)
					{
						error = L"glTF 인덱스 접근자가 SCALAR 가 아닙니다.";
						return false;
					}
				}
				else
				{
					indices.resize(vertexCount);
					for (size_t i = 0; i < vertexCount; ++i) indices[i] = static_cast<uint32_t>(i);
				}
				if (indices.size() % 3 != 0)
				{
					error = L"glTF 인덱스 수가 3의 배수가 아닙니다.";
					return false;
				}
				if (indices.size() > kMaxIndices)
				{
					error = L"glTF 인덱스가 너무 많습니다.";
					return false;
				}
				for (uint32_t index : indices)
				{
					if (index >= vertexCount)
					{
						error = L"glTF 인덱스가 정점 수를 넘었습니다.";
						return false;
					}
				}

				SourceMesh mesh;
				mesh.skinned = skinned;
				mesh.material = primitive.Has("material") && !options.geometryOnly
					? primitive["material"].AsUint() : kInvalidIndex;
				mesh.vertices.resize(vertexCount);
				mesh.indices = std::move(indices);

				for (size_t i = 0; i < vertexCount; ++i)
				{
					SourceVertex& vertex = mesh.vertices[i];
					vertex.position = { positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2] };
					if (!normals.empty())
						vertex.normal = { normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2] };
					if (!uvs.empty())
						vertex.uv = { uvs[i * 2], uvs[i * 2 + 1] };   // glTF UV 는 이미 위에서 아래다
					if (!colors.empty())
						vertex.color = { colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2] };
					if (!tangents.empty())
						vertex.tangent = { tangents[i * 4], tangents[i * 4 + 1],
							tangents[i * 4 + 2], tangents[i * 4 + 3] };

					if (skinned)
					{
						float sum = 0.0f;
						for (size_t slot = 0; slot < kJointsPerVertex; ++slot)
							sum += weights[i * 4 + slot];
						const float inv = sum > 1.0e-6f ? 1.0f / sum : 0.0f;
						for (size_t slot = 0; slot < kJointsPerVertex; ++slot)
						{
							vertex.joints[slot] = static_cast<uint16_t>(
								(std::min)(joints[i * 4 + slot], uint32_t(0xFFFF)));
							vertex.weights[slot] = weights[i * 4 + slot] * inv;
						}
					}
				}

				// 와인딩 — 법선이 있을 때만 맞춘다.
				if (!normals.empty())
				{
					for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3)
						OrientTriangle(mesh, first);
				}

				primitivesOfMesh[m].push_back(static_cast<uint32_t>(out.meshes.size()));
				out.meshes.push_back(std::move(mesh));
			}
		}

		// ── 6) 노드 → SourceNode ───────────────────────
		out.nodes.resize(order.gltfIndex.size());
		for (size_t i = 0; i < order.gltfIndex.size(); ++i)
		{
			const uint32_t gltfIndex = order.gltfIndex[i];
			SourceNode& node = out.nodes[i];
			node.name = nodes[gltfIndex]["name"].AsString();
			node.parent = order.parent[i];
			node.local = NodeLocal(nodes[gltfIndex]);

			const JsonValue& meshRef = nodes[gltfIndex]["mesh"];
			if (!meshRef.IsNull())
			{
				const uint32_t meshIndex = meshRef.AsUint();
				if (meshIndex < primitivesOfMesh.size())
					node.meshes = primitivesOfMesh[meshIndex];
			}
		}

		// 노드가 하나도 없는데 메시가 있으면(규격 위반 파일) 루트 하나를 만들어 전부 매단다.
		if (out.nodes.empty() && !out.meshes.empty())
		{
			SourceNode node;
			node.name = "root";
			node.meshes.reserve(out.meshes.size());
			for (size_t i = 0; i < out.meshes.size(); ++i)
				node.meshes.push_back(static_cast<uint32_t>(i));
			out.nodes.push_back(std::move(node));
		}

		// ── 7) 재질·텍스처 ─────────────────────────────
		if (!options.geometryOnly)
		{
			const JsonValue& materials = gltf.json["materials"];
			if (materials.Size() > kMaxMaterials)
			{
				error = L"glTF 재질이 너무 많습니다.";
				return false;
			}
			const JsonValue& textures = gltf.json["textures"];
			const JsonValue& images = gltf.json["images"];

			// 이미지 → (상대 경로) 또는 (내장 바이트). 같은 이미지를 두 번 담지 않는다.
			std::unordered_map<uint32_t, uint32_t> embeddedOfImage;

			// 텍스처 번호 → 이미지 번호
			const auto imageOfTexture = [&](uint32_t textureIndex) -> uint32_t
			{
				if (textureIndex >= textures.Size()) return kInvalidIndex;
				const JsonValue& source = textures[textureIndex]["source"];
				return source.IsNull() ? kInvalidIndex : source.AsUint();
			};

			// 재질 슬롯 하나를 채운다.
			const auto assignTexture = [&](SourceMaterial& material, SourceTextureSlot slot,
				const JsonValue& textureRef) -> bool
			{
				if (textureRef.IsNull() || !textureRef.Has("index")) return true;
				const uint32_t imageIndex = imageOfTexture(textureRef["index"].AsUint());
				if (imageIndex == kInvalidIndex || imageIndex >= images.Size()) return true;

				const JsonValue& image = images[imageIndex];
				const std::string uri = image["uri"].AsString();
				const size_t slotIndex = static_cast<size_t>(slot);

				if (!uri.empty() && uri.rfind("data:", 0) != 0)
				{
					// 외부 파일 — 모델 폴더 기준 상대 경로를 그대로 넘긴다(ModelBuilder 가 푼다).
					material.texturePaths[slotIndex] = PercentDecode(uri);
					return true;
				}

				// 내장 이미지 — .glb 의 bufferView 또는 data URI
				if (auto it = embeddedOfImage.find(imageIndex); it != embeddedOfImage.end())
				{
					material.embeddedImages[slotIndex] = it->second;
					return true;
				}

				SourceImage embedded;
				embedded.name = image["name"].AsString();
				embedded.mimeType = image["mimeType"].AsString();

				if (!uri.empty())
				{
					std::string mime;
					if (!DecodeDataUri(uri, embedded.bytes, mime))
					{
						error = L"glTF 내장 이미지의 data URI 를 해석하지 못했습니다.";
						return false;
					}
					if (embedded.mimeType.empty()) embedded.mimeType = mime;
				}
				else if (image.Has("bufferView"))
				{
					const uint32_t viewIndex = image["bufferView"].AsUint();
					const JsonValue& views = gltf.json["bufferViews"];
					if (viewIndex >= views.Size())
					{
						error = L"glTF 이미지의 bufferView 번호가 범위를 벗어났습니다.";
						return false;
					}
					const JsonValue& view = views[viewIndex];
					const uint32_t bufferIndex = view["buffer"].AsUint();
					if (bufferIndex >= gltf.buffers.size())
					{
						error = L"glTF 이미지의 buffer 번호가 범위를 벗어났습니다.";
						return false;
					}
					const std::vector<uint8_t>& buffer = gltf.buffers[bufferIndex];
					const size_t offset = view["byteOffset"].AsUint(0);
					const size_t length = view["byteLength"].AsUint(0);
					if (offset > buffer.size() || length > buffer.size() - offset)
					{
						error = L"glTF 내장 이미지가 buffer 범위를 벗어났습니다.";
						return false;
					}
					embedded.bytes.assign(buffer.begin() + offset, buffer.begin() + offset + length);
				}
				else
				{
					return true;   // uri 도 bufferView 도 없는 이미지 — 건너뛴다
				}

				const uint32_t embeddedIndex = static_cast<uint32_t>(out.images.size());
				out.images.push_back(std::move(embedded));
				embeddedOfImage.emplace(imageIndex, embeddedIndex);
				material.embeddedImages[slotIndex] = embeddedIndex;
				return true;
			};

			out.materials.resize(materials.Size());
			for (size_t i = 0; i < materials.Size(); ++i)
			{
				const JsonValue& source = materials[i];
				SourceMaterial& material = out.materials[i];
				material.name = source["name"].AsString();

				const JsonValue& pbr = source["pbrMetallicRoughness"];
				const JsonValue& baseColor = pbr["baseColorFactor"];
				if (baseColor.IsArray() && baseColor.Size() == 4)
				{
					material.baseColor = {
						static_cast<float>(baseColor.At(0).AsNumber(1.0)),
						static_cast<float>(baseColor[1].AsNumber(1.0)),
						static_cast<float>(baseColor[2].AsNumber(1.0)),
						static_cast<float>(baseColor[3].AsNumber(1.0)) };
				}
				material.roughness = std::clamp(
					static_cast<float>(pbr["roughnessFactor"].AsNumber(1.0)), 0.04f, 1.0f);
				material.metallic = std::clamp(
					static_cast<float>(pbr["metallicFactor"].AsNumber(1.0)), 0.0f, 1.0f);

				const JsonValue& emissive = source["emissiveFactor"];
				if (emissive.IsArray() && emissive.Size() == 3)
				{
					material.emissive = {
						static_cast<float>(emissive.At(0).AsNumber()),
						static_cast<float>(emissive[1].AsNumber()),
						static_cast<float>(emissive[2].AsNumber()) };
				}

				// ★ glTF 는 금속도와 거칠기를 한 텍스처(B=금속, G=거칠기)에 담는다.
				//   우리 셰이더는 슬롯이 따로라 같은 이미지를 두 슬롯에 걸어 둔다.
				//   채널 선택은 셰이더가 하지 않으므로, 분리 텍스처를 쓰려면 굽는 단계에서 나눠야 한다.
				if (!assignTexture(material, SourceTextureSlot::BaseColor, pbr["baseColorTexture"])) return false;
				if (!assignTexture(material, SourceTextureSlot::Roughness, pbr["metallicRoughnessTexture"])) return false;
				if (!assignTexture(material, SourceTextureSlot::Metallic, pbr["metallicRoughnessTexture"])) return false;
				if (!assignTexture(material, SourceTextureSlot::Normal, source["normalTexture"])) return false;
				if (!assignTexture(material, SourceTextureSlot::Emissive, source["emissiveTexture"])) return false;
			}

			// 메시가 가리키는 재질 번호가 범위를 넘었으면 재질 없음으로 돌린다.
			for (SourceMesh& mesh : out.meshes)
			{
				if (mesh.material != kInvalidIndex && mesh.material >= out.materials.size())
					mesh.material = kInvalidIndex;
			}
		}

		// ── 8) 애니메이션 클립 ─────────────────────────
		if (wantAnimation && !out.skeleton.Empty())
		{
			const JsonValue& animations = gltf.json["animations"];
			if (animations.Size() > kMaxAnimations)
			{
				error = L"glTF 애니메이션이 너무 많습니다.";
				return false;
			}

			for (size_t a = 0; a < animations.Size(); ++a)
			{
				const JsonValue& source = animations[a];
				AnimationSource clip;
				clip.name = source["name"].AsString();
				if (clip.name.empty()) clip.name = "clip" + std::to_string(a);

				const JsonValue& channels = source["channels"];
				const JsonValue& samplers = source["samplers"];

				for (size_t c = 0; c < channels.Size(); ++c)
				{
					const JsonValue& channel = channels[c];
					const JsonValue& target = channel["target"];

					// 조인트가 아닌 노드(소품 움직임 등)는 지금 쓰지 않는다 — 조용히 건너뛴다.
					const JsonValue& targetNode = target["node"];
					if (targetNode.IsNull()) continue;
					const auto joint = jointOfNode.find(targetNode.AsUint());
					if (joint == jointOfNode.end()) continue;

					const std::string path = target["path"].AsString();
					AnimationPath which;
					if (path == "translation")   which = AnimationPath::Translation;
					else if (path == "rotation") which = AnimationPath::Rotation;
					else if (path == "scale")    which = AnimationPath::Scale;
					else continue;               // weights(모프)는 지원하지 않는다

					const uint32_t samplerIndex = channel["sampler"].AsUint();
					if (samplerIndex >= samplers.Size()) continue;
					const JsonValue& sampler = samplers[samplerIndex];

					std::vector<float> times, values;
					size_t timeComponents = 0, timeCount = 0;
					if (!ReadFloats(gltf, sampler["input"].AsUint(), times, timeComponents, timeCount, error))
						return false;
					size_t valueComponents = 0, valueCount = 0;
					if (!ReadFloats(gltf, sampler["output"].AsUint(), values, valueComponents, valueCount, error))
						return false;
					if (timeComponents != 1 || timeCount == 0) continue;

					const size_t stride = which == AnimationPath::Rotation ? 4u : 3u;
					if (valueComponents != stride) continue;

					AnimationChannel out_channel;
					out_channel.joint = joint->second;
					out_channel.path = which;

					const std::string interpolation = sampler["interpolation"].AsString();
					if (interpolation == "STEP")
					{
						out_channel.interpolation = AnimationInterpolation::Step;
					}
					else if (interpolation == "CUBICSPLINE")
					{
						// ★ 키프레임당 (들어오는 접선, 값, 나가는 접선) 3개가 들어 있다.
						//   가운데 값만 뽑아 Linear 로 낮춘다 — 접선을 쓰는 샘플러를 들이지 않는다.
						if (valueCount != timeCount * 3) continue;
						std::vector<float> middle(timeCount * stride);
						for (size_t k = 0; k < timeCount; ++k)
						{
							const size_t sourceOffset = (k * 3 + 1) * stride;
							for (size_t e = 0; e < stride; ++e)
								middle[k * stride + e] = values[sourceOffset + e];
						}
						values = std::move(middle);
						valueCount = timeCount;
						out_channel.interpolation = AnimationInterpolation::Linear;
					}
					else
					{
						out_channel.interpolation = AnimationInterpolation::Linear;
					}

					if (valueCount != timeCount) continue;

					out_channel.times = std::move(times);
					out_channel.values = std::move(values);
					clip.duration = (std::max)(clip.duration, out_channel.times.back());
					clip.channels.push_back(std::move(out_channel));
				}

				if (!clip.channels.empty())
					out.animations.push_back(std::move(clip));
			}
		}

		if (out.meshes.empty() && out.animations.empty() && out.skeleton.Empty())
		{
			error = L"glTF 에서 읽을 것이 없습니다(삼각형 메시·스킨·애니메이션 모두 없음).";
			return false;
		}
		return true;
	}

} // namespace Shared
