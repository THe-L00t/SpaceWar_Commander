#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "Shared/HeightmapData.h"
#include "AnimationData.h"
#include "ModelData.h"

// ============================================================
//  ResourceManager — 리소스 캐싱 + 핸들 발급 (명세 §4)
//
//  기존 결정 (문서 6곳에서 수집):
//    · 핸들만 반환, DX 은닉            (렌더러_경계_명세)
//    · 핸들 = {index, generation}      (프레임워크 분석)
//    · 해시맵 캐싱, 중복 로드 방지     (주간계획 10주차 완료 기준)
//    · 리소스 종류별 컨테이너 분리     (예상 프로그램 구조)
//    · OOP 유지                        (프레임워크 분석)
//
//  ★ 2026-10-07 — 종류를 셋으로 나눴다
//    명세 §4 의 관리 대상이 «Mesh · Texture · Animation …» 이고, §6.4.2 의 DynamicObject 는
//    Mesh Index 와 Animation Index 를 **따로** 들고 있다. 그래서 컨테이너도 따로 둔다:
//      메시·재질(ModelData) / 스켈레톤(SkeletonResource) / 애니메이션 클립(AnimationClipData)
//    glTF 파일 하나에서 셋이 같이 나올 수도 있고(캐릭터), 클립만 든 파일이 따로 올 수도 있다
//    (동작 하나당 파일 하나). 그래서 로드 API 가 «파일 하나 → 핸들 묶음» 을 돌려준다.
//
//  ★ RAM 까지가 이 클래스 몫이다 (명세 원칙 5)
//    GPU 자원은 Renderer 가 만들고, 포즈 계산은 Animator 가 한다(§14).
//    여기 담긴 것은 전부 «불변 데이터» 다 — 재생 상태는 Animator 가 들고 있다.
//
//  비동기 전용 스레드는 주간계획 10주차 항목이라 지금 만들지 않는다
//  (멀티스레딩 명세 M6: 파일 I/O 스레드 + 완료 큐 ⑥).
// ============================================================

namespace swc {

	// index 0 = 무효. 기본 생성된 핸들이 자동으로 무효가 되게 한다.
	struct HeightmapHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;
		bool Valid() const { return index != 0; }
	};

	struct ModelHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;
		bool Valid() const { return index != 0; }
	};

	struct SkeletonHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;
		bool Valid() const { return index != 0; }
	};

	struct AnimationHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;
		bool Valid() const { return index != 0; }
	};

	// 모델 파일 하나에서 나온 리소스들. 비어 있는 핸들은 그 파일에 그것이 없다는 뜻이다.
	struct ModelFileResources
	{
		ModelHandle                  model;        // 메시·재질
		SkeletonHandle               skeleton;     // 본
		std::vector<AnimationHandle> animations;   // 클립
	};

	class ResourceManager
	{
	public:
		// 같은 경로를 두 번 요청하면 캐시에서 같은 핸들을 준다.
		// 경로는 와이드 문자열 — 윈도우 경로는 원래 UTF-16 이다.
		HeightmapHandle LoadHeightmap(const wchar_t* path);

		// 파일 하나를 읽어 메시·스켈레톤·클립을 각각 등록한다.
		ModelFileResources LoadModelFile(const wchar_t* path);

		// 메시만 쓰는 호출부용. 내부적으로 LoadModelFile 을 부른다.
		ModelHandle LoadModel(const wchar_t* path);

		// 무효 핸들이면 nullptr
		const Shared::HeightmapData* Get(HeightmapHandle) const;
		const ModelData* Get(ModelHandle) const;
		const SkeletonResource* Get(SkeletonHandle) const;
		const AnimationClipData* Get(AnimationHandle) const;

		// 한 파일에 클립이 여러 개일 때 이름으로 고른다("Walk", "Run" …).
		AnimationHandle FindAnimation(const ModelFileResources&, const std::string& name) const;

		const std::wstring& LastError() const { return lastError; }
		size_t HeightmapCount() const { return heightmaps.size(); }
		size_t ModelCount() const { return models.size(); }
		size_t SkeletonCount() const { return skeletons.size(); }
		size_t AnimationCount() const { return animations.size(); }

	private:
		// 새 항목을 추가해도 기존 Get() 의 주소가 바뀌지 않게 unique_ptr 로 담는다.
		std::unordered_map<std::wstring, HeightmapHandle> pathCache;
		std::vector<Shared::HeightmapData> heightmaps;
		std::vector<uint32_t> generations;

		std::unordered_map<std::wstring, ModelFileResources> fileCache;

		std::vector<std::unique_ptr<ModelData>> models;
		std::vector<uint32_t> modelGenerations;

		std::vector<std::unique_ptr<SkeletonResource>> skeletons;
		std::vector<uint32_t> skeletonGenerations;

		std::vector<std::unique_ptr<AnimationClipData>> animations;
		std::vector<uint32_t> animationGenerations;

		std::wstring lastError;
	};
}
