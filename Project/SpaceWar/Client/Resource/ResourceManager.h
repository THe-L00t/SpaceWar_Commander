#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "Shared/HeightmapData.h"
#include "Client/Resource/TextureData.h"
#include "Client/Resource/LoadQueue.h"
#include "Client/Resource/LoadBatch.h"

// ============================================================
//  ResourceManager — 리소스 캐싱 + 핸들 발급
//
//  기존 결정 (문서 6곳에서 수집):
//    · 핸들만 반환, DX 은닉            (렌더러_경계_명세)
//    · 핸들 = {index, generation}      (프레임워크 분석)
//    · 해시맵 캐싱, 중복 로드 방지     (주간계획 10주차 완료 기준)
//    · 리소스 종류별 컨테이너 분리     (예상 프로그램 구조)
//    · OOP 유지                        (프레임워크 분석)
//
//  ★ 비동기 로딩 (2026-10-09) — 「멀티스레딩 분류 명세서」 5장 Resource Manager · 6장 ⑥
//    파일 읽기·디코딩은 로딩 스레드(LoadQueue)에서, 컨테이너 등록은 메인에서 한다.
//    Request* 는 LoadBatch 에 작업을 얹을 뿐이고, 묶음이 돌 때 실제로 읽는다.
//    Load* (동기) 는 로딩 화면 자체의 이미지처럼 «로딩 씬보다 먼저» 필요한 것에만 쓴다.
//  ★ 컨테이너는 메인 스레드 소유다 (7장 «로딩 중 리소스 → ⑥ 뒤 Resource Manager(메인)»)
//    등록이 끝난 데이터는 불변이라 다른 스레드도 읽을 수 있다(7장 읽기 전용 공유 데이터).
//    그래서 원소를 unique_ptr 로 들어 주소가 바뀌지 않게 한다 — 로딩 스레드가
//    지형(TerrainSampler → HeightmapData)을 읽는 동안 메인이 새 원소를 넣어도 안전하다.
//  ★ GPU 자원은 만들지 않는다 (명세서 §4·원칙 5). RAM 까지가 이 클래스의 몫이다.
// ============================================================

namespace swc {

	// index 0 = 무효. 기본 생성된 핸들이 자동으로 무효가 되게 한다.
	struct HeightmapHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;

		bool Valid() const { return index != 0; }
	};

	class ResourceManager
	{
	public:
		// 메인에서 받는 «다 읽었다» 알림. 실패면 무효 핸들이 온다(원인은 LastError).
		// 반환값 false = 이 로드 묶음을 실패로 끝낸다.
		using HeightmapReady = std::function<bool(HeightmapHandle)>;
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

		// 무효 핸들이면 nullptr
		const Shared::HeightmapData* Get(HeightmapHandle) const;

		// ── 텍스처 (RAM 사본) ───────────────────────────────────
		TextureDataHandle LoadTexture(const wchar_t* path);
		void RequestTexture(LoadBatch& batch, const std::wstring& path, TextureReady onReady);

		const TextureData* Get(TextureDataHandle) const;

		// GPU 로 올린 뒤 RAM 사본을 버린다. 남은 핸들의 Get 은 nullptr 가 된다.
		void Release(TextureDataHandle);

		const std::wstring& LastError() const { return lastError; }
		size_t HeightmapCount() const { return heightmaps.size(); }

	private:
		HeightmapHandle AddHeightmap(const std::wstring& path, Shared::HeightmapData&& data);
		TextureDataHandle AddTexture(const std::wstring& path, TextureData&& data);

		LoadQueue loader;

		std::unordered_map<std::wstring, HeightmapHandle>     pathCache;
		std::vector<std::unique_ptr<Shared::HeightmapData>>   heightmaps;
		std::vector<uint32_t>                                 generations;

		std::unordered_map<std::wstring, TextureDataHandle>   texturePathCache;
		std::vector<std::unique_ptr<TextureData>>             textures;
		std::vector<uint32_t>                                 textureGenerations;

		std::wstring lastError;
	};
}
