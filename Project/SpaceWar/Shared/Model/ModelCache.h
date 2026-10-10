#pragma once
#include <cstdint>
#include <string>
#include "ModelSource.h"

// ============================================================
//  Shared/Model/ModelCache.h — 파싱 결과를 «메모리 배치 그대로» 저장해 두는 캐시 (2026-10-10)
//
//  첫 실행: 원본(OBJ·glTF)을 파싱하고 후처리까지 끝낸 ModelSource 를 이 파일로 쓴다.
//  다음부터: 원본을 파싱하지 않고 이 파일을 정확한 크기의 배열에 한 번에 읽는다.
//      → 텍스트 버퍼·원본 v/vt/vn 배열·정점 중복 제거 해시·용량 여분·법선/탄젠트 계산이 전부 없다.
//        로드 중 피크 = 최종 데이터 크기(행성 약 0.64GiB). 비교: 노션 「행성 리소스 로드 방식 비교」.
//
//  ★ 무효화 — 아래 하나라도 다르면 캐시를 버리고 원본을 다시 파싱한다(그리고 새로 쓴다)
//    · 원본 파일 크기·수정 시각
//    · 읽기 옵션(geometryOnly·generateCollision·generateTangents·readAnimation·splitByObject)
//    · 태그(collectObject 처럼 함수라 비교할 수 없는 선택을 호출 쪽이 문자열로 적은 것)
//    · kModelCacheVersion · sizeof(Vertex) · sizeof(SkinVertex)
//  ★ kModelCacheVersion 은 «결과가 달라지는» 변경마다 올린다
//    ModelSource·Vertex 구조, 리더의 파싱 규칙, 후처리(법선·탄젠트·바운드) 규칙이 바뀌면 올린다.
//    올리지 않으면 예전 결과를 그대로 읽는다 — 머리말의 sizeof 검사는 일부만 잡아 준다.
//  ★ 캐시 폴더는 호출 쪽이 정한다(Shared 는 OS 폴더 규칙을 모른다). 클라는 %LOCALAPPDATA%\SpaceWar\cache.
//  ★ 쓰기는 임시 파일에 쓴 뒤 이름을 바꾼다 — 쓰다 끊겨도 반쪽 캐시가 남지 않는다.
//  ★ 같은 기계 전용이다(바이트 순서·구조체 배치를 그대로 쓴다). 배포용 bake(AssetBaker)도 이 형식을 쓴다.
// ============================================================

namespace Shared {

	// 결과가 달라지는 변경을 하면 올린다(위 주석).
	inline constexpr uint32_t kModelCacheVersion = 1;

	struct ModelCacheKey
	{
		std::wstring sourcePath;     // 정규화한 절대 경로
		uint64_t     sourceSize = 0;
		int64_t      sourceTime = 0; // 마지막 수정 시각(파일 시스템 시계의 틱)
		uint32_t     optionFlags = 0;
		std::string  tag;
	};

	// 원본 파일의 크기·수정 시각을 읽어 키를 만든다. 파일이 없으면 false.
	bool MakeModelCacheKey(const std::wstring& sourcePath, const ReadOptions& options,
		const std::string& tag, ModelCacheKey& out);

	// 캐시 폴더 안의 파일 이름. 원본 파일 이름 + (경로·옵션·태그) 해시 — 같은 파일을 다른 옵션으로 읽어도 겹치지 않는다.
	std::wstring ModelCacheFileName(const ModelCacheKey& key);

	// 캐시가 있고 키가 맞으면 out 을 채우고 true. 없거나 낡았거나 깨졌으면 false(out 은 비운다).
	bool ReadModelCache(const std::wstring& cachePath, const ModelCacheKey& key, ModelSource& out);

	// 캐시를 쓴다. 실패해도 게임은 계속된다(다음 실행에서 다시 파싱할 뿐이다).
	bool WriteModelCache(const std::wstring& cachePath, const ModelCacheKey& key,
		const ModelSource& source, std::wstring& error);

} // namespace Shared
