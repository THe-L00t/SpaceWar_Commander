#pragma once
#include "../ModelReader.h"

// ============================================================
//  Shared/Model/Readers/ObjReader.h — Wavefront OBJ (+ 짝 MTL)
//
//  ★ 왜 OBJ 를 먼저 만드나 (설계안 8장 올리기 순서)
//    포맷이 제일 단순해서 «파서 → ModelBuilder → 화면» 길 자체를 먼저 세울 수 있다.
//    .swm 과 glTF 는 같은 길 위에 리더만 하나씩 더 얹는 일이 된다.
//
//  지원
//    v · vt · vn · f(삼각형·다각형 모두, 음수 인덱스 포함) · usemtl · mtllib · o · g
//    MTL: Kd · d/Tr · Ke · Ns · Pr · Pm · map_Kd · map_Bump/bump/norm · map_Pr/map_Ns · map_Pm · map_Ke
//
//  빼는 것 (설계안 5장에 적은 대로)
//    노드 계층 — OBJ 에 없다. 파일 하나가 노드 하나가 된다.
//    NURBS·스무딩 그룹(s) — 무시한다.
// ============================================================

namespace Shared {

	// 텍스트 포맷이라 매직이 없다. 확장자로만 가린다.
	class ObjReader final : public IModelReader
	{
	public:
		bool Matches(const uint8_t* head, size_t headSize, const char* ext) const override;
		ModelFormat Format() const override { return ModelFormat::Obj; }

		bool Read(const wchar_t* path, const ReadOptions& options,
			ModelSource& out, std::wstring& error) override;
	};

} // namespace Shared
