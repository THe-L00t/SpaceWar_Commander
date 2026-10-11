#include "AnimationBlender.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace swc {

	namespace {

		// 키 구간 찾기 — 시간이 오름차순이라 이분 탐색이다.
		// 돌려주는 값: 왼쪽 키 번호. ratio 에 구간 내 비율(0~1)을 넣는다.
		size_t FindKey(const std::vector<float>& times, float time, float& ratio)
		{
			ratio = 0.0f;
			if (times.size() < 2) return 0;

			if (time <= times.front()) return 0;
			if (time >= times.back()) return times.size() - 1;

			const auto upper = std::upper_bound(times.begin(), times.end(), time);
			const size_t right = static_cast<size_t>(upper - times.begin());
			const size_t left = right - 1;

			const float span = times[right] - times[left];
			ratio = span > 1.0e-8f ? (time - times[left]) / span : 0.0f;
			return left;
		}

		// 채널 하나를 샘플링한다. components = 3(T·S) 또는 4(R).
		void SampleChannel(const AnimationChannelData& channel, float time, float* out, size_t components)
		{
			const size_t keyCount = channel.times.size();
			if (keyCount == 0) return;

			float ratio = 0.0f;
			const size_t left = FindKey(channel.times, time, ratio);
			const size_t right = (left + 1 < keyCount) ? left + 1 : left;

			const float* a = channel.values.data() + left * components;
			const float* b = channel.values.data() + right * components;

			if (channel.interpolation == AnimationInterpolation::Step || left == right || ratio <= 0.0f)
			{
				for (size_t i = 0; i < components; ++i) out[i] = a[i];
				return;
			}

			if (components == 4)
			{
				// 회전은 구면 보간. XMQuaternionSlerp 가 최단 경로를 고른다.
				const XMVECTOR qa = XMVectorSet(a[0], a[1], a[2], a[3]);
				const XMVECTOR qb = XMVectorSet(b[0], b[1], b[2], b[3]);
				XMFLOAT4 result;
				XMStoreFloat4(&result, XMQuaternionNormalize(XMQuaternionSlerp(qa, qb, ratio)));
				out[0] = result.x; out[1] = result.y; out[2] = result.z; out[3] = result.w;
				return;
			}

			for (size_t i = 0; i < components; ++i)
				out[i] = a[i] + (b[i] - a[i]) * ratio;
		}

	} // namespace

	void AnimationBlender::Begin(const SkeletonResource& skeleton)
	{
		const size_t count = skeleton.joints.size();
		pose.resize(count);
		accumulated.assign(count, 0.0f);
		touched.assign(count, false);

		for (size_t i = 0; i < count; ++i)
		{
			// 바인드 자세로 시작한다. 클립이 건드리지 않는 조인트는 이 값이 그대로 남는다.
			pose[i].translation = skeleton.joints[i].restTranslation;
			pose[i].rotation = skeleton.joints[i].restRotation;
			pose[i].scale = skeleton.joints[i].restScale;
		}
	}

	void AnimationBlender::Add(const SkeletonResource& skeleton, const AnimationClipData& clip,
		float time, float weight)
	{
		// Begin 과 다른 스켈레톤이 들어오면 섞지 않는다(조인트 번호가 어긋난다).
		if (weight <= 0.0f || pose.empty() || pose.size() != skeleton.joints.size()) return;

		for (const AnimationChannelData& channel : clip.channels)
		{
			if (channel.joint >= pose.size()) continue;
			const size_t joint = channel.joint;

			// 이 조인트를 처음 건드리면 바인드 자세를 0 으로 밀어내고 누적을 시작한다.
			if (!touched[joint])
			{
				touched[joint] = true;
				pose[joint].translation = { 0.0f, 0.0f, 0.0f };
				pose[joint].rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
				pose[joint].scale = { 0.0f, 0.0f, 0.0f };
				accumulated[joint] = 0.0f;
			}

			switch (channel.path)
			{
			case AnimationPath::Translation:
			{
				float value[3] = { 0.0f, 0.0f, 0.0f };
				SampleChannel(channel, time, value, 3);
				pose[joint].translation.x += value[0] * weight;
				pose[joint].translation.y += value[1] * weight;
				pose[joint].translation.z += value[2] * weight;
				break;
			}
			case AnimationPath::Scale:
			{
				float value[3] = { 1.0f, 1.0f, 1.0f };
				SampleChannel(channel, time, value, 3);
				pose[joint].scale.x += value[0] * weight;
				pose[joint].scale.y += value[1] * weight;
				pose[joint].scale.z += value[2] * weight;
				break;
			}
			case AnimationPath::Rotation:
			{
				float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				SampleChannel(channel, time, value, 4);

				// ★ 쿼터니언 부호 정렬
				//   q 와 −q 는 같은 회전인데 그냥 더하면 서로 상쇄된다.
				//   이미 쌓인 값과 내적이 음수면 뒤집어서 더한다.
				XMFLOAT4& target = pose[joint].rotation;
				const float dot = target.x * value[0] + target.y * value[1] +
					target.z * value[2] + target.w * value[3];
				const float sign = dot < 0.0f ? -1.0f : 1.0f;

				target.x += value[0] * weight * sign;
				target.y += value[1] * weight * sign;
				target.z += value[2] * weight * sign;
				target.w += value[3] * weight * sign;
				break;
			}
			}

			accumulated[joint] += weight;
		}
	}

	void AnimationBlender::End(const SkeletonResource& skeleton)
	{
		for (size_t i = 0; i < pose.size(); ++i)
		{
			if (!touched[i]) continue;

			const float total = accumulated[i];
			if (total <= 1.0e-6f)
			{
				// 가중치가 사실상 0 이면 바인드 자세로 되돌린다.
				pose[i].translation = skeleton.joints[i].restTranslation;
				pose[i].rotation = skeleton.joints[i].restRotation;
				pose[i].scale = skeleton.joints[i].restScale;
				continue;
			}

			const float inv = 1.0f / total;
			pose[i].translation.x *= inv;
			pose[i].translation.y *= inv;
			pose[i].translation.z *= inv;
			pose[i].scale.x *= inv;
			pose[i].scale.y *= inv;
			pose[i].scale.z *= inv;

			// 회전은 길이를 1 로 되돌리는 것으로 끝난다(가중 평균 + 정규화 = 근사 슬러프).
			XMVECTOR rotation = XMLoadFloat4(&pose[i].rotation);
			if (XMVectorGetX(XMVector4LengthSq(rotation)) < 1.0e-12f)
				rotation = XMLoadFloat4(&skeleton.joints[i].restRotation);
			XMStoreFloat4(&pose[i].rotation, XMQuaternionNormalize(rotation));
		}
	}

	void AnimationBlender::Resolve(const SkeletonResource& skeleton,
		std::vector<XMFLOAT4X4>& globals, std::vector<XMFLOAT4X4>& skinning) const
	{
		const size_t count = skeleton.joints.size();
		globals.resize(count);
		skinning.resize(count);

		for (size_t i = 0; i < count; ++i)
		{
			// 값으로 받는다 — 삼항의 임시에 참조를 묶으면 수명이 끊긴다.
			const JointPose joint = (i < pose.size()) ? pose[i] : JointPose{};

			// 행 벡터 규약: p' = p · S · R · T
			const XMMATRIX local =
				XMMatrixScaling(joint.scale.x, joint.scale.y, joint.scale.z) *
				XMMatrixRotationQuaternion(XMLoadFloat4(&joint.rotation)) *
				XMMatrixTranslation(joint.translation.x, joint.translation.y, joint.translation.z);

			// ★ 부모가 배열에서 먼저 오므로 한 번만 훑으면 된다(리더가 그 순서를 보장한다).
			const uint32_t parent = skeleton.joints[i].parent;
			const XMMATRIX global = (parent == kInvalidJoint)
				? local
				: local * XMLoadFloat4x4(&globals[parent]);

			XMStoreFloat4x4(&globals[i], global);
			XMStoreFloat4x4(&skinning[i],
				XMLoadFloat4x4(&skeleton.joints[i].inverseBind) * global);
		}
	}
}
