#pragma once
#include <vector>
#include <DirectXMath.h>
#include "Client/Resource/AnimationData.h"

// ============================================================
//  Client/Animation/AnimationBlender.h — 명세 §14 의 «Animation Blender»
//
//  여러 애니메이션 상태를 섞어 **포즈 하나**를 만든다. 전환(크로스페이드)이 여기서 일어난다.
//
//  ★ 포즈는 행렬이 아니라 TRS 로 섞는다
//    회전은 쿼터니언으로 섞어야 한다. 행렬을 가중 평균하면 중간 자세가 찌그러진다.
//    그래서 섞는 동안은 (이동·회전·배율)로 들고, 마지막에 한 번만 행렬로 만든다.
//
//  ★ 쓰는 순서
//    Begin(skeleton) → Add(clip, time, weight) × N → End() → Resolve(globals, skinning)
//    Add 를 하나도 안 하면 바인드 자세(rest)가 그대로 나온다.
// ============================================================

namespace swc {

	struct JointPose
	{
		DirectX::XMFLOAT3 translation{ 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };   // 쿼터니언 xyzw
		DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
	};

	class AnimationBlender
	{
	public:
		// 바인드 자세로 초기화한다. 조인트 수가 바뀌면 버퍼를 다시 잡는다.
		void Begin(const SkeletonResource& skeleton);

		// 클립을 time 위치에서 샘플링해 weight 로 누적한다. weight <= 0 이면 무시한다.
		void Add(const SkeletonResource& skeleton, const AnimationClipData& clip,
			float time, float weight);

		// 누적 가중치로 정규화한다. 가중치가 0 인 조인트는 바인드 자세로 남는다.
		void End(const SkeletonResource& skeleton);

		const std::vector<JointPose>& Pose() const { return pose; }

		// 포즈 → 조인트 글로벌 변환 + 스키닝 행렬(역바인드 × 글로벌).
		//  globals 는 게임 로직이 쓴다(총구 위치·히트박스 — 멀티스레딩 명세 3.2).
		//  skinning 은 렌더가 쓴다(GPU 스키닝).
		void Resolve(const SkeletonResource& skeleton,
			std::vector<DirectX::XMFLOAT4X4>& globals,
			std::vector<DirectX::XMFLOAT4X4>& skinning) const;

	private:
		std::vector<JointPose> pose;
		std::vector<float>     accumulated;   // 조인트별 누적 가중치
		std::vector<bool>      touched;       // 클립이 건드린 조인트인가
	};
}
