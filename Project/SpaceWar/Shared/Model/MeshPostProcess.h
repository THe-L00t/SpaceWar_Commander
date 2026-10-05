#pragma once
#include "ModelSource.h"

// ============================================================
//  Shared/Model/MeshPostProcess.h — 세 포맷이 공유하는 후처리
//
//  포맷별 리더는 «파일에 적힌 것» 만 꺼낸다. 아래는 포맷과 무관한 공통 계산이라
//  한 곳에 모은다. FBX SDK 를 쓸 때 우리가 직접 하던 일과 같은 것들이다
//  (탄젠트 생성·바운드 계산 — 옛 FbxModelLoader 의 MakeTangents·IncludePoint).
//
//  ★ 좌표 공간 규약
//    메시 정점은 «노드 로컬» 공간이다. 노드가 계층 변환을 들고 있고,
//    Scene 에 올릴 때 그 계층이 그대로 재현된다(Client 의 Model::Instantiate).
//    따라서 바운드와 충돌 삼각형은 계층을 훑어 모델 공간으로 모아야 한다.
// ============================================================

namespace Shared {

	// 부모가 자기보다 앞에 있는지, 메시·재질 인덱스가 범위 안인지 검사한다.
	// 리더가 무엇을 채웠든 이 검사를 통과해야 다음 단계로 넘긴다.
	bool ValidateModelSource(const ModelSource& source, std::wstring& error);

	// 법선이 0 인 정점에 면 법선을 넣는다. 포맷에 법선이 없을 때만 의미가 있다.
	void GenerateNormals(SourceMesh& mesh);

	// UV 기준 탄젠트. 노멀맵을 쓰려면 필요하다.
	// 삼각형마다 구해 정점에 누적하고 마지막에 그람-슈미트로 직교화한다.
	void GenerateTangents(SourceMesh& mesh);

	// 노드 계층을 훑어 모델 공간 바운드를 구한다.
	// generateCollision 이면 충돌 섹션이 비어 있을 때 렌더 삼각형으로 채운다.
	void BuildBoundsAndCollision(ModelSource& source, bool generateCollision);

} // namespace Shared
