#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ============================================================
//  Shared/Model/Readers/JsonValue.h — glTF 2.0 전용 최소 JSON 파서
//
//  ★ 왜 직접 만드나 — 외부 라이브러리 금지 방침
//    이 저장소는 의존성을 늘리지 않는다(FBX SDK 를 걷어낸 것과 같은 이유).
//    그래서 .gltf 와 .glb 의 JSON 청크를 읽는 데 필요한 만큼으로 범위를 좁혔다.
//    읽기만 있고 쓰기·직렬화는 없다. 예외도 던지지 않는다 — 실패는 Parse 의 false + error 뿐이다.
// ============================================================

namespace Shared {

	class JsonValue
	{
	public:
		enum class Type { Null, Bool, Number, String, Array, Object };

		// 성공하면 true. 실패하면 error 에 «바이트 위치 + 사람이 읽을 이유» 를 넣는다.
		// text 는 NUL 종료가 아닐 수 있다 — 반드시 length 로 끝을 판단한다.
		static bool Parse(const char* text, size_t length, JsonValue& out, std::string& error);

		Type GetType() const;
		bool IsNull() const;
		bool IsBool() const;
		bool IsNumber() const;
		bool IsString() const;
		bool IsArray() const;
		bool IsObject() const;

		bool        AsBool(bool fallback = false) const;
		double      AsNumber(double fallback = 0.0) const;
		int32_t     AsInt(int32_t fallback = 0) const;        // 소수점은 잘라낸다
		uint32_t    AsUint(uint32_t fallback = 0) const;      // 음수·비숫자는 fallback
		const std::string& AsString() const;                  // 문자열이 아니면 빈 문자열 참조

		size_t Size() const;                                  // 배열·객체의 항목 수, 그 밖은 0
		bool   Has(const char* key) const;

		// 범위를 벗어나거나 키가 없으면 «Null 값» 을 돌려준다. 절대 터지지 않는다.
		const JsonValue& operator[](size_t index) const;
		const JsonValue& operator[](const char* key) const;

		// ★ 리터럴 0 에는 이것을 쓴다 (2026-10-07)
		//   `value[0]` 의 0 은 널 포인터 상수이기도 해서 위 두 개가 모호해진다(C2666).
		//   정수 오버로드를 더하면 이번엔 uint32_t 첨자가 모호해지므로, 이름을 따로 둔다.
		const JsonValue& At(size_t index) const { return (*this)[index]; }

		const std::vector<JsonValue>& Elements() const;                                  // 배열이 아니면 빈 벡터
		const std::vector<std::pair<std::string, JsonValue>>& Members() const;           // 객체가 아니면 빈 벡터

	private:
		// 파서 본체는 .cpp 에만 있다. 중첩 클래스라 아래 멤버에 바로 닿는다.
		class Parser;

		Type        m_type = Type::Null;
		bool        m_bool = false;
		double      m_number = 0.0;
		std::string m_string;

		// ★ 중첩을 포인터가 아니라 vector 로 담는다 — 복사·이동·소멸이 기본 구현만으로 맞다.
		std::vector<JsonValue>                         m_elements;
		std::vector<std::pair<std::string, JsonValue>> m_members;
	};

} // namespace Shared
