#include "JsonValue.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Shared {

	namespace {

		// ── 글자 분류 ───────────────────────────────────────
		//  JSON 의 공백은 이 네 글자뿐이다. 주석(// , /* */)은 JSON 이 아니므로 받지 않는다.
		inline bool IsJsonSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

		int HexDigit(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		inline constexpr uint32_t kReplacementChar = 0xFFFDu;   // 짝 없는 서로게이트가 들어갈 자리
		inline constexpr int      kMaxDepth = 64;               // ★ 손상·악성 입력의 재귀 폭주를 막는 상한
		inline constexpr size_t   kNumberBufferSize = 64;       // ★ strtod 에 넘길 숫자 토큰 복사용

		// 코드포인트 하나를 UTF-8 바이트로 붙인다.
		void AppendUtf8(uint32_t cp, std::string& out)
		{
			if (cp < 0x80u)
			{
				out.push_back(static_cast<char>(cp));
			}
			else if (cp < 0x800u)
			{
				out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
				out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
			}
			else if (cp < 0x10000u)
			{
				out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
				out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
				out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
			}
		}

		// ── 비어 있는 자리를 돌려줄 값들 ────────────────────
		//  ★ 함수 지역 static 하나로 둔다 — 초기화가 스레드 안전하고(magic static),
		//    없는 자리를 받은 호출부가 길이·키 검사를 일일이 하지 않아도 된다.
		const JsonValue& NullValue()
		{
			static const JsonValue kNull;
			return kNull;
		}

		const std::string& EmptyString()
		{
			static const std::string kEmpty;
			return kEmpty;
		}

		const std::vector<JsonValue>& EmptyElements()
		{
			static const std::vector<JsonValue> kEmpty;
			return kEmpty;
		}

		const std::vector<std::pair<std::string, JsonValue>>& EmptyMembers()
		{
			static const std::vector<std::pair<std::string, JsonValue>> kEmpty;
			return kEmpty;
		}

	} // namespace

	// ── 파서 ────────────────────────────────────────────
	class JsonValue::Parser
	{
	public:
		Parser(const char* text, size_t length)
			: m_begin(text), m_cur(text), m_end(text + length)
		{
		}

		bool Run(JsonValue& out, std::string& error)
		{
			SkipBom();

			JsonValue root;
			bool ok = ParseValue(root, 0);
			if (ok)
			{
				SkipSpaces();
				// 값 하나를 읽은 뒤에는 공백 말고 아무것도 남아 있으면 안 된다.
				if (m_cur < m_end) ok = Fail("값 하나를 읽은 뒤에 남은 데이터가 있습니다");
			}

			if (!ok)
			{
				error = std::move(m_error);
				return false;
			}

			out = std::move(root);
			error.clear();
			return true;
		}

	private:
		bool Fail(const char* reason) { return FailAt(m_cur, reason); }

		bool FailAt(const char* at, const char* reason)
		{
			const size_t offset = static_cast<size_t>(at - m_begin);
			m_error = "JSON 바이트 " + std::to_string(offset) + ": " + reason;
			return false;
		}

		// 일부 툴이 .gltf 를 BOM 붙여 저장한다. 값 앞의 BOM 은 건너뛴다.
		void SkipBom()
		{
			if (static_cast<size_t>(m_end - m_cur) >= 3 &&
				static_cast<unsigned char>(m_cur[0]) == 0xEFu &&
				static_cast<unsigned char>(m_cur[1]) == 0xBBu &&
				static_cast<unsigned char>(m_cur[2]) == 0xBFu)
			{
				m_cur += 3;
			}
		}

		void SkipSpaces()
		{
			while (m_cur < m_end && IsJsonSpace(*m_cur)) ++m_cur;
		}

		bool Expect(const char* word)
		{
			const char* at = m_cur;
			for (const char* w = word; *w != '\0'; ++w)
			{
				if (m_cur >= m_end || *m_cur != *w) return FailAt(at, "리터럴 표기가 올바르지 않습니다");
				++m_cur;
			}
			return true;
		}

		// out 은 호출 시점에 갓 만들어진 Null 값이어야 한다.
		bool ParseValue(JsonValue& out, int depth)
		{
			SkipSpaces();
			if (m_cur >= m_end) return Fail("값이 와야 할 자리에서 입력이 끝났습니다");

			switch (*m_cur)
			{
			case '{':
				return ParseObject(out, depth);

			case '[':
				return ParseArray(out, depth);

			case '"':
			{
				std::string text;
				if (!ParseString(text)) return false;
				out.m_type = Type::String;
				out.m_string = std::move(text);
				return true;
			}

			case 't':
				if (!Expect("true")) return false;
				out.m_type = Type::Bool;
				out.m_bool = true;
				return true;

			case 'f':
				if (!Expect("false")) return false;
				out.m_type = Type::Bool;
				out.m_bool = false;
				return true;

			case 'n':
				if (!Expect("null")) return false;
				out.m_type = Type::Null;
				return true;

			default:
				if (*m_cur == '-' || IsDigit(*m_cur)) return ParseNumber(out);
				return Fail("값으로 해석할 수 없는 문자입니다");
			}
		}

		// JSON 숫자 문법을 그대로 검사한다 — «01» ·«.5» ·«1.» ·«+1» 은 모두 JSON 이 아니다.
		bool ParseNumber(JsonValue& out)
		{
			const char* start = m_cur;
			if (m_cur < m_end && *m_cur == '-') ++m_cur;

			if (m_cur >= m_end || !IsDigit(*m_cur)) return FailAt(start, "숫자에 정수부가 없습니다");
			if (*m_cur == '0')
			{
				++m_cur;
				if (m_cur < m_end && IsDigit(*m_cur)) return FailAt(start, "0 으로 시작하는 정수는 쓸 수 없습니다");
			}
			else
			{
				while (m_cur < m_end && IsDigit(*m_cur)) ++m_cur;
			}

			if (m_cur < m_end && *m_cur == '.')
			{
				++m_cur;
				if (m_cur >= m_end || !IsDigit(*m_cur)) return FailAt(start, "소수점 뒤에 숫자가 없습니다");
				while (m_cur < m_end && IsDigit(*m_cur)) ++m_cur;
			}

			if (m_cur < m_end && (*m_cur == 'e' || *m_cur == 'E'))
			{
				++m_cur;
				if (m_cur < m_end && (*m_cur == '+' || *m_cur == '-')) ++m_cur;
				if (m_cur >= m_end || !IsDigit(*m_cur)) return FailAt(start, "지수부에 숫자가 없습니다");
				while (m_cur < m_end && IsDigit(*m_cur)) ++m_cur;
			}

			const size_t length = static_cast<size_t>(m_cur - start);
			if (length >= kNumberBufferSize) return FailAt(start, "숫자 토큰이 너무 깁니다(63바이트 제한)");

			// ★ 입력이 NUL 종료라는 보장이 없다(.glb 의 JSON 청크가 그렇다).
			//   strtod 가 끝을 알 수 있게 토큰만 지역 버퍼로 복사해서 넘긴다.
			char buffer[kNumberBufferSize];
			std::memcpy(buffer, start, length);
			buffer[length] = '\0';

			out.m_type = Type::Number;
			out.m_number = std::strtod(buffer, nullptr);
			return true;
		}

		// 여는 큰따옴표 위에서 들어온다.
		bool ParseString(std::string& out)
		{
			const char* open = m_cur;
			++m_cur;
			out.clear();

			while (true)
			{
				if (m_cur >= m_end) return FailAt(open, "문자열이 닫히지 않았습니다");

				const unsigned char c = static_cast<unsigned char>(*m_cur);
				if (c == '"')
				{
					++m_cur;
					return true;
				}
				if (c < 0x20u) return Fail("문자열 안에 제어문자를 그대로 쓸 수 없습니다");
				if (c != '\\')
				{
					// ★ UTF-8 바이트는 해석하지 않고 그대로 통과시킨다(glTF 에 한글 노드 이름이 올 수 있다).
					out.push_back(*m_cur);
					++m_cur;
					continue;
				}

				++m_cur;
				if (m_cur >= m_end) return FailAt(open, "이스케이프가 끝나지 않았습니다");

				const char* escapeAt = m_cur;
				const char esc = *m_cur++;
				switch (esc)
				{
				case '"':  out.push_back('"');  break;
				case '\\': out.push_back('\\'); break;
				case '/':  out.push_back('/');  break;
				case 'b':  out.push_back('\b'); break;
				case 'f':  out.push_back('\f'); break;
				case 'n':  out.push_back('\n'); break;
				case 'r':  out.push_back('\r'); break;
				case 't':  out.push_back('\t'); break;
				case 'u':
					if (!ParseUnicodeEscape(out)) return false;
					break;
				default:
					return FailAt(escapeAt, "알 수 없는 이스케이프입니다");
				}
			}
		}

		bool ReadHex4(uint32_t& out)
		{
			if (static_cast<size_t>(m_end - m_cur) < 4) return Fail("\\u 뒤에 16진수 네 자리가 필요합니다");

			uint32_t value = 0;
			for (int i = 0; i < 4; ++i)
			{
				const int digit = HexDigit(m_cur[i]);
				if (digit < 0) return Fail("\\u 뒤에 16진수가 아닌 문자가 있습니다");
				value = (value << 4) | static_cast<uint32_t>(digit);
			}
			m_cur += 4;
			out = value;
			return true;
		}

		// \u 를 지난 자리에서 들어온다. 상위 서로게이트면 뒤따르는 \uXXXX 까지 묶어 한 코드포인트로 만든다.
		bool ParseUnicodeEscape(std::string& out)
		{
			uint32_t unit = 0;
			if (!ReadHex4(unit)) return false;

			if (unit >= 0xD800u && unit <= 0xDBFFu)
			{
				if (static_cast<size_t>(m_end - m_cur) >= 6 && m_cur[0] == '\\' && m_cur[1] == 'u')
				{
					uint32_t low = 0;
					bool valid = true;
					for (int i = 0; i < 4; ++i)
					{
						const int digit = HexDigit(m_cur[2 + i]);
						if (digit < 0)
						{
							valid = false;
							break;
						}
						low = (low << 4) | static_cast<uint32_t>(digit);
					}

					if (valid && low >= 0xDC00u && low <= 0xDFFFu)
					{
						m_cur += 6;
						AppendUtf8(0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u), out);
						return true;
					}
				}

				// ★ 짝이 안 맞는 서로게이트는 실패로 처리하지 않는다 — 이름 한 글자 때문에
				//   모델 전체를 못 읽는 쪽이 더 나쁘다. U+FFFD 로 바꿔 두고 계속 읽는다.
				AppendUtf8(kReplacementChar, out);
				return true;
			}

			if (unit >= 0xDC00u && unit <= 0xDFFFu)
			{
				AppendUtf8(kReplacementChar, out);
				return true;
			}

			AppendUtf8(unit, out);
			return true;
		}

		bool ParseArray(JsonValue& out, int depth)
		{
			if (depth >= kMaxDepth) return Fail("중첩이 64단계를 넘었습니다");

			const char* open = m_cur;
			++m_cur;
			out.m_type = Type::Array;

			SkipSpaces();
			if (m_cur < m_end && *m_cur == ']')
			{
				++m_cur;
				return true;
			}

			while (true)
			{
				out.m_elements.emplace_back();
				if (!ParseValue(out.m_elements.back(), depth + 1)) return false;

				SkipSpaces();
				if (m_cur >= m_end) return FailAt(open, "배열이 닫히지 않았습니다");
				if (*m_cur == ',')
				{
					++m_cur;
					continue;
				}
				if (*m_cur == ']')
				{
					++m_cur;
					return true;
				}
				return Fail("배열에서 ',' 또는 ']' 가 와야 합니다");
			}
		}

		bool ParseObject(JsonValue& out, int depth)
		{
			if (depth >= kMaxDepth) return Fail("중첩이 64단계를 넘었습니다");

			const char* open = m_cur;
			++m_cur;
			out.m_type = Type::Object;

			SkipSpaces();
			if (m_cur < m_end && *m_cur == '}')
			{
				++m_cur;
				return true;
			}

			while (true)
			{
				SkipSpaces();
				if (m_cur >= m_end) return FailAt(open, "객체가 닫히지 않았습니다");
				if (*m_cur != '"') return Fail("객체의 키는 문자열이어야 합니다");

				std::string key;
				if (!ParseString(key)) return false;

				SkipSpaces();
				if (m_cur >= m_end || *m_cur != ':') return Fail("객체에서 키 뒤에 ':' 가 와야 합니다");
				++m_cur;

				// ★ 같은 키가 두 번 나오면 나중 것이 이긴다. 제자리에 덮어써서
				//   Members() 를 훑는 쪽이 중복 키를 보지 않게 한다(glTF 키 수는 적어 선형 탐색으로 충분).
				JsonValue* slot = nullptr;
				for (auto& member : out.m_members)
				{
					if (member.first == key)
					{
						member.second = JsonValue();
						slot = &member.second;
						break;
					}
				}
				if (slot == nullptr)
				{
					out.m_members.emplace_back(std::move(key), JsonValue());
					slot = &out.m_members.back().second;
				}

				if (!ParseValue(*slot, depth + 1)) return false;

				SkipSpaces();
				if (m_cur >= m_end) return FailAt(open, "객체가 닫히지 않았습니다");
				if (*m_cur == ',')
				{
					++m_cur;
					continue;
				}
				if (*m_cur == '}')
				{
					++m_cur;
					return true;
				}
				return Fail("객체에서 ',' 또는 '}' 가 와야 합니다");
			}
		}

		const char* m_begin;
		const char* m_cur;
		const char* m_end;
		std::string m_error;
	};

	bool JsonValue::Parse(const char* text, size_t length, JsonValue& out, std::string& error)
	{
		out = JsonValue();

		if (text == nullptr || length == 0)
		{
			error = "JSON 입력이 비어 있습니다";
			return false;
		}

		Parser parser(text, length);
		return parser.Run(out, error);
	}

	// ── 조회 ────────────────────────────────────────────
	JsonValue::Type JsonValue::GetType() const { return m_type; }

	bool JsonValue::IsNull()   const { return m_type == Type::Null; }
	bool JsonValue::IsBool()   const { return m_type == Type::Bool; }
	bool JsonValue::IsNumber() const { return m_type == Type::Number; }
	bool JsonValue::IsString() const { return m_type == Type::String; }
	bool JsonValue::IsArray()  const { return m_type == Type::Array; }
	bool JsonValue::IsObject() const { return m_type == Type::Object; }

	bool JsonValue::AsBool(bool fallback) const
	{
		return m_type == Type::Bool ? m_bool : fallback;
	}

	double JsonValue::AsNumber(double fallback) const
	{
		return m_type == Type::Number ? m_number : fallback;
	}

	int32_t JsonValue::AsInt(int32_t fallback) const
	{
		if (m_type != Type::Number) return fallback;

		const double value = std::trunc(m_number);
		// NaN·무한·범위 밖은 비교가 모두 거짓이 되어 fallback 으로 떨어진다.
		if (!(value >= -2147483648.0 && value <= 2147483647.0)) return fallback;
		return static_cast<int32_t>(value);
	}

	uint32_t JsonValue::AsUint(uint32_t fallback) const
	{
		if (m_type != Type::Number) return fallback;

		const double value = std::trunc(m_number);
		if (!(value >= 0.0 && value <= 4294967295.0)) return fallback;
		return static_cast<uint32_t>(value);
	}

	const std::string& JsonValue::AsString() const
	{
		return m_type == Type::String ? m_string : EmptyString();
	}

	size_t JsonValue::Size() const
	{
		if (m_type == Type::Array)  return m_elements.size();
		if (m_type == Type::Object) return m_members.size();
		return 0;
	}

	bool JsonValue::Has(const char* key) const
	{
		if (m_type != Type::Object || key == nullptr) return false;

		for (const auto& member : m_members)
		{
			if (member.first == key) return true;
		}
		return false;
	}

	const JsonValue& JsonValue::operator[](size_t index) const
	{
		// 객체도 자리 번호로 받는다 — Size() 가 멤버 수를 돌려주므로 둘의 짝이 맞아야 한다.
		if (m_type == Type::Array)  return index < m_elements.size() ? m_elements[index] : NullValue();
		if (m_type == Type::Object) return index < m_members.size() ? m_members[index].second : NullValue();
		return NullValue();
	}

	const JsonValue& JsonValue::operator[](const char* key) const
	{
		if (m_type == Type::Object && key != nullptr)
		{
			for (const auto& member : m_members)
			{
				if (member.first == key) return member.second;
			}
		}
		return NullValue();
	}

	const std::vector<JsonValue>& JsonValue::Elements() const
	{
		return m_type == Type::Array ? m_elements : EmptyElements();
	}

	const std::vector<std::pair<std::string, JsonValue>>& JsonValue::Members() const
	{
		return m_type == Type::Object ? m_members : EmptyMembers();
	}

} // namespace Shared
