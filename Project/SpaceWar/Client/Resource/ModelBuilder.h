#pragma once
#include <string>
#include <vector>
#include "AnimationData.h"
#include "ModelData.h"
#include "Shared/Model/ModelSource.h"

// ============================================================
//  Client/Resource/ModelBuilder.h — ModelSource → CPU 리소스 3종
//
//  ★ 여기가 Shared 와 DirectX 의 경계다
//    파서(Shared)는 DirectXMath 를 모르고 텍스처 픽셀도 담지 않는다.
//    그 둘을 붙이는 일만 이 파일이 한다 — 재질·행렬 타입 올리기 + WIC 로 텍스처 읽기.
//    정점은 변환하지 않는다 — 파서가 처음부터 렌더 배치(Shared::Vertex)로 만든다(2026-10-09).
//    `ModelData` 이후(Model·GRenderer·Forward.hlsl)는 한 줄도 바뀌지 않는다.
//
//  ★ 파일 하나에서 리소스가 셋 나온다 (명세 §4)
//    메시·재질(ModelData) / 스켈레톤(SkeletonResource) / 애니메이션 클립(AnimationClipData).
//    Resource Manager 가 각각 다른 컨테이너에 담고 다른 핸들을 준다.
//    클립만 든 파일이면 ModelData 가 비어 있고, 정적 소품이면 스켈레톤·클립이 비어 있다.
//
//  ★ CoInitializeEx 가 먼저 필요하다 (TextureLoader 가 WIC = COM 이다).
// ============================================================

namespace swc {

	// 실패하면 error 에 사람이 읽을 문장을 넣고 false. 예외는 던지지 않는다.
	// ★ source 의 메시 배열(정점·인덱스·스킨)을 «넘겨받는다» — 끝나면 source.meshes 의 배열은 비어 있다.
	//   파서가 이미 렌더 배치로 만들었으므로 복사하지 않는다(2026-10-09). 스켈레톤·클립은 건드리지 않는다.
	bool BuildModelData(Shared::ModelSource& source, ModelData& out, std::wstring& error);

	// 스킨이 없으면 out 이 빈 스켈레톤으로 남는다(실패가 아니다).
	void BuildSkeleton(const Shared::ModelSource& source, SkeletonResource& out);

	// 클립이 없으면 out 이 빈 배열로 남는다(실패가 아니다).
	void BuildAnimationClips(const Shared::ModelSource& source, std::vector<AnimationClipData>& out);
}
