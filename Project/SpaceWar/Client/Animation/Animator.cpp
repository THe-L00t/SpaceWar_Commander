#include "Animator.h"

#include <algorithm>

using namespace DirectX;

namespace swc {

	Animator::Animator()
	{
		instances.resize(1);   // 0번은 «무효» 자리
	}

	Animator::~Animator() = default;

	Animator::Instance* Animator::Find(AnimationInstance instance)
	{
		if (instance == kInvalidAnimationInstance || instance >= instances.size()) return nullptr;
		Instance& found = instances[instance];
		return found.alive ? &found : nullptr;
	}

	const Animator::Instance* Animator::Find(AnimationInstance instance) const
	{
		if (instance == kInvalidAnimationInstance || instance >= instances.size()) return nullptr;
		const Instance& found = instances[instance];
		return found.alive ? &found : nullptr;
	}

	AnimationInstance Animator::Create(SkeletonHandle skeleton)
	{
		if (!skeleton.Valid()) return kInvalidAnimationInstance;

		AnimationInstance index;
		if (!freeList.empty())
		{
			index = freeList.back();
			freeList.pop_back();
			instances[index] = Instance{};
		}
		else
		{
			index = static_cast<AnimationInstance>(instances.size());
			instances.emplace_back();
		}

		instances[index].skeleton = skeleton;
		instances[index].alive = true;
		return index;
	}

	void Animator::Destroy(AnimationInstance instance)
	{
		Instance* found = Find(instance);
		if (!found) return;

		*found = Instance{};
		freeList.push_back(instance);
	}

	bool Animator::Play(AnimationInstance instance, AnimationHandle clip, bool loop, float speed)
	{
		Instance* found = Find(instance);
		if (!found || !clip.Valid()) return false;

		for (AnimationObject& layer : found->layers) layer.Reset();

		AnimationObject& layer = found->layers[0];
		layer.clip = clip;
		layer.loop = loop;
		layer.speed = speed;
		layer.weight = 1.0f;
		layer.weightTarget = 1.0f;
		layer.active = true;
		return true;
	}

	bool Animator::CrossFade(AnimationInstance instance, AnimationHandle clip, float seconds,
		bool loop, float speed)
	{
		if (seconds <= 0.0f) return Play(instance, clip, loop, speed);

		Instance* found = Find(instance);
		if (!found || !clip.Valid()) return false;

		// 돌고 있는 것들은 0 으로 빼고, 빈 자리(없으면 가중치가 가장 낮은 자리)에 새 클립을 넣는다.
		size_t slot = kMaxLayers;
		for (size_t i = 0; i < kMaxLayers; ++i)
		{
			AnimationObject& layer = found->layers[i];
			if (!layer.active)
			{
				if (slot == kMaxLayers) slot = i;
				continue;
			}
			layer.weightTarget = 0.0f;
			layer.fadeTotal = seconds;
			layer.fadeRemain = seconds;
		}
		if (slot == kMaxLayers)
		{
			// 자리가 없다 — 가중치가 가장 낮은 레이어를 내보낸다.
			size_t weakest = 0;
			for (size_t i = 1; i < kMaxLayers; ++i)
				if (found->layers[i].weight < found->layers[weakest].weight) weakest = i;
			slot = weakest;
		}

		AnimationObject& layer = found->layers[slot];
		layer.Reset();
		layer.clip = clip;
		layer.loop = loop;
		layer.speed = speed;
		layer.weight = 0.0f;
		layer.weightTarget = 1.0f;
		layer.fadeTotal = seconds;
		layer.fadeRemain = seconds;
		layer.active = true;
		return true;
	}

	void Animator::Stop(AnimationInstance instance)
	{
		Instance* found = Find(instance);
		if (!found) return;
		for (AnimationObject& layer : found->layers) layer.Reset();
	}

	void Animator::Update(const ResourceManager& resources, float deltaSeconds)
	{
		for (size_t i = 1; i < instances.size(); ++i)
		{
			Instance& instance = instances[i];
			if (!instance.alive) continue;

			const SkeletonResource* skeleton = resources.Get(instance.skeleton);
			if (!skeleton || skeleton->Empty())
			{
				instance.globals.clear();
				instance.skinning.clear();
				continue;
			}

			// ── 1) 시간·가중치 진행 ────────────────────
			for (AnimationObject& layer : instance.layers)
			{
				if (!layer.active) continue;

				const AnimationClipData* clip = resources.Get(layer.clip);
				if (!clip)
				{
					layer.Reset();          // 리소스가 사라졌다(해제된 핸들)
					continue;
				}

				layer.Advance(deltaSeconds, clip->duration);
				layer.AdvanceFade(deltaSeconds);

				// 페이드가 끝나 가중치가 0 이면 자리를 비운다.
				if (layer.fadeRemain <= 0.0f && layer.weightTarget <= 0.0f)
					layer.Reset();
			}

			// ── 2) 포즈 합성 (§14 Animation Blender) ───
			instance.blender.Begin(*skeleton);
			for (const AnimationObject& layer : instance.layers)
			{
				if (!layer.active || layer.weight <= 0.0f) continue;
				const AnimationClipData* clip = resources.Get(layer.clip);
				if (!clip) continue;
				instance.blender.Add(*skeleton, *clip, layer.time, layer.weight);
			}
			instance.blender.End(*skeleton);

			// ── 3) 행렬로 펼친다 ──────────────────────
			//  globals → 게임 로직(총구·히트박스), skinning → 렌더(GPU 스키닝)
			instance.blender.Resolve(*skeleton, instance.globals, instance.skinning);
		}
	}

	const std::vector<XMFLOAT4X4>* Animator::SkinningMatrices(AnimationInstance instance) const
	{
		const Instance* found = Find(instance);
		return (found && !found->skinning.empty()) ? &found->skinning : nullptr;
	}

	const std::vector<XMFLOAT4X4>* Animator::JointTransforms(AnimationInstance instance) const
	{
		const Instance* found = Find(instance);
		return (found && !found->globals.empty()) ? &found->globals : nullptr;
	}

	size_t Animator::InstanceCount() const
	{
		size_t count = 0;
		for (size_t i = 1; i < instances.size(); ++i)
			if (instances[i].alive) ++count;
		return count;
	}

} // namespace swc
