#pragma once

// ============================================================
//  Client/Scene/Scene.h — 아키텍처 명세서 5절 · 6.10
//
//  하나의 씬. 그 씬에 속한 GameObject(Player · OtherPlayer · NPC · StaticObject)와
//  UI 가 여기에 귀속된다. 객체는 렌더 데이터를 직접 갖지 않고 RenderObjectID 로
//  Render World 를 가리킨다 (명세 6.11).
//
//  ★ 맵 정보는 서버에게서 받아서 가진다 (2026-09-18 결정)
//    씬이 맵을 스스로 정하지 않는다. 서버가 보낸 것을 받아 들고 있을 뿐이다.
//  ★ 렌더용 평면 배열을 들고 있는 기존 swc::LegacyScene 과는 다른 것이다.
//    LegacyScene 은 명세 적용 때 RenderWorld 로 대체되어 사라진다.
// ============================================================

namespace swc {

	class Scene
	{
	public:
		Scene();
		~Scene();
	};

} // namespace swc
