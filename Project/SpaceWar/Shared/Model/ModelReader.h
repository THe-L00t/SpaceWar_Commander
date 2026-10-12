#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ModelSource.h"

// ============================================================
//  Shared/Model/ModelReader.h — 포맷 판별 + 위임 + 공통 후처리
//
//  ★ 부르는 쪽은 포맷을 모른다
//    경로만 주면 된다. 판별은 파일 앞부분의 매직과 확장자로 여기서 한다.
//    새 포맷을 지원하려면 IModelReader 를 하나 만들고 ModelReader.cpp 의
//    등록 목록에 한 줄 더하면 끝이다 — 호출부는 그대로다.
//
//  ★ 운영 포맷은 glTF 2.0 하나다 (2026-10-07)
//    스킨·애니메이션이 규격에 있어야 명세 §14 Animator 를 채울 수 있다.
//    OBJ 는 정적 소품·충돌·디버깅용으로 남는다. FBX·Assimp 는 쓰지 않는다.
//
//  사용 예
//      Shared::ModelSource source;
//      std::wstring error;
//      if (!Shared::ModelReader().Load(path, source, error)) { /* error 표시 */ }
//
//  서버는 충돌만 필요하므로:
//      Shared::ReadOptions opt;
//      opt.geometryOnly = true;     // 재질·애니메이션을 읽지 않는다
//      opt.readAnimation = false;
//      ModelReader().Load(path, source, error, opt);   // source.collision 만 쓴다
// ============================================================

namespace Shared {

	// 포맷 하나당 하나. 파일에 «적혀 있는 것» 만 꺼내고 공통 계산은 하지 않는다.
	class IModelReader
	{
	public:
		virtual ~IModelReader() = default;

		// 이 리더가 읽을 수 있는 파일인가. head 는 파일 앞부분(최대 kProbeBytes), ext 는 소문자 확장자(".obj").
		virtual bool Matches(const uint8_t* head, size_t headSize, const char* ext) const = 0;

		// 포맷 이름 (오류 메시지·로그용)
		virtual ModelFormat Format() const = 0;

		virtual bool Read(const wchar_t* path, const ReadOptions& options,
			ModelSource& out, std::wstring& error) = 0;
	};

	class ModelReader
	{
	public:
		// 판별에 쓰는 파일 앞부분 크기. glTF 의 JSON 공백까지 보려면 이 정도는 필요하다.
		static constexpr size_t kProbeBytes = 64;

		ModelReader();
		~ModelReader();

		bool Load(const wchar_t* path, ModelSource& out, std::wstring& error,
			const ReadOptions& options = {});

		// 읽지 않고 포맷만 알아본다 (도구·검증용).
		static ModelFormat Detect(const wchar_t* path);

	private:
		std::vector<std::unique_ptr<IModelReader>> readers;
	};

} // namespace Shared
