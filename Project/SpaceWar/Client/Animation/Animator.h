#pragma once
#include <array>
#include <vector>
#include <DirectXMath.h>
#include "AnimationBlender.h"
#include "AnimationObject.h"
#include "Client/Resource/ResourceManager.h"

// ============================================================
//  Client/Animation/Animator.h — 아키텍처 명세서 14절
//
//  Resource Manager 에서 애니메이션 리소스를 **받아** GameObject 에 적용하고,
//  렌더링에 필요한 애니메이션 데이터를 Renderer 로 넘긴다.
//
//  ★ 소유 관계 (명세 §1 · §4 · §14 · §17)
//    Resource Manager 와 Animator 는 Engine 아래 **형제**다. 클립·스켈레톤의 소유자는
//    Resource Manager(RAM, 불변 데이터)이고, Animator 는 핸들로 읽어 간다.
//    Animator 가 소유하는 것은 «재생 상태»(Animation Object)와 그 결과 포즈뿐이다.
//
//  ★ 실행 위치 — 메인 스레드, 포즈 계산은 캐릭터 범위 단위 잡
//    「멀티스레딩 분류 명세서」 §5 Animator 행 · §10 M5.
//    포즈는 게임 단계의 출력이다(총구 위치·히트박스를 게임 로직이 쓴다) → ③ 상태 커밋.
//    스키닝 행렬은 ④ 렌더 추출의 프레임 패킷에 실려 Render World 의 Animation[] 이 된다(§7).
//    인스턴스 하나가 자기 슬롯만 쓰므로 그대로 잡으로 쪼갤 수 있다(§9.3).
//
//  ★ 클라이언트에만 있다. 서버는 화면을 그리지 않는다.
//    (나중에 서버가 피격 판정을 뼈 단위로 하게 되면 그때 다시 생각한다)
// ============================================================

namespace swc {

	// 0 = 무효. 기본 생성된 값이 자동으로 무효가 되게 한다.
	using AnimationInstance = uint32_t;
	inline constexpr AnimationInstance kInvalidAnimationInstance = 0;

	class Animator
	{
	public:
		// 한 캐릭터가 동시에 섞을 수 있는 클립 수. 크로스페이드는 2개면 되고,
		// 상체·하체 레이어를 쓰면 더 필요하다(§14 Animation Blender).
		static constexpr size_t kMaxLayers = 4;

		Animator();
		~Animator();

		// 스켈레톤 하나에 붙는 재생 인스턴스를 만든다. 캐릭터 한 명 = 인스턴스 하나.
		AnimationInstance Create(SkeletonHandle skeleton);
		void Destroy(AnimationInstance instance);

		// 즉시 교체. 돌고 있던 클립은 버린다.
		bool Play(AnimationInstance instance, AnimationHandle clip, bool loop = true, float speed = 1.0f);

		// seconds 동안 가중치를 넘기며 교체한다. 0 이하면 Play 와 같다.
		bool CrossFade(AnimationInstance instance, AnimationHandle clip, float seconds,
			bool loop = true, float speed = 1.0f);

		void Stop(AnimationInstance instance);

		// 모든 인스턴스의 시간을 진행시키고 포즈를 다시 만든다.
		// ★ 리소스를 인자로 받는다 — Animator 가 Resource Manager 를 소유하지 않는다(§17 의 공급 방향).
		void Update(const ResourceManager& resources, float deltaSeconds);

		// 렌더가 쓰는 스키닝 행렬(역바인드 × 글로벌). 없으면 nullptr.
		const std::vector<DirectX::XMFLOAT4X4>* SkinningMatrices(AnimationInstance instance) const;

		// 게임 로직이 쓰는 조인트 글로벌 변환(총구·히트박스). 없으면 nullptr.
		const std::vector<DirectX::XMFLOAT4X4>* JointTransforms(AnimationInstance instance) const;

		size_t InstanceCount() const;

	private:
		struct Instance
		{
			SkeletonHandle skeleton;
			std::array<AnimationObject, kMaxLayers> layers;

			AnimationBlender blender;
			std::vector<DirectX::XMFLOAT4X4> globals;
			std::vector<DirectX::XMFLOAT4X4> skinning;

			bool alive = false;
		};

		Instance* Find(AnimationInstance instance);
		const Instance* Find(AnimationInstance instance) const;

		// index 0 은 «무효» 자리로 비워 둔다. 지운 자리는 freeList 로 재사용한다.
		std::vector<Instance>          instances;
		std::vector<AnimationInstance> freeList;
	};

} // namespace swc
