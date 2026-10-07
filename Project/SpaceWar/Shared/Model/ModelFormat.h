#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================
//  Shared/Model/ModelFormat.h — 포맷 구분 + 파이프라인 상한값
//
//  ★ 2026-10-07: 자체 포맷(.swm)을 걷어냈다
//    애니메이션·스키닝은 선택이 아니라 기본 요구다 —
//    명세 §14 Animator, 멀티스레딩 명세 «100명 포즈는 서로 독립 / 스키닝은 GPU»,
//    렌더링 설계 v1 «CPU 는 애니메이션 포즈만». 그런데 .swm v1 규격은 그것을 담지 못했다.
//    운영 포맷은 **glTF 2.0 하나**로 간다: 스킨(조인트·웨이트·역바인드)과 애니메이션 클립이
//    규격에 들어 있고, 블렌더가 그대로 내보낸다.
//    OBJ 는 정적 소품·충돌 메시·디버깅용으로만 남긴다(계층·스킨·애니메이션 없음).
//    FBX 리더는 만들지 않는다 — 바이너리 FBX 는 비공개 규격이고 압축 해제까지 필요하다.
// ============================================================

namespace Shared {

	enum class ModelFormat
	{
		Unknown,
		Gltf,   // .glb / .gltf — 운영 포맷 (메시·재질·계층·스킨·애니메이션)
		Obj,    // .obj (+ .mtl) — 정적 소품·충돌·디버깅
	};

	inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

	// ── 상한값 ──────────────────────────────────────
	//  손상·부정 입력을 «읽기 전에» 걸러내는 한계다. 포맷과 무관한 파이프라인 상한이므로
	//  리더 세 개가 같은 값을 쓴다. 개수를 믿고 먼저 할당하면 메모리 폭탄이 된다.
	inline constexpr uint32_t kMaxMeshes = 4096;
	inline constexpr uint32_t kMaxMaterials = 4096;
	inline constexpr uint32_t kMaxNodes = 65536;
	inline constexpr uint32_t kMaxVertices = 16u * 1024u * 1024u;
	inline constexpr uint32_t kMaxIndices = 64u * 1024u * 1024u;
	inline constexpr uint32_t kMaxNameLength = 1024;

	// ── 스키닝·애니메이션 상한 ──────────────────────
	//  kMaxJoints 는 한 캐릭터의 본 행렬 버퍼 몫이다. 사람형 리그는 보통 60~90 개다.
	inline constexpr uint32_t kMaxJoints = 256;
	inline constexpr uint32_t kMaxAnimations = 256;
	inline constexpr uint32_t kMaxKeyframes = 1u * 1024u * 1024u;

	// 정점 하나가 영향을 받는 조인트 수. glTF 의 JOINTS_0 / WEIGHTS_0 가 4개짜리다.
	inline constexpr size_t kJointsPerVertex = 4;

} // namespace Shared
