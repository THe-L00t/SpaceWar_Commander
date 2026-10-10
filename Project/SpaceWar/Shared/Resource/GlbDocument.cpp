#include "GlbDocument.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Shared {
	namespace {

		constexpr uint32_t kGlbMagic = 0x46546C67;
		constexpr uint32_t kJsonChunk = 0x4E4F534A;
		constexpr uint32_t kBinaryChunk = 0x004E4942;
		constexpr size_t kMaxJsonBytes = 16 * 1024 * 1024;
		constexpr size_t kMaxFileBytes = 1024 * 1024 * 1024;
		constexpr size_t kMaxPrimitiveBytes = 512 * 1024 * 1024;

		struct GlbError { std::wstring message; };
		[[noreturn]] void Fail(const wchar_t* message) { throw GlbError{ message }; }

		enum class JsonKind { Null, Boolean, Number, String, Array, Object };

		struct Json
		{
			JsonKind kind = JsonKind::Null;
			bool boolean = false;
			double number = 0;
			std::string text;
			std::vector<Json> values;
			std::vector<std::string> keys;

			const Json* Find(std::string_view key) const
			{
				if (kind != JsonKind::Object) Fail(L"GLB JSON 객체 형식이 유효하지 않습니다.");
				for (size_t i = 0; i < keys.size(); ++i)
					if (keys[i] == key) return &values[i];
				return nullptr;
			}
		};

		// 필요한 GLB JSON을 표준 라이브러리만으로 읽는다.
		class JsonParser
		{
		public:
			explicit JsonParser(std::string_view source) : source(source) {}

			Json Parse()
			{
				Json result = Value(0);
				Space();
				if (position != source.size()) Fail(L"GLB JSON 끝에 잘못된 데이터가 있습니다.");
				return result;
			}

		private:
			void Space()
			{
				while (position < source.size())
				{
					const char c = source[position];
					if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
					++position;
				}
			}

			bool Take(char value)
			{
				Space();
				if (position == source.size() || source[position] != value) return false;
				++position;
				return true;
			}

			void Expect(char value)
			{
				if (!Take(value)) Fail(L"GLB JSON 구문이 유효하지 않습니다.");
			}

			bool Literal(std::string_view word)
			{
				if (source.substr(position, word.size()) != word) return false;
				position += word.size();
				return true;
			}

			static void Utf8(std::string& result, uint32_t codepoint)
			{
				if (codepoint <= 0x7F) result.push_back(static_cast<char>(codepoint));
				else if (codepoint <= 0x7FF)
				{
					result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
					result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
				}
				else if (codepoint <= 0xFFFF)
				{
					result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
					result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
					result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
				}
				else
				{
					result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
					result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
					result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
					result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
				}
			}

			uint32_t Hex4()
			{
				if (source.size() - position < 4) Fail(L"GLB JSON 유니코드 문자가 잘못되었습니다.");
				uint32_t value = 0;
				for (unsigned i = 0; i < 4; ++i)
				{
					const char c = source[position++];
					const int digit = c >= '0' && c <= '9' ? c - '0'
						: c >= 'a' && c <= 'f' ? c - 'a' + 10
						: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
					if (digit < 0) Fail(L"GLB JSON 유니코드 문자가 잘못되었습니다.");
					value = (value << 4) | static_cast<uint32_t>(digit);
				}
				return value;
			}

			std::string String()
			{
				Expect('"');
				std::string result;
				while (position < source.size())
				{
					const auto c = static_cast<unsigned char>(source[position++]);
					if (c == '"') return result;
					if (c < 0x20) Fail(L"GLB JSON 문자열에 제어 문자가 있습니다.");
					if (c == '\\')
					{
						if (position == source.size()) Fail(L"GLB JSON 문자열이 끝나지 않았습니다.");
						switch (source[position++])
						{
						case '"': result.push_back('"'); break;
						case '\\': result.push_back('\\'); break;
						case '/': result.push_back('/'); break;
						case 'b': result.push_back('\b'); break;
						case 'f': result.push_back('\f'); break;
						case 'n': result.push_back('\n'); break;
						case 'r': result.push_back('\r'); break;
						case 't': result.push_back('\t'); break;
						case 'u':
						{
							uint32_t cp = Hex4();
							if (cp >= 0xD800 && cp <= 0xDBFF)
							{
								if (source.size() - position < 2 || source[position] != '\\'
									|| source[position + 1] != 'u')
									Fail(L"GLB JSON 유니코드 surrogate가 잘못되었습니다.");
								position += 2;
								const uint32_t low = Hex4();
								if (low < 0xDC00 || low > 0xDFFF)
									Fail(L"GLB JSON 유니코드 surrogate가 잘못되었습니다.");
								cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
							}
							else if (cp >= 0xDC00 && cp <= 0xDFFF)
								Fail(L"GLB JSON 유니코드 surrogate가 잘못되었습니다.");
							Utf8(result, cp);
							break;
						}
						default: Fail(L"GLB JSON 문자열 escape가 잘못되었습니다.");
						}
					}
					else if (c < 0x80) result.push_back(static_cast<char>(c));
					else
					{
						// 원문 UTF-8도 검증하여 이름을 손상 없이 유지한다.
						const unsigned extra = c >= 0xC2 && c <= 0xDF ? 1
							: c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
						if (!extra || source.size() - position < extra)
							Fail(L"GLB JSON UTF-8 문자열이 잘못되었습니다.");
						uint32_t cp = c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
						for (unsigned i = 0; i < extra; ++i)
						{
							const auto tail = static_cast<unsigned char>(source[position++]);
							if ((tail & 0xC0) != 0x80) Fail(L"GLB JSON UTF-8 문자열이 잘못되었습니다.");
							cp = (cp << 6) | (tail & 0x3F);
						}
						if (cp < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u)
							|| cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
							Fail(L"GLB JSON UTF-8 문자열이 잘못되었습니다.");
						Utf8(result, cp);
					}
				}
				Fail(L"GLB JSON 문자열이 끝나지 않았습니다.");
			}

			Json Value(unsigned depth)
			{
				if (depth > 128 || ++tokens > 1000000) Fail(L"GLB JSON이 너무 복잡합니다.");
				Space();
				if (position == source.size()) Fail(L"GLB JSON이 잘렸습니다.");
				Json result;
				const char c = source[position];
				if (c == '{')
				{
					result.kind = JsonKind::Object;
					++position;
					if (Take('}')) return result;
					do
					{
						std::string key = String();
						if (std::find(result.keys.begin(), result.keys.end(), key) != result.keys.end())
							Fail(L"GLB JSON에 중복된 속성이 있습니다.");
						Expect(':');
						result.keys.push_back(std::move(key));
						result.values.push_back(Value(depth + 1));
					} while (Take(','));
					Expect('}');
				}
				else if (c == '[')
				{
					result.kind = JsonKind::Array;
					++position;
					if (Take(']')) return result;
					do { result.values.push_back(Value(depth + 1)); } while (Take(','));
					Expect(']');
				}
				else if (c == '"')
				{
					result.kind = JsonKind::String;
					result.text = String();
				}
				else if (c == 't' && Literal("true"))
				{
					result.kind = JsonKind::Boolean;
					result.boolean = true;
				}
				else if (c == 'f' && Literal("false")) result.kind = JsonKind::Boolean;
				else if (c == 'n' && Literal("null")) {}
				else
				{
					const size_t first = position;
					if (source[position] == '-') ++position;
					if (position == source.size()) Fail(L"GLB JSON 숫자가 잘못되었습니다.");
					if (source[position] == '0') ++position;
					else
					{
						if (source[position] < '1' || source[position] > '9')
							Fail(L"GLB JSON 숫자가 잘못되었습니다.");
						while (position < source.size() && source[position] >= '0'
							&& source[position] <= '9') ++position;
					}
					if (position < source.size() && source[position] == '.')
					{
						const size_t digits = ++position;
						while (position < source.size() && source[position] >= '0'
							&& source[position] <= '9') ++position;
						if (position == digits) Fail(L"GLB JSON 숫자가 잘못되었습니다.");
					}
					if (position < source.size() && (source[position] == 'e' || source[position] == 'E'))
					{
						++position;
						if (position < source.size() && (source[position] == '+' || source[position] == '-'))
							++position;
						const size_t digits = position;
						while (position < source.size() && source[position] >= '0'
							&& source[position] <= '9') ++position;
						if (position == digits) Fail(L"GLB JSON 숫자가 잘못되었습니다.");
					}
					const auto parsed = std::from_chars(source.data() + first,
						source.data() + position, result.number);
					if (parsed.ec != std::errc{} || parsed.ptr != source.data() + position
						|| !std::isfinite(result.number)) Fail(L"GLB JSON 숫자가 범위를 벗어났습니다.");
					result.kind = JsonKind::Number;
				}
				return result;
			}

			std::string_view source;
			size_t position = 0;
			size_t tokens = 0;
		};

		const Json& Required(const Json& object, std::string_view key)
		{
			const Json* value = object.Find(key);
			if (!value) Fail(L"GLB JSON 필수 속성이 없습니다.");
			return *value;
		}

		const std::vector<Json>& Array(const Json& value)
		{
			if (value.kind != JsonKind::Array) Fail(L"GLB JSON 배열 형식이 유효하지 않습니다.");
			return value.values;
		}

		std::string Text(const Json& value)
		{
			if (value.kind != JsonKind::String) Fail(L"GLB JSON 문자열 형식이 유효하지 않습니다.");
			return value.text;
		}

		uint32_t UInt(const Json& value)
		{
			if (value.kind != JsonKind::Number || value.number < 0
				|| value.number > UINT32_MAX || std::floor(value.number) != value.number)
				Fail(L"GLB JSON 인덱스 또는 크기가 유효하지 않습니다.");
			return static_cast<uint32_t>(value.number);
		}

		uint32_t UIntOr(const Json& object, std::string_view key, uint32_t fallback)
		{
			const Json* value = object.Find(key);
			if (!value) return fallback;
			const uint32_t result = UInt(*value);
			if (result == kInvalidGlbIndex && fallback == kInvalidGlbIndex)
				Fail(L"GLB 인덱스가 예약된 invalid 값입니다.");
			return result;
		}

		float Scalar(const Json& value)
		{
			if (value.kind != JsonKind::Number
				|| std::abs(value.number) > std::numeric_limits<float>::max())
				Fail(L"GLB 실수 값이 유효하지 않습니다.");
			return static_cast<float>(value.number);
		}

		bool BoolOr(const Json& object, std::string_view key, bool fallback)
		{
			const Json* value = object.Find(key);
			if (!value) return fallback;
			if (value->kind != JsonKind::Boolean) Fail(L"GLB JSON 논리 값이 잘못되었습니다.");
			return value->boolean;
		}

		template<size_t Count>
		void FloatArray(const Json& value, std::array<float, Count>& out)
		{
			const auto& array = Array(value);
			if (array.size() != Count) Fail(L"GLB 벡터 또는 행렬 크기가 잘못되었습니다.");
			for (size_t i = 0; i < Count; ++i) out[i] = Scalar(array[i]);
		}

		uint32_t ComponentBytes(uint32_t component)
		{
			switch (component)
			{
			case 5121: return 1;
			case 5123: return 2;
			case 5125: case 5126: return 4;
			default: Fail(L"지원하지 않는 GLB accessor componentType입니다.");
			}
		}

		uint32_t Dimensions(const Json& value)
		{
			const std::string type = Text(value);
			if (type == "SCALAR") return 1;
			if (type == "VEC2") return 2;
			if (type == "VEC3") return 3;
			if (type == "VEC4") return 4;
			Fail(L"정적 메시에서 지원하지 않는 GLB accessor type입니다.");
		}

		uint32_t Little32(const uint8_t* data)
		{
			return static_cast<uint32_t>(data[0])
				| (static_cast<uint32_t>(data[1]) << 8)
				| (static_cast<uint32_t>(data[2]) << 16)
				| (static_cast<uint32_t>(data[3]) << 24);
		}

		uint32_t Read32(std::ifstream& input)
		{
			uint8_t bytes[4]{};
			if (!input.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
				Fail(L"GLB 헤더가 잘렸습니다.");
			return Little32(bytes);
		}

		void NoData(const Json& document, std::string_view key, const wchar_t* error)
		{
			const Json* value = document.Find(key);
			if (value && !Array(*value).empty()) Fail(error);
		}

		std::array<float, 16> Multiply(const std::array<float, 16>& first,
			const std::array<float, 16>& second)
		{
			std::array<float, 16> result{};
			for (size_t row = 0; row < 4; ++row)
				for (size_t column = 0; column < 4; ++column)
				{
					double value = 0;
					for (size_t k = 0; k < 4; ++k)
						value += static_cast<double>(first[row * 4 + k]) * second[k * 4 + column];
					if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
						Fail(L"GLB 노드의 누적 변환이 범위를 벗어났습니다.");
					result[row * 4 + column] = static_cast<float>(value);
				}
			return result;
		}

		std::array<float, 16> LocalTransform(const Json& node)
		{
			std::array<float, 16> result{
				1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			const Json* matrix = node.Find("matrix");
			if (matrix)
			{
				if (node.Find("translation") || node.Find("rotation") || node.Find("scale"))
					Fail(L"GLB 노드에 matrix와 TRS가 동시에 있습니다.");
				// glTF column-major 배열은 전치된 row-major 행 벡터 배열과 동일하다.
				FloatArray(*matrix, result);
				if (result[3] != 0 || result[7] != 0 || result[11] != 0 || result[15] != 1)
					Fail(L"GLB 노드의 투영 행렬은 지원하지 않습니다.");
			}
			else
			{
				std::array<float, 3> translation{}, scale{ 1, 1, 1 };
				std::array<float, 4> rotation{ 0, 0, 0, 1 };
				if (const Json* value = node.Find("translation")) FloatArray(*value, translation);
				if (const Json* value = node.Find("scale")) FloatArray(*value, scale);
				if (const Json* value = node.Find("rotation")) FloatArray(*value, rotation);
				const double length = std::sqrt(static_cast<double>(rotation[0]) * rotation[0]
					+ static_cast<double>(rotation[1]) * rotation[1]
					+ static_cast<double>(rotation[2]) * rotation[2]
					+ static_cast<double>(rotation[3]) * rotation[3]);
				if (!std::isfinite(length) || length < 1e-12) Fail(L"GLB 회전 quaternion이 잘못되었습니다.");
				const double x = rotation[0] / length, y = rotation[1] / length;
				const double z = rotation[2] / length, w = rotation[3] / length;
				const double rows[9]{
					1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
					2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
					2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
				for (size_t row = 0; row < 3; ++row)
					for (size_t column = 0; column < 3; ++column)
						result[row * 4 + column] = static_cast<float>(rows[row * 3 + column] * scale[row]);
				std::copy(translation.begin(), translation.end(), result.begin() + 12);
			}
			// H * transpose(M) * H: Z 반사는 메시와 노드에 한 번씩 같은 규약으로 적용한다.
			for (size_t row = 0; row < 4; ++row)
				for (size_t column = 0; column < 4; ++column)
					if ((row == 2) != (column == 2)) result[row * 4 + column] = -result[row * 4 + column];
			const double determinant = static_cast<double>(result[0])
					* (static_cast<double>(result[5]) * result[10] - static_cast<double>(result[6]) * result[9])
				- static_cast<double>(result[1])
					* (static_cast<double>(result[4]) * result[10] - static_cast<double>(result[6]) * result[8])
				+ static_cast<double>(result[2])
					* (static_cast<double>(result[4]) * result[9] - static_cast<double>(result[5]) * result[8]);
			if (!std::isfinite(determinant) || determinant <= 0)
				Fail(L"GLB 노드의 영 스케일 또는 반사 스케일은 지원하지 않습니다.");
			return result;
		}

		struct PendingNode
		{
			GlbNode node;
			std::vector<uint32_t> children;
		};

		void Normalize(std::array<float, 3>& value)
		{
			const double length = std::sqrt(static_cast<double>(value[0]) * value[0]
				+ static_cast<double>(value[1]) * value[1]
				+ static_cast<double>(value[2]) * value[2]);
			if (!std::isfinite(length) || length < 1e-12) Fail(L"GLB 정점 법선이 유효하지 않습니다.");
			for (float& item : value) item = static_cast<float>(item / length);
		}

	} // namespace

	bool GlbDocument::Load(const wchar_t* path, std::wstring& error)
	{
		error.clear();
		try
		{
			if (!path || !*path) Fail(L"GLB 경로가 비어 있습니다.");
			std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
			if (!input) Fail(L"GLB 파일을 열지 못했습니다.");
			const std::streamoff fileSize = input.tellg();
			if (fileSize < 20 || static_cast<uint64_t>(fileSize) > kMaxFileBytes)
				Fail(L"GLB 파일 크기가 유효하지 않거나 1 GiB를 초과합니다.");
			input.seekg(0);
			if (Read32(input) != kGlbMagic || Read32(input) != 2)
				Fail(L"GLB 2.0 파일이 아닙니다.");
			if (Read32(input) != static_cast<uint64_t>(fileSize))
				Fail(L"GLB 헤더와 실제 파일 크기가 다릅니다.");

			GlbDocument loaded;
			std::string jsonText;
			size_t cursor = 12;
			bool hasJson = false, hasBinary = false;
			while (cursor < static_cast<size_t>(fileSize))
			{
				if (static_cast<size_t>(fileSize) - cursor < 8) Fail(L"GLB chunk 헤더가 잘렸습니다.");
				const uint32_t length = Read32(input);
				const uint32_t kind = Read32(input);
				cursor += 8;
				if ((length & 3) || length > static_cast<size_t>(fileSize) - cursor)
					Fail(L"GLB chunk 크기 또는 정렬이 잘못되었습니다.");
				if (!hasJson && kind != kJsonChunk) Fail(L"GLB 첫 chunk는 JSON이어야 합니다.");
				if (kind == kJsonChunk)
				{
					if (hasJson || !length || length > kMaxJsonBytes)
						Fail(L"GLB JSON chunk가 중복되거나 너무 큽니다.");
					jsonText.resize(length);
					if (!input.read(jsonText.data(), length)) Fail(L"GLB JSON chunk가 잘렸습니다.");
					hasJson = true;
				}
				else if (kind == kBinaryChunk)
				{
					if (hasBinary || !length) Fail(L"GLB BIN chunk가 중복되거나 비어 있습니다.");
					loaded.binary.resize(length);
					if (!input.read(reinterpret_cast<char*>(loaded.binary.data()), length))
						Fail(L"GLB BIN chunk가 잘렸습니다.");
					hasBinary = true;
				}
				else
				{
					// GLB 규격상 모르는 추가 chunk는 건너뛴다.
					input.seekg(length, std::ios::cur);
					if (!input) Fail(L"GLB 추가 chunk가 잘렸습니다.");
				}
				cursor += length;
			}
			if (!hasJson || !hasBinary) Fail(L"GLB JSON 또는 BIN chunk가 없습니다.");
			const Json document = JsonParser(jsonText).Parse();
			const Json& asset = Required(document, "asset");
			if (Text(Required(asset, "version")) != "2.0") Fail(L"glTF 2.0만 지원합니다.");
			if (const Json* minimum = asset.Find("minVersion"))
				if (Text(*minimum) != "2.0") Fail(L"GLB에서 요구하는 glTF 버전을 지원하지 않습니다.");
			if (const Json* required = document.Find("extensionsRequired"))
				for (const Json& extension : Array(*required))
					if (Text(extension) != "KHR_materials_emissive_strength")
						Fail(L"지원하지 않는 필수 glTF 확장이 있습니다.");
			NoData(document, "animations", L"이 GLB 로더는 정적 맵만 지원하며 애니메이션을 읽지 않습니다.");
			NoData(document, "skins", L"이 GLB 로더는 스킨 메시를 지원하지 않습니다.");

			const auto& buffers = Array(Required(document, "buffers"));
			if (buffers.size() != 1 || buffers[0].Find("uri"))
				Fail(L"GLB 내부 BIN 버퍼 하나만 지원합니다.");
			const size_t binaryLength = UInt(Required(buffers[0], "byteLength"));
			if (!binaryLength || binaryLength > loaded.binary.size()
				|| loaded.binary.size() - binaryLength > 3)
				Fail(L"GLB 버퍼 길이와 BIN chunk가 다릅니다.");

			const auto& jsonViews = Array(Required(document, "bufferViews"));
			loaded.views.reserve(jsonViews.size());
			for (const Json& item : jsonViews)
			{
				if (UInt(Required(item, "buffer")) != 0) Fail(L"GLB bufferView 버퍼가 잘못되었습니다.");
				if (const Json* extensions = item.Find("extensions"))
					if (extensions->Find("EXT_meshopt_compression"))
						Fail(L"meshopt 압축 GLB는 지원하지 않습니다.");
				BufferView view;
				view.offset = UIntOr(item, "byteOffset", 0);
				view.length = UInt(Required(item, "byteLength"));
				view.stride = UIntOr(item, "byteStride", 0);
				if (view.offset > binaryLength || view.length > binaryLength - view.offset
					|| (view.offset & 3) || (view.stride && (view.stride < 4
						|| view.stride > 252 || (view.stride & 3))))
					Fail(L"GLB bufferView 범위 또는 정렬이 잘못되었습니다.");
				loaded.views.push_back(view);
			}

			if (const Json* jsonImages = document.Find("images"))
			{
				const auto& items = Array(*jsonImages);
				loaded.images.reserve(items.size());
				for (const Json& item : items)
				{
					if (item.Find("uri")) Fail(L"외부 또는 data URI GLB 이미지는 지원하지 않습니다.");
					GlbImage image;
					if (const Json* name = item.Find("name")) image.name = Text(*name);
					image.mimeType = Text(Required(item, "mimeType"));
					if (image.mimeType != "image/png" && image.mimeType != "image/jpeg")
						Fail(L"GLB 내장 이미지는 PNG 또는 JPEG만 지원합니다.");
					image.view = UInt(Required(item, "bufferView"));
					if (image.view >= loaded.views.size() || loaded.views[image.view].stride
						|| !loaded.views[image.view].length)
						Fail(L"GLB 이미지 bufferView가 유효하지 않습니다.");
					loaded.images.push_back(std::move(image));
				}
			}

			// 현재 렌더러의 linear/repeat sampler로 표현할 수 있는 설정만 받는다.
			// mip 필터는 한 mip 텍스처에서 같은 base-level linear 필터로 동작한다.
			size_t samplerCount = 0;
			if (const Json* samplers = document.Find("samplers"))
			{
				const auto& items = Array(*samplers);
				samplerCount = items.size();
				for (const Json& item : items)
				{
					const uint32_t minFilter = UIntOr(item, "minFilter", 9987);
					if (UIntOr(item, "magFilter", 9729) != 9729
						|| (minFilter != 9729 && minFilter != 9985 && minFilter != 9987)
						|| UIntOr(item, "wrapS", 10497) != 10497
						|| UIntOr(item, "wrapT", 10497) != 10497)
						Fail(L"이 GLB 로더는 linear/repeat 텍스처 sampler만 지원합니다.");
				}
			}
			if (const Json* jsonTextures = document.Find("textures"))
			{
				const auto& items = Array(*jsonTextures);
				loaded.textures.reserve(items.size());
				for (const Json& item : items)
				{
					if (const Json* extensions = item.Find("extensions"))
						if (extensions->kind != JsonKind::Object || !extensions->keys.empty())
							Fail(L"GLB 텍스처 형식 확장은 지원하지 않습니다.");
					GlbTexture texture;
					texture.source = UInt(Required(item, "source"));
					if (texture.source >= loaded.images.size()) Fail(L"GLB texture 이미지 인덱스가 잘못되었습니다.");
					const uint32_t sampler = UIntOr(item, "sampler", kInvalidGlbIndex);
					if (sampler != kInvalidGlbIndex && sampler >= samplerCount)
						Fail(L"GLB texture sampler 인덱스가 잘못되었습니다.");
					loaded.textures.push_back(texture);
				}
			}
			auto textureIndex = [&](const Json& info)
			{
				const uint32_t index = UInt(Required(info, "index"));
				if (index >= loaded.textures.size()) Fail(L"GLB 재질 texture 인덱스가 잘못되었습니다.");
				if (UIntOr(info, "texCoord", 0) != 0) Fail(L"GLB 텍스처는 TEXCOORD_0만 지원합니다.");
				if (const Json* extensions = info.Find("extensions"))
					if (extensions->kind != JsonKind::Object || !extensions->keys.empty())
						Fail(L"GLB textureInfo 변환 확장은 지원하지 않습니다.");
				return index;
			};

			const auto& jsonAccessors = Array(Required(document, "accessors"));
			loaded.accessors.reserve(jsonAccessors.size());
			for (const Json& item : jsonAccessors)
			{
				if (item.Find("sparse")) Fail(L"sparse GLB accessor는 지원하지 않습니다.");
				Accessor accessor;
				accessor.view = UInt(Required(item, "bufferView"));
				accessor.offset = UIntOr(item, "byteOffset", 0);
				accessor.count = UInt(Required(item, "count"));
				accessor.component = UInt(Required(item, "componentType"));
				accessor.dimensions = Dimensions(Required(item, "type"));
				accessor.normalized = BoolOr(item, "normalized", false);
				const size_t componentBytes = ComponentBytes(accessor.component);
				if (accessor.normalized && (accessor.component == 5125 || accessor.component == 5126))
					Fail(L"GLB accessor normalized 형식이 잘못되었습니다.");
				if (accessor.view >= loaded.views.size() || !accessor.count)
					Fail(L"GLB accessor 버퍼 또는 개수가 잘못되었습니다.");
				const BufferView& view = loaded.views[accessor.view];
				const size_t elementBytes = accessor.dimensions * componentBytes;
				const size_t stride = view.stride ? view.stride : elementBytes;
				if (accessor.offset > view.length || elementBytes > view.length - accessor.offset
					|| stride < elementBytes || (accessor.offset + view.offset) % componentBytes)
					Fail(L"GLB accessor 범위 또는 정렬이 잘못되었습니다.");
				// 곱셈 대신 나눗셈으로 마지막 요소를 검사하여 정수 overflow도 막는다.
				if (accessor.count - 1 > (view.length - accessor.offset - elementBytes) / stride)
					Fail(L"GLB accessor가 버퍼 범위를 벗어났습니다.");
				loaded.accessors.push_back(accessor);
			}

			if (const Json* jsonMaterials = document.Find("materials"))
			{
				const auto& items = Array(*jsonMaterials);
				loaded.materials.reserve(items.size());
				for (const Json& item : items)
				{
					GlbMaterial material;
					if (const Json* name = item.Find("name")) material.name = Text(*name);
					material.doubleSided = BoolOr(item, "doubleSided", false);
					if (const Json* alpha = item.Find("alphaMode"))
						if (Text(*alpha) != "OPAQUE") Fail(L"투명 또는 alpha mask GLB 재질은 지원하지 않습니다.");
					if (item.Find("normalTexture") || item.Find("occlusionTexture"))
						Fail(L"GLB normal/occlusion 텍스처는 지원하지 않습니다.");
					if (const Json* value = item.Find("emissiveTexture"))
						material.emissiveTexture = textureIndex(*value);
					if (const Json* pbr = item.Find("pbrMetallicRoughness"))
					{
						if (pbr->Find("metallicRoughnessTexture"))
							Fail(L"GLB metallic/roughness 텍스처는 지원하지 않습니다.");
						if (const Json* value = pbr->Find("baseColorTexture"))
							material.baseColorTexture = textureIndex(*value);
						if (const Json* value = pbr->Find("baseColorFactor")) FloatArray(*value, material.baseColor);
						if (const Json* value = pbr->Find("roughnessFactor")) material.roughness = Scalar(*value);
						if (const Json* value = pbr->Find("metallicFactor")) material.metallic = Scalar(*value);
					}
					if (const Json* value = item.Find("emissiveFactor")) FloatArray(*value, material.emissive);
					for (float value : material.baseColor)
						if (value < 0 || value > 1) Fail(L"GLB baseColorFactor가 범위를 벗어났습니다.");
					for (float value : material.emissive)
						if (value < 0) Fail(L"GLB emissiveFactor가 음수입니다.");
					if (material.roughness < 0 || material.roughness > 1
						|| material.metallic < 0 || material.metallic > 1)
						Fail(L"GLB PBR 재질 값이 범위를 벗어났습니다.");
					if (const Json* extensions = item.Find("extensions"))
					{
						if (const Json* extension = extensions->Find("KHR_materials_emissive_strength"))
						{
							float strength = 1.0f;
							if (const Json* value = extension->Find("emissiveStrength")) strength = Scalar(*value);
							if (strength < 0) Fail(L"GLB emissiveStrength가 음수입니다.");
							for (float& value : material.emissive)
							{
								const double scaled = static_cast<double>(value) * strength;
								if (!std::isfinite(scaled) || scaled > std::numeric_limits<float>::max())
									Fail(L"GLB emissiveStrength 계산이 범위를 벗어났습니다.");
								value = static_cast<float>(scaled);
							}
						}
					}
					// optional clearcoat/transmission/ior/sheen은 기본 PBR 값으로 표시한다.
					loaded.materials.push_back(std::move(material));
				}
			}

			const auto& jsonMeshes = Array(Required(document, "meshes"));
			loaded.meshes.reserve(jsonMeshes.size());
			auto validateAttribute = [&](uint32_t index, uint32_t dimensions, bool color = false)
			{
				if (index == kInvalidGlbIndex) return;
				if (index >= loaded.accessors.size()) Fail(L"GLB 정점 accessor 인덱스가 잘못되었습니다.");
				const Accessor& accessor = loaded.accessors[index];
				if ((!color && accessor.dimensions != dimensions)
					|| (color && accessor.dimensions != 3 && accessor.dimensions != 4))
					Fail(L"GLB 정점 속성 크기가 잘못되었습니다.");
				if (color && (accessor.component == 5121 || accessor.component == 5123)
					&& accessor.normalized) return;
				if (accessor.component != 5126 || accessor.normalized)
					Fail(L"GLB 정점 속성은 float32 형식이어야 합니다.");
			};
			for (const Json& item : jsonMeshes)
			{
				GlbMesh mesh;
				if (const Json* name = item.Find("name")) mesh.name = Text(*name);
				if (item.Find("weights")) Fail(L"GLB morph weight는 지원하지 않습니다.");
				const auto& primitives = Array(Required(item, "primitives"));
				if (primitives.empty()) Fail(L"GLB 메시 primitive가 비어 있습니다.");
				mesh.primitives.reserve(primitives.size());
				for (const Json& primitive : primitives)
				{
					if (UIntOr(primitive, "mode", 4) != 4) Fail(L"GLB 삼각형 primitive만 지원합니다.");
					if (primitive.Find("targets")) Fail(L"GLB morph target은 지원하지 않습니다.");
					if (const Json* extensions = primitive.Find("extensions"))
						if (extensions->Find("KHR_draco_mesh_compression"))
							Fail(L"Draco 압축 GLB는 지원하지 않습니다.");
					const Json& attributes = Required(primitive, "attributes");
					GlbPrimitiveInfo info;
					info.position = UInt(Required(attributes, "POSITION"));
					info.normal = UIntOr(attributes, "NORMAL", kInvalidGlbIndex);
					info.uv = UIntOr(attributes, "TEXCOORD_0", kInvalidGlbIndex);
					const uint32_t secondaryUv = UIntOr(attributes, "TEXCOORD_1", kInvalidGlbIndex);
					info.color = UIntOr(attributes, "COLOR_0", kInvalidGlbIndex);
					info.tangent = UIntOr(attributes, "TANGENT", kInvalidGlbIndex);
					info.indices = UIntOr(primitive, "indices", kInvalidGlbIndex);
					info.material = UIntOr(primitive, "material", kInvalidGlbIndex);
					for (const std::string& key : attributes.keys)
						if (key != "POSITION" && key != "NORMAL" && key != "TEXCOORD_0"
							&& key != "TEXCOORD_1" && key != "COLOR_0" && key != "TANGENT")
							Fail(L"지원하지 않는 GLB 정점 속성이 있습니다.");
					validateAttribute(info.position, 3);
					validateAttribute(info.normal, 3);
					validateAttribute(info.uv, 2);
					// 사용하지 않는 두 번째 UV도 형식과 개수를 검증한다.
					// 재질 textureInfo는 TEXCOORD_0만 허용하므로 렌더 데이터에는 보관하지 않는다.
					validateAttribute(secondaryUv, 2);
					validateAttribute(info.color, 4, true);
					validateAttribute(info.tangent, 4);
					if (info.position == kInvalidGlbIndex) Fail(L"GLB POSITION accessor가 없습니다.");
					const size_t vertexCount = loaded.accessors[info.position].count;
					for (uint32_t index : { info.normal, info.uv, secondaryUv, info.color, info.tangent })
						if (index != kInvalidGlbIndex && loaded.accessors[index].count != vertexCount)
							Fail(L"GLB 정점 속성 개수가 POSITION과 다릅니다.");
					size_t indexCount = vertexCount;
					if (info.indices != kInvalidGlbIndex)
					{
						if (info.indices >= loaded.accessors.size()) Fail(L"GLB index accessor가 잘못되었습니다.");
						const Accessor& accessor = loaded.accessors[info.indices];
						if (accessor.dimensions != 1 || accessor.normalized
							|| (accessor.component != 5121 && accessor.component != 5123 && accessor.component != 5125)
							|| loaded.views[accessor.view].stride)
							Fail(L"GLB index accessor 형식이 잘못되었습니다.");
						indexCount = accessor.count;
					}
					if (indexCount % 3) Fail(L"GLB 삼각형 인덱스 개수가 3의 배수가 아닙니다.");
					info.vertexCount = vertexCount;
					info.indexCount = indexCount;
					if (static_cast<uint64_t>(vertexCount) * sizeof(GlbVertex)
						+ static_cast<uint64_t>(indexCount) * sizeof(uint32_t) > kMaxPrimitiveBytes)
						Fail(L"GLB 단일 primitive가 메모리 제한을 초과합니다.");
					if (info.material != kInvalidGlbIndex && info.material >= loaded.materials.size())
						Fail(L"GLB material 인덱스가 잘못되었습니다.");
					if (info.material != kInvalidGlbIndex)
					{
						const GlbMaterial& material = loaded.materials[info.material];
						if ((material.baseColorTexture != kInvalidGlbIndex || material.emissiveTexture != kInvalidGlbIndex)
							&& info.uv == kInvalidGlbIndex)
							Fail(L"GLB 텍스처 재질에 필요한 TEXCOORD_0가 없습니다.");
					}
					mesh.primitives.push_back(info);
				}
				loaded.meshes.push_back(std::move(mesh));
			}

			const auto& jsonNodes = Array(Required(document, "nodes"));
			std::vector<PendingNode> pending;
			pending.reserve(jsonNodes.size());
			for (const Json& item : jsonNodes)
			{
				PendingNode entry;
				if (const Json* name = item.Find("name")) entry.node.name = Text(*name);
				entry.node.mesh = UIntOr(item, "mesh", kInvalidGlbIndex);
				if (entry.node.mesh != kInvalidGlbIndex && entry.node.mesh >= loaded.meshes.size())
					Fail(L"GLB 노드 mesh 인덱스가 잘못되었습니다.");
				if (item.Find("skin") || item.Find("weights"))
					Fail(L"GLB 스킨 또는 morph 노드는 지원하지 않습니다.");
				entry.node.local = LocalTransform(item);
				if (const Json* children = item.Find("children"))
					for (const Json& child : Array(*children)) entry.children.push_back(UInt(child));
				pending.push_back(std::move(entry));
			}

			std::vector<uint32_t> parents(pending.size(), kInvalidGlbIndex);
			for (size_t i = 0; i < pending.size(); ++i)
				for (uint32_t child : pending[i].children)
				{
					if (child >= pending.size() || child == i) Fail(L"GLB 노드 child가 잘못되었습니다.");
					if (parents[child] != kInvalidGlbIndex) Fail(L"GLB 노드에 여러 부모 또는 중복 child가 있습니다.");
					parents[child] = static_cast<uint32_t>(i);
				}
			// 활성 scene 밖에 있는 노드도 cycle이면 잘못된 glTF다.
			std::vector<uint8_t> state(pending.size(), 0);
			for (size_t i = 0; i < pending.size(); ++i)
			{
				if (state[i] == 2) continue;
				uint32_t current = static_cast<uint32_t>(i);
				while (current != kInvalidGlbIndex && state[current] == 0)
				{
					state[current] = 1;
					current = parents[current];
				}
				if (current != kInvalidGlbIndex && state[current] == 1) Fail(L"GLB 노드 계층에 cycle이 있습니다.");
				current = static_cast<uint32_t>(i);
				while (current != kInvalidGlbIndex && state[current] == 1)
				{
					state[current] = 2;
					current = parents[current];
				}
			}

			const auto& scenes = Array(Required(document, "scenes"));
			const uint32_t scene = UIntOr(document, "scene", 0);
			if (scene >= scenes.size()) Fail(L"GLB 활성 scene 인덱스가 잘못되었습니다.");
			const auto& roots = Array(Required(scenes[scene], "nodes"));
			std::vector<std::pair<uint32_t, uint32_t>> stack;
			std::vector<uint8_t> emitted(pending.size(), 0);
			loaded.nodes.reserve(pending.size());
			for (const Json& root : roots)
			{
				const uint32_t rootIndex = UInt(root);
				if (rootIndex >= pending.size() || parents[rootIndex] != kInvalidGlbIndex)
					Fail(L"GLB scene root 노드가 잘못되었습니다.");
				stack.emplace_back(rootIndex, kInvalidGlbIndex);
				while (!stack.empty())
				{
					const auto [originalIndex, parentIndex] = stack.back();
					stack.pop_back();
					if (emitted[originalIndex]) Fail(L"GLB 활성 scene에 중복된 노드가 있습니다.");
					emitted[originalIndex] = 1;
					PendingNode& entry = pending[originalIndex];
					entry.node.parent = parentIndex;
					entry.node.world = parentIndex == kInvalidGlbIndex
						? entry.node.local : Multiply(entry.node.local, loaded.nodes[parentIndex].world);
					const uint32_t newIndex = static_cast<uint32_t>(loaded.nodes.size());
					loaded.nodes.push_back(std::move(entry.node));
					for (auto child = entry.children.rbegin(); child != entry.children.rend(); ++child)
						stack.emplace_back(*child, newIndex);
				}
			}
			if (loaded.nodes.empty()) Fail(L"GLB 활성 scene에 노드가 없습니다.");
			*this = std::move(loaded);
			return true;
		}
		catch (const GlbError& failure) { error = failure.message; }
		catch (const std::bad_alloc&) { error = L"GLB 모델을 읽는 중 메모리를 할당하지 못했습니다."; }
		catch (const std::exception&) { error = L"GLB 파일을 읽는 중 입출력 또는 데이터 처리 오류가 발생했습니다."; }
		return false;
	}

	bool GlbDocument::ReadImage(uint32_t image, const uint8_t*& bytes, size_t& byteCount,
		std::wstring& error) const
	{
		error.clear();
		bytes = nullptr;
		byteCount = 0;
		try
		{
			if (image >= images.size() || images[image].view >= views.size())
				Fail(L"GLB 이미지 인덱스가 잘못되었습니다.");
			const BufferView& view = views[images[image].view];
			if (view.stride || !view.length || view.offset > binary.size()
				|| view.length > binary.size() - view.offset)
				Fail(L"GLB 이미지 데이터 범위가 잘못되었습니다.");
			const uint8_t* data = binary.data() + view.offset;
			constexpr uint8_t pngSignature[]{ 137, 80, 78, 71, 13, 10, 26, 10 };
			if (images[image].mimeType == "image/png")
			{
				if (view.length < sizeof(pngSignature) || std::memcmp(data, pngSignature, sizeof(pngSignature)))
					Fail(L"GLB PNG 이미지 서명이 잘못되었습니다.");
			}
			else if (images[image].mimeType == "image/jpeg")
			{
				if (view.length < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
					Fail(L"GLB JPEG 이미지 서명이 잘못되었습니다.");
			}
			else Fail(L"지원하지 않는 GLB 이미지 형식입니다.");
			// 디코더가 문서의 BIN 데이터를 직접 읽게 하여 압축 이미지 복제를 피한다.
			bytes = data;
			byteCount = view.length;
			return true;
		}
		catch (const GlbError& failure) { error = failure.message; }
		return false;
	}

	bool GlbDocument::ReadGeometry(uint32_t mesh, uint32_t primitive,
		GlbGeometry& out, std::wstring& error) const
	{
		error.clear();
		try
		{
			if (mesh >= meshes.size() || primitive >= meshes[mesh].primitives.size())
				Fail(L"GLB 메시 또는 primitive 인덱스가 잘못되었습니다.");
			const GlbPrimitiveInfo& info = meshes[mesh].primitives[primitive];
			const Accessor& positionAccessor = accessors[info.position];
			const size_t vertexCount = positionAccessor.count;
			const size_t indexCount = info.indices == kInvalidGlbIndex
				? vertexCount : accessors[info.indices].count;
			GlbGeometry decoded;
			// 한 정점당 64-byte GlbVertex 대신 12-byte 위치만 할당한다.
			decoded.positions.resize(vertexCount);
			decoded.indices.resize(indexCount);

			auto address = [&](const Accessor& accessor, size_t element)
			{
				const BufferView& view = views[accessor.view];
				const size_t stride = view.stride ? view.stride
					: ComponentBytes(accessor.component) * accessor.dimensions;
				return binary.data() + view.offset + accessor.offset + element * stride;
			};
			for (size_t i = 0; i < vertexCount; ++i)
			{
				const uint8_t* data = address(positionAccessor, i);
				auto& position = decoded.positions[i];
				for (size_t component = 0; component < position.size(); ++component)
				{
					const uint32_t bits = Little32(data + component * sizeof(float));
					static_assert(sizeof(float) == sizeof(bits), "GLB requires float32");
					std::memcpy(position.data() + component, &bits, sizeof(bits));
					if (!std::isfinite(position[component]))
						Fail(L"GLB 접지 정점에 NaN 또는 무한대가 있습니다.");
				}
				position[2] = -position[2];
			}

			for (size_t i = 0; i < indexCount; ++i)
			{
				uint32_t index = static_cast<uint32_t>(i);
				if (info.indices != kInvalidGlbIndex)
				{
					const Accessor& accessor = accessors[info.indices];
					const uint8_t* data = address(accessor, i);
					if (accessor.component == 5121) index = static_cast<uint32_t>(data[0]);
					else if (accessor.component == 5123) index = static_cast<uint32_t>(data[0])
						| (static_cast<uint32_t>(data[1]) << 8);
					else index = Little32(data);
				}
				if (index >= vertexCount) Fail(L"GLB 접지 인덱스가 정점 범위를 벗어났습니다.");
				decoded.indices[i] = index;
			}
			for (size_t i = 0; i < indexCount; i += 3)
				std::swap(decoded.indices[i + 1], decoded.indices[i + 2]);
			out = std::move(decoded);
			return true;
		}
		catch (const GlbError& failure) { error = failure.message; }
		catch (const std::bad_alloc&) { error = L"GLB 접지 데이터를 디코딩할 메모리를 할당하지 못했습니다."; }
		catch (const std::exception&) { error = L"GLB 접지 데이터를 디코딩하지 못했습니다."; }
		return false;
	}

	bool GlbDocument::ReadPrimitive(uint32_t mesh, uint32_t primitive,
		GlbPrimitive& out, std::wstring& error) const
	{
		error.clear();
		try
		{
			if (mesh >= meshes.size() || primitive >= meshes[mesh].primitives.size())
				Fail(L"GLB 메시 또는 primitive 인덱스가 잘못되었습니다.");
			const GlbPrimitiveInfo& info = meshes[mesh].primitives[primitive];
			const size_t vertexCount = accessors[info.position].count;
			const size_t indexCount = info.indices == kInvalidGlbIndex
				? vertexCount : accessors[info.indices].count;
			GlbPrimitive decoded;
			decoded.material = info.material;
			// primitive마다 정확한 개수만 할당한다. 반복 노드 때문에 이 배열을 복제하지 않는다.
			decoded.vertices.resize(vertexCount);
			decoded.indices.resize(indexCount);

			auto address = [&](uint32_t index, size_t element)
			{
				const Accessor& accessor = accessors[index];
				const BufferView& view = views[accessor.view];
				const size_t stride = view.stride ? view.stride
					: ComponentBytes(accessor.component) * accessor.dimensions;
				return binary.data() + view.offset + accessor.offset + element * stride;
			};
			auto unsignedValue = [](const uint8_t* data, uint32_t component)
			{
				if (component == 5121) return static_cast<uint32_t>(data[0]);
				if (component == 5123) return static_cast<uint32_t>(data[0])
					| (static_cast<uint32_t>(data[1]) << 8);
				return Little32(data);
			};
			auto readValues = [&](uint32_t index, size_t element, float* target)
			{
				if (index == kInvalidGlbIndex) return;
				const Accessor& accessor = accessors[index];
				const uint8_t* data = address(index, element);
				const uint32_t bytes = ComponentBytes(accessor.component);
				for (uint32_t i = 0; i < accessor.dimensions; ++i)
				{
					if (accessor.component == 5126)
					{
						const uint32_t bits = Little32(data + i * bytes);
						static_assert(sizeof(float) == sizeof(bits), "GLB requires float32");
						std::memcpy(target + i, &bits, sizeof(bits));
					}
					else
					{
						const uint32_t integer = unsignedValue(data + i * bytes, accessor.component);
						target[i] = static_cast<float>(integer)
							/ (accessor.component == 5121 ? 255.0f : 65535.0f);
					}
					if (!std::isfinite(target[i])) Fail(L"GLB 정점 데이터에 NaN 또는 무한대가 있습니다.");
				}
			};

			for (size_t i = 0; i < vertexCount; ++i)
			{
				GlbVertex& vertex = decoded.vertices[i];
				readValues(info.position, i, vertex.position.data());
				readValues(info.normal, i, vertex.normal.data());
				readValues(info.uv, i, vertex.uv.data());
				readValues(info.color, i, vertex.color.data());
				readValues(info.tangent, i, vertex.tangent.data());
				vertex.position[2] = -vertex.position[2];
				if (info.normal != kInvalidGlbIndex)
				{
					vertex.normal[2] = -vertex.normal[2];
					Normalize(vertex.normal);
				}
				else vertex.normal = { 0, 0, 0 };
				for (float value : vertex.color)
					if (value < 0 || value > 1) Fail(L"GLB 정점 색상이 범위를 벗어났습니다.");
				if (info.tangent != kInvalidGlbIndex)
				{
					std::array<float, 3> direction{
						vertex.tangent[0], vertex.tangent[1], -vertex.tangent[2] };
					Normalize(direction);
					std::copy(direction.begin(), direction.end(), vertex.tangent.begin());
					if (std::abs(std::abs(vertex.tangent[3]) - 1.0f) > 1e-5f)
						Fail(L"GLB tangent handedness가 유효하지 않습니다.");
					vertex.tangent[3] = -vertex.tangent[3];
				}
			}

			for (size_t i = 0; i < indexCount; ++i)
			{
				const uint32_t index = info.indices == kInvalidGlbIndex ? static_cast<uint32_t>(i)
					: unsignedValue(address(info.indices, i), accessors[info.indices].component);
				if (index >= vertexCount) Fail(L"GLB 삼각형 인덱스가 정점 범위를 벗어났습니다.");
				decoded.indices[i] = index;
			}
			for (size_t i = 0; i < indexCount; i += 3)
				std::swap(decoded.indices[i + 1], decoded.indices[i + 2]);

			if (info.normal == kInvalidGlbIndex)
			{
				// 법선이 없는 정적 primitive는 면적 가중 법선을 한 번 계산한다.
				for (size_t i = 0; i < indexCount; i += 3)
				{
					const auto& a = decoded.vertices[decoded.indices[i]].position;
					const auto& b = decoded.vertices[decoded.indices[i + 1]].position;
					const auto& c = decoded.vertices[decoded.indices[i + 2]].position;
					const double ab[3]{ static_cast<double>(b[0]) - a[0],
						static_cast<double>(b[1]) - a[1], static_cast<double>(b[2]) - a[2] };
					const double ac[3]{ static_cast<double>(c[0]) - a[0],
						static_cast<double>(c[1]) - a[1], static_cast<double>(c[2]) - a[2] };
					const double normal[3]{ ab[1] * ac[2] - ab[2] * ac[1],
						ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
					for (size_t corner = 0; corner < 3; ++corner)
					{
						auto& sum = decoded.vertices[decoded.indices[i + corner]].normal;
						for (size_t component = 0; component < 3; ++component)
						{
							const double value = static_cast<double>(sum[component]) + normal[component];
							if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
								Fail(L"GLB 계산 법선이 범위를 벗어났습니다.");
							sum[component] = static_cast<float>(value);
						}
					}
				}
				for (GlbVertex& vertex : decoded.vertices)
				{
					const double squared = static_cast<double>(vertex.normal[0]) * vertex.normal[0]
						+ static_cast<double>(vertex.normal[1]) * vertex.normal[1]
						+ static_cast<double>(vertex.normal[2]) * vertex.normal[2];
					if (squared < 1e-24) vertex.normal = { 0, 1, 0 };
					else Normalize(vertex.normal);
				}
			}
			out = std::move(decoded);
			return true;
		}
		catch (const GlbError& failure) { error = failure.message; }
		catch (const std::bad_alloc&) { error = L"GLB primitive를 디코딩할 메모리를 할당하지 못했습니다."; }
		catch (const std::exception&) { error = L"GLB primitive를 디코딩하지 못했습니다."; }
		return false;
	}
}
