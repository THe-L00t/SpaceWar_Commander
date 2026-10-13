#pragma once
#include <cmath>
#include "Client/Resource/ResourceManager.h"

// ============================================================
//  Client/Animation/AnimationObject.h — 명세 §14 의 «Animation Object»
//
//  클립 하나의 **재생 상태**다. 클립 데이터(불변)는 Resource Manager 가 소유하고,
//  여기에는 핸들과 시간·속도·가중치만 둔다(명세 §4 와 §14 의 경계).
//  캐릭터 한 명이 여러 개를 동시에 돌린다 — 그것을 섞는 것이 Animation Blender 다.
// ============================================================

namespace swc {

	struct AnimationObject
	{
		AnimationHandle clip;
		float time = 0.0f;           // 재생 위치 (초)
		float speed = 1.0f;
		bool  loop = true;
		bool  active = false;

		// 블렌딩 가중치. 크로스페이드 중에는 fadeRemain 이 줄면서 weight 가 목표로 간다.
		float weight = 1.0f;
		float weightTarget = 1.0f;
		float fadeRemain = 0.0f;     // 남은 페이드 시간 (초)
		float fadeTotal = 0.0f;

		void Reset()
		{
			*this = AnimationObject{};
		}

		// 시간을 진행시킨다. 클립 길이가 0 이면 시간을 0 에 둔다(한 포즈만 있는 클립).
		void Advance(float deltaSeconds, float duration)
		{
			if (duration <= 0.0f)
			{
				time = 0.0f;
				return;
			}
			time += deltaSeconds * speed;
			if (loop)
			{
				time = std::fmod(time, duration);
				if (time < 0.0f) time += duration;   // 역재생도 감싼다
			}
			else
			{
				time = time < 0.0f ? 0.0f : (time > duration ? duration : time);
			}
		}

		// 페이드를 진행시킨다. 끝나면 weight 가 목표값에 정확히 닿는다.
		void AdvanceFade(float deltaSeconds)
		{
			if (fadeRemain <= 0.0f || fadeTotal <= 0.0f)
			{
				weight = weightTarget;
				fadeRemain = 0.0f;
				return;
			}
			fadeRemain -= deltaSeconds;
			if (fadeRemain <= 0.0f)
			{
				weight = weightTarget;
				fadeRemain = 0.0f;
				return;
			}
			// 남은 비율로 선형 보간. 시작 가중치를 따로 들지 않아도 된다.
			const float progress = 1.0f - (fadeRemain / fadeTotal);
			weight = weight + (weightTarget - weight) * progress;
		}
	};
}
