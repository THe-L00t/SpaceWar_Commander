#pragma once

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
// ============================================================

namespace swc {

	class Scene
	{
	public:
		Scene();
		~Scene();
	};

} // namespace swc
