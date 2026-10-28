#pragma once
#include "Client/UI/UI.h"

// ============================================================
//  Client/Scene/Scene.h — 아키텍처 명세서 5절 · 6.10
//
//  하나의 씬. 그 씬에 속한 GameObject(Player · OtherPlayer · NPC · StaticObject)와
//  UI 가 여기에 귀속된다. 객체는 렌더 데이터를 직접 갖지 않고 RenderObjectID 로
//  Render World 를 가리킨다 (명세 6.11).
//
//  ★ 실행 위치 — 메인 스레드 소유 (「멀티스레딩 분류 명세서」 5장·7장)
//    렌더 추출 ④ 의 원본이다. 렌더 단계는 추출한 복사본(프레임 패킷)만 본다.
//  ★ 맵 정보는 서버에게서 받아서 가진다 (2026-09-18 결정)
//    씬이 맵을 스스로 정하지 않는다. 서버가 보낸 것을 받아 들고 있을 뿐이다.
//  ★ 지금은 swc::LegacyScene 이 이 역할과 Render World 역할을 겸하고 있다.
//    노드 SoA 와 Extract 구조는 「렌더러 수정방향」 §5 대로 유지하고 소유만 나눈다.
//  ★ 씬 종류마다 이 클래스를 상속한다 (2026-10-09). 첫 상속은 LoadingScene 이다.
//    게임 씬은 아직 Engine 안에 있다(Engine.h «아직 자리로 옮기지 않은 것»).
// ============================================================

namespace swc {

	class Scene
	{
	public:
		Scene();
		virtual ~Scene();

		// 매 프레임 메인에서 부른다. 씬의 상태(UI 애니메이션 등)를 진행한다.
		virtual void Update(float dt);

		UI& GetUI() { return ui; }
		const UI& GetUI() const { return ui; }

	protected:
		UI ui;    // 명세 5절 — UI 는 Scene 에 귀속된다
	};

} // namespace swc
