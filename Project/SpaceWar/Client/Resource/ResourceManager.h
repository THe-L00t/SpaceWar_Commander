#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "Shared/HeightmapData.h"

// ============================================================
//  ResourceManager — 리소스 캐싱 + 핸들 발급
//
//  기존 결정 (문서 6곳에서 수집):
//    · 핸들만 반환, DX 은닉            (렌더러_경계_명세)
//    · 핸들 = {index, generation}      (프레임워크 분석)
//    · 해시맵 캐싱, 중복 로드 방지     (주간계획 10주차 완료 기준)
//    · 리소스 종류별 컨테이너 분리     (예상 프로그램 구조)
//    · OOP 유지                        (프레임워크 분석)
//
//  비동기 전용 스레드는 주간계획 10주차 항목이라 지금 만들지 않는다.
//  인터페이스만 확정해두면 나중에 내부만 바꿔도 호출부가 안 깨진다.
// ============================================================

namespace swc {

	// index 0 = 무효. 기본 생성된 핸들이 자동으로 무효가 되게 한다.
	struct HeightmapHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;

		bool Valid() const { return index != 0; }
	};

	class ResourceManager
	{
	public:
		// 같은 경로를 두 번 요청하면 캐시에서 같은 핸들을 준다.
		// 경로는 와이드 문자열 — 윈도우 경로는 원래 UTF-16 이다.
		HeightmapHandle LoadHeightmap(const wchar_t* path);

		// 무효 핸들이면 nullptr
		const Shared::HeightmapData* Get(HeightmapHandle) const;

		const std::wstring& LastError() const { return lastError; }
		size_t HeightmapCount() const { return heightmaps.size(); }

	private:
		std::unordered_map<std::wstring, HeightmapHandle> pathCache;
		std::vector<Shared::HeightmapData> heightmaps;
		std::vector<uint32_t> generations;
		std::wstring lastError;
	};
}
