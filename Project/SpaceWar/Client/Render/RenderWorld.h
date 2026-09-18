#pragma once

// ============================================================
//  Client/Render/RenderWorld.h — 아키텍처 명세서 7절
//
//  렌더링에 필요한 데이터만 연속된 메모리로 들고 있는 데이터 지향 구조다.
//      Transform[] · Mesh[] · Material[] · WorldMatrix[] · Visibility[] ·
//      Animation[] · Instance[]
//  GameObject 는 렌더 데이터를 소유하지 않고 RenderObjectID 로 여기를 가리킨다
//  (명세 6.11). Renderer 는 객체 그래프 대신 이 배열들을 순회한다 (명세 18절 원칙 6).
//
//  ★ 클라이언트에만 있다. 서버는 그리지 않는다.
//  ★ 지금 이 역할을 하는 것은 swc::LegacyScene 이다. 구조가 많이 달라질 예정이라
//    LegacyScene 을 그대로 두고, 명세를 적용할 때 이 클래스로 대체한다.
// ============================================================

namespace swc {

	class RenderWorld
	{
	public:
		RenderWorld();
		~RenderWorld();
	};

} // namespace swc
