#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string_view>
#include "Shared/HeightmapData.h"
#include "AnimationData.h"
#include "ModelData.h"
#include "Client/Resource/LoadQueue.h"
#include "Client/Resource/LoadBatch.h"

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
//  ★ 비동기 로딩 (2026-10-09) — 「멀티스레딩 분류 명세서」 5장 Resource Manager · 6장 ⑥
//    파일 읽기·파싱·디코딩은 로딩 스레드(LoadQueue)에서, 컨테이너 등록은 메인에서 한다.
//    Request* 는 LoadBatch 에 작업을 얹을 뿐이고, 묶음이 돌 때 실제로 읽는다.
//    Load* (동기) 는 그대로 둔다 — 로딩 화면 자체의 이미지처럼 «로딩 씬보다 먼저» 필요한 것에 쓴다.
//  ★ 컨테이너는 메인 스레드 소유다 (7장 «로딩 중 리소스 → ⑥ 뒤 Resource Manager(메인)»)
//    등록이 끝난 데이터는 불변이라 다른 스레드도 읽을 수 있다(7장 읽기 전용 공유 데이터).
//    그래서 원소를 unique_ptr 로 들어 주소가 바뀌지 않게 한다.
// ============================================================

namespace Shared { struct ModelSource; }

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

	// 단독 이미지(UI 등)의 RAM 사본. 모델 안의 텍스처는 ModelData 가 들고 있다.
	struct TextureDataHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;
		bool Valid() const { return index != 0; }
	};

	// RequestModelFile 의 추가 선택 (2026-10-09)
	//  같은 파싱 결과에서 렌더용 말고 다른 것도 만들 때 쓴다 — 행성 OBJ 를 한 번만 읽고
	//  렌더 메시와 접지면(Shared::PlanetSurface)을 함께 얻는다.
	struct ModelRequestOptions
	{
		// Shared::ReadOptions::collectObject 로 넘긴다. 고른 오브젝트의 면이 ModelSource::collected 에 모인다.
		std::function<bool(std::string_view)> collectObject;

		// ★ 로딩 스레드에서 파싱 직후 한 번 부른다(렌더용 변환 전). false = 실패, error 에 원인.
		//   메인 스레드 소유물(Scene·Renderer·GameObject)을 건드리면 안 된다.
		//   로딩 중 메인이 손대지 않는 대상(예: 로드 전용 PlanetSurface)만 채운다.
		//   디스크 캐시(ModelCache)에서 읽었을 때도 똑같이 불린다.
		//   이번 실행에서 이미 등록된 파일(RAM 의 fileCache)이면 파싱도 훅도 없다.
		std::function<bool(const Shared::ModelSource&, std::wstring& error)> onParsed;

		// ★ 디스크 캐시 태그 (2026-10-10) — collectObject 를 쓰면 반드시 채운다
		//   함수는 비교할 수 없어서 «어떤 규칙으로 모았는가» 를 문자열로 캐시 머리말에 적는다.
		//   collectObject 가 있는데 태그가 비어 있으면 캐시를 쓰지 않는다(잘못된 접지면을 읽지 않게).
		std::string cacheTag;
	};

	// 모델 파일 하나에서 나온 리소스들. 비어 있는 핸들은 그 파일에 그것이 없다는 뜻이다.
	struct ModelFileResources
	{
		ModelHandle                  model;        // 메시·재질
		SkeletonHandle               skeleton;     // 본
		std::vector<AnimationHandle> animations;   // 클립
		bool                         fromCache = false;   // 디스크 캐시에서 읽었는가 (확인용)
	};

	class ResourceManager
	{
	public:
		// 메인에서 받는 «다 읽었다» 알림. 실패면 무효 핸들이 온다(원인은 LastError).
		// 반환값 false = 이 로드 묶음을 실패로 끝낸다.
		using HeightmapReady = std::function<bool(HeightmapHandle)>;
		using ModelReady = std::function<bool(const ModelFileResources&)>;
		using TextureReady = std::function<bool(TextureDataHandle)>;

		~ResourceManager();

		// ── 로딩 스레드 (「멀티스레딩 분류 명세서」 9.1: 시작은 Renderer 보다 먼저, 정지는 뒤) ──
		bool StartLoader();
		void StopLoader();
		LoadQueue& Loader() { return loader; }

		// 완료 큐 ⑥ 을 비운다. 메인 스레드에서 매 프레임 한 번 부른다.
		// maxCount = 프레임당 처리 상한(업로드 예산). 남은 것은 다음 프레임으로 넘어간다.
		size_t PumpLoaded(size_t maxCount);

		// ── 하이트맵 ──────────────────────────────────────────
		// 같은 경로를 두 번 요청하면 캐시에서 같은 핸들을 준다.
		// 경로는 와이드 문자열 — 윈도우 경로는 원래 UTF-16 이다.
		HeightmapHandle LoadHeightmap(const wchar_t* path);
		void RequestHeightmap(LoadBatch& batch, const std::wstring& path, HeightmapReady onReady);

		// ── 모델 ──────────────────────────────────────────────
		// 파일 하나를 읽어 메시·스켈레톤·클립을 각각 등록한다.
		ModelFileResources LoadModelFile(const wchar_t* path);
		// 같은 일을 로딩 스레드에서 한다. 파싱은 로딩 스레드, 등록과 onReady 는 메인.
		void RequestModelFile(LoadBatch& batch, const std::wstring& path, ModelReady onReady,
			ModelRequestOptions extra = {});

		// 메시만 쓰는 호출부용. 내부적으로 LoadModelFile 을 부른다.
		ModelHandle LoadModel(const wchar_t* path);

		// GPU 업로드가 끝난 뒤 CPU 메시·텍스처 픽셀을 버린다 (client2 `c680733` 에서 가져온 기능).
		// 행성 모델은 삼각형이 수십만이라 CPU 사본을 들고 있을 이유가 없다.
		//  · 그 핸들과 Get() 포인터는 무효가 된다. 세대(generation)를 올려 막는다.
		//  · ★ 스켈레톤·애니메이션 클립은 그대로 남는다 — Animator 가 계속 읽는다.
		//  · 같은 경로를 다시 LoadModelFile 하면 파일을 다시 읽는다(캐시에서 지운다).
		void ReleaseModel(ModelHandle);

		// ── 단독 텍스처 (RAM 사본) ─────────────────────────────
		// srgb = 색 이미지면 true. UI 는 스왑체인이 UNORM 이라 false 로 읽는다.
		TextureDataHandle LoadTexture(const wchar_t* path, bool srgb);
		void RequestTexture(LoadBatch& batch, const std::wstring& path, bool srgb, TextureReady onReady);

		// GPU 로 올린 뒤 RAM 사본을 버린다. 남은 핸들의 Get 은 nullptr 가 된다.
		void Release(TextureDataHandle);

		// 무효 핸들이면 nullptr
		const Shared::HeightmapData* Get(HeightmapHandle) const;
		const ModelData* Get(ModelHandle) const;
		const SkeletonResource* Get(SkeletonHandle) const;
		const AnimationClipData* Get(AnimationHandle) const;
		const TextureData* Get(TextureDataHandle) const;

		// 한 파일에 클립이 여러 개일 때 이름으로 고른다("Walk", "Run" …).
		AnimationHandle FindAnimation(const ModelFileResources&, const std::string& name) const;

		const std::wstring& LastError() const { return lastError; }
		size_t HeightmapCount() const { return heightmaps.size(); }
		size_t ModelCount() const { return models.size(); }
		size_t SkeletonCount() const { return skeletons.size(); }
		size_t AnimationCount() const { return animations.size(); }

	private:
		// 파일 하나를 파싱한 결과 — 아직 어느 컨테이너에도 들어가지 않았다.
		// 로딩 스레드가 채우고 메인이 RegisterModelFile 로 등록한다.
		struct ParsedModelFile
		{
			std::unique_ptr<ModelData>        model;
			std::unique_ptr<SkeletonResource> skeleton;
			std::vector<AnimationClipData>    clips;
			bool                              fromCache = false;
		};

		// 모델 디스크 캐시 폴더 (%LOCALAPPDATA%\SpaceWar\cache). 못 정하면 빈 문자열 = 캐시 안 씀.
		static std::wstring ModelCacheDirectory();

		// 캐시 키 = 정규화한 절대 경로. 실패면 빈 문자열.
		static std::wstring ModelKey(const wchar_t* path, std::wstring& error);
		// 컨테이너를 건드리지 않는다 → 어느 스레드에서나 부를 수 있다.
		static bool ParseModelFile(const std::wstring& key, const ModelRequestOptions& extra,
			ParsedModelFile& out, std::wstring& error);
		// 메인 스레드 전용.
		ModelFileResources RegisterModelFile(const std::wstring& key, ParsedModelFile&& parsed);

		HeightmapHandle AddHeightmap(const std::wstring& path, Shared::HeightmapData&& data);
		TextureDataHandle AddTexture(const std::wstring& key, TextureData&& data);

		LoadQueue loader;

		// 새 항목을 추가해도 기존 Get() 의 주소가 바뀌지 않게 unique_ptr 로 담는다.
		std::unordered_map<std::wstring, HeightmapHandle> pathCache;
		std::vector<std::unique_ptr<Shared::HeightmapData>> heightmaps;
		std::vector<uint32_t> generations;

		std::unordered_map<std::wstring, ModelFileResources> fileCache;

		std::vector<std::unique_ptr<ModelData>> models;
		std::vector<uint32_t> modelGenerations;

		std::vector<std::unique_ptr<SkeletonResource>> skeletons;
		std::vector<uint32_t> skeletonGenerations;

		std::vector<std::unique_ptr<AnimationClipData>> animations;
		std::vector<uint32_t> animationGenerations;

		std::unordered_map<std::wstring, TextureDataHandle> texturePathCache;
		std::vector<std::unique_ptr<TextureData>> textures;
		std::vector<uint32_t> textureGenerations;

		std::wstring lastError;
	};
}
