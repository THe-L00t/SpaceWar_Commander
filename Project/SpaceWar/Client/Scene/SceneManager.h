#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Client/Scene/LoadingScene.h"
#include "Client/Resource/LoadBatch.h"
#include "Client/UISprite.h"

// ============================================================
//  Client/Scene/SceneManager.h — 아키텍처 명세서 5절
//
//  씬의 생성·초기화·전환·생명주기를 관리한다. 살아 있는 Scene 을 소유한다.
//
//  ★ 실행 위치 — 메인 스레드 소유 (「멀티스레딩 분류 명세서」 5장)
//  ★ 클라이언트에만 있다 (2026-09-18 결정)
//    맵 정보는 서버에게서 받아 Scene 이 들고 있으면 되고, 서버는 씬이라는 개념을
//    알 필요가 없다. 서버는 맵 정보만 가진다.
//
//  ★ 전환 규칙 (2026-10-09)
//    ChangeScene(다음 씬, 로드 묶음) 을 부르면
//      묶음이 비었거나 이미 끝났다 → 바로 다음 씬
//      아직 남았다                 → 로딩 씬을 띄우고, 묶음이 끝나는 프레임에 다음 씬으로
//    같은 씬 안에서 큰 리소스를 더 읽을 때도 ChangeScene(Active(), 묶음) 으로 로딩 씬을 쓴다.
//    묶음이 실패하면 다음 씬으로 넘어가지 않고 LoadFailed 를 세운다(호출 쪽이 처리).
//  ★ 게임 씬은 아직 Engine 이 직접 돈다 (Engine.h «아직 자리로 옮기지 않은 것»)
//    그래서 지금 이 클래스가 실제로 소유하는 씬은 LoadingScene 뿐이고, 게임은 SceneId 로만 가리킨다.
// ============================================================

namespace swc {

	class GRenderer;
	class ResourceManager;

	enum class SceneId : uint8_t
	{
		None,
		Loading,
		Game,
	};

	class SceneManager
	{
	public:
		SceneManager();
		~SceneManager();

		// 로딩 씬을 만든다. 로딩 씬 자신의 이미지는 여기서 동기로 읽는다(LoadingScene.h).
		bool Initialize(GRenderer& renderer, ResourceManager& resources,
			const std::wstring& loadingBackgroundPath, const std::wstring& loadingSpinnerPath,
			std::wstring& error);

		// 앞 묶음이 아직 도는 중이면 받지 않고 false 를 돌려준다.
		bool ChangeScene(SceneId next, std::unique_ptr<LoadBatch> batch, ResourceManager& resources);

		// 매 프레임 메인에서. 묶음의 다음 단계를 내고, 끝났으면 다음 씬으로 넘긴다.
		// ★ 완료 큐(⑥)를 비우는 것은 Engine 이 ResourceManager::PumpLoaded 로 먼저 한다.
		void Update(float dt, ResourceManager& resources);

		SceneId Active() const { return active; }
		bool IsLoading() const { return active == SceneId::Loading; }

		// 이번 프레임에 새로 들어간 씬. 한 번 꺼내면 None 이 된다.
		SceneId TakeEntered();

		bool LoadFailed() const { return loadFailed; }
		const std::wstring& LoadError() const { return loadError; }

		// 로딩 중일 때 진행률 표시용 (창 제목 등)
		const LoadBatch* CurrentBatch() const { return batch.get(); }

		// 렌더 추출 ④ 의 UI 몫 — 지금 씬의 UI 를 out 에 복사한다.
		void ExtractUI(std::vector<UISprite>& out) const;

	private:
		void Enter(SceneId id);

		LoadingScene                loading;
		SceneId                     active = SceneId::None;
		SceneId                     pending = SceneId::None;
		SceneId                     entered = SceneId::None;
		std::unique_ptr<LoadBatch>  batch;

		bool                        loadFailed = false;
		std::wstring                loadError;
	};

} // namespace swc
