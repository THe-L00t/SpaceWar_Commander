#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Client/Scene/LoadingScene.h"
#include "Client/Scene/PlanetDebugScene.h"
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
//    그래서 지금 이 클래스가 실제로 소유하는 씬은 LoadingScene 과 예시 행성 디버그 씬 5개뿐이고,
//    게임은 SceneId 로만 가리킨다.
//  ★ 예시 행성 디버그 씬 1~5 (2026-10-28) — 숫자키 1~5 (PlanetDebugScene.h)
//    처음 고를 때는 맵을 읽는 묶음과 함께 ChangeScene 하고, 이미 읽은 행성은 묶음 없이 바로 넘어간다.
// ============================================================

namespace swc {

	class GRenderer;
	class ResourceManager;

	enum class SceneId : uint8_t
	{
		None,
		Loading,
		Game,
		// 예시 행성 디버그 씬 (2026-10-28) — 숫자키 1~5. 순서는 PlanetDebugScene::Name 과 같다.
		DebugPlanet1,
		DebugPlanet2,
		DebugPlanet3,
		DebugPlanet4,
		DebugPlanet5,
	};
	static_assert(size_t(SceneId::DebugPlanet5) - size_t(SceneId::DebugPlanet1) + 1 == PlanetDebugScene::kCount,
		"디버그 행성 씬 ID 수와 PlanetDebugScene::kCount 가 같아야 한다");

	// 디버그 행성 번호(0 ~ 4) ↔ 씬 ID
	inline SceneId DebugPlanetSceneId(size_t index)
	{
		return SceneId(uint8_t(size_t(SceneId::DebugPlanet1) + index));
	}
	inline bool IsDebugPlanetScene(SceneId id, size_t& index)
	{
		if (id < SceneId::DebugPlanet1 || id > SceneId::DebugPlanet5) return false;
		index = size_t(id) - size_t(SceneId::DebugPlanet1);
		return true;
	}

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

		// 예시 행성 디버그 씬 (index 0 ~ PlanetDebugScene::kCount - 1)
		PlanetDebugScene& DebugPlanet(size_t index) { return debugPlanets[index]; }
		const PlanetDebugScene& DebugPlanet(size_t index) const { return debugPlanets[index]; }

		// ── 예시 행성 남기기 (2026-11-17, 사용자 «이전에 있던 행성만 남기고, 더 오래된 행성은 해제하는 방식으로 해줘 우선») ──
		//  명세 5절 «씬 … 전환 및 생명주기» — 어느 행성을 내릴지는 여기서 정하고, 내리기는 Engine 이 부른다(PlanetDebugScene::Release).
		//  GPU 에 남기는 예시 행성 = 지금 행성 + 바로 전 행성. 게임 맵(0 키)은 세지 않는다(늘 남는다).
		//  ★ 나중에 «우주에서 가까운 행성 미리 로드» 로 바뀐다(사용자) — 그때 이 규칙을 갈아 끼운다.
		static constexpr size_t kKeptDebugPlanets = 2;
		// 예시 행성 index 에 들어갔다(Engine::EnterDebugPlanet). 내릴 행성을 «마지막으로 들어간 때» 로 고를 때 쓴다.
		void NoteDebugPlanetEntered(size_t index);
		// 아직 올리지 않은 next 를 읽기 전에 내릴 행성. 올라간 행성이 kKeptDebugPlanets 개 이상이면
		//  keep(지금 보이는 행성 — 없으면 아무 값)을 뺀 것 중 마지막으로 들어간 때가 가장 이른 것. 없으면 PlanetDebugScene::kCount.
		size_t DebugPlanetToRelease(size_t next, size_t keep) const;

	private:
		void Enter(SceneId id);

		LoadingScene                loading;
		std::array<PlanetDebugScene, PlanetDebugScene::kCount> debugPlanets;
		std::array<uint64_t, PlanetDebugScene::kCount> debugPlanetEnteredAt{};   // 2026-11-17 — 마지막으로 들어간 순번(0 = 아직)
		uint64_t                    debugPlanetEnterCount = 0;                      // 2026-11-17 — 들어간 횟수(순번을 매긴다)
		SceneId                     active = SceneId::None;
		SceneId                     pending = SceneId::None;
		SceneId                     entered = SceneId::None;
		std::unique_ptr<LoadBatch>  batch;

		bool                        loadFailed = false;
		std::wstring                loadError;
	};

} // namespace swc
