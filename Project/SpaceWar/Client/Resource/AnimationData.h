#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

// ============================================================
//  Client/Resource/AnimationData.h — 스켈레톤·애니메이션 CPU 리소스
//
//  ★ 메시와 «다른» 리소스다 (명세 §4 · §6.4.2)
//    명세 §4 는 Resource Manager 의 관리 대상을 «Mesh · Texture · Animation …» 으로 적고,
//    §6.4.2 의 DynamicObject 는 Mesh Index 와 Animation Index 를 따로 들고 있다.
//    그래서 ModelData(메시·재질)와 별개 컨테이너로 적재한다 —
//    클립 파일 하나를 여러 캐릭터가 공유하고, 캐릭터 하나가 클립 여러 개를 쓴다.
//
//  ★ 여기는 «불변 데이터» 다
//    재생 시간·가중치 같은 가변 상태는 Animator 가 들고 있다(§14 Animation Object).
//    Resource Manager 는 RAM 소유자이고 Animator 는 핸들로 읽어 간다.
// ============================================================

namespace swc {

	inline constexpr uint32_t kInvalidJoint = UINT32_MAX;

	struct JointData
	{
		std::string name;
		uint32_t    parent = kInvalidJoint;   // ★ 부모가 배열에서 먼저 온다
		DirectX::XMFLOAT4X4 localRest{
			1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // 바인드 자세 로컬 변환
		DirectX::XMFLOAT4X4 inverseBind{
			1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // 역바인드 (스키닝용)

		// ★ 바인드 자세를 TRS 로 미리 풀어 둔다 (로드 때 한 번)
		//   포즈 블렌딩은 TRS 공간에서 해야 회전이 제대로 섞인다(행렬 평균은 찌그러진다).
		//   채널이 없는 조인트는 매 프레임 이 값을 그대로 쓴다 — 행렬 분해를 반복하지 않는다.
		DirectX::XMFLOAT3 restTranslation{ 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4 restRotation{ 0.0f, 0.0f, 0.0f, 1.0f };   // 쿼터니언 xyzw
		DirectX::XMFLOAT3 restScale{ 1.0f, 1.0f, 1.0f };
	};

	struct SkeletonResource
	{
		std::string            name;
		std::vector<JointData> joints;

		bool Empty() const { return joints.empty(); }

		// 이름으로 조인트를 찾는다. 클립을 다른 스켈레톤에 붙일 때 쓴다(로드 시 1회, 런타임 아님).
		uint32_t Find(const std::string& jointName) const
		{
			for (size_t i = 0; i < joints.size(); ++i)
				if (joints[i].name == jointName) return static_cast<uint32_t>(i);
			return kInvalidJoint;
		}
	};

	enum class AnimationPath : uint8_t { Translation, Rotation, Scale };
	enum class AnimationInterpolation : uint8_t { Linear, Step };

	struct AnimationChannelData
	{
		uint32_t               joint = kInvalidJoint;
		AnimationPath          path = AnimationPath::Translation;
		AnimationInterpolation interpolation = AnimationInterpolation::Linear;

		std::vector<float> times;    // 초, 오름차순
		std::vector<float> values;   // T·S = 3개씩, R = 4개씩(쿼터니언 xyzw)
	};

	struct AnimationClipData
	{
		std::string                       name;
		float                             duration = 0.0f;   // 초
		std::vector<AnimationChannelData> channels;
	};
}
