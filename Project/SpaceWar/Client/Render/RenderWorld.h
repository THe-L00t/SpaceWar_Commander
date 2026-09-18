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
//  ★ 실행 위치 — 렌더 단계 소유 (「멀티스레딩 분류 명세서」 5장·7장)
//    메인은 접근하지 않는다. 렌더 추출 ④ 뒤의 렌더 쪽 복사본이다.
//    DynamicObject 는 매 프레임, StaticObject 는 바뀔 때만 갱신한다.
//  ★ 받는 입력은 프레임 패킷뿐이다 (「렌더러 수정방향」 3.2)
//    뷰 · InstanceData 배열 · 광원 · 이미터 · 데칼 · 버전 번호.
//  ★ 클라이언트에만 있다. 서버는 그리지 않는다.
//  ★ 지금은 swc::LegacyScene 이 게임 쪽 Scene 과 이 역할을 함께 하고 있다.
// ============================================================

namespace swc {

	class RenderWorld
	{
	public:
		RenderWorld();
		~RenderWorld();
	};

} // namespace swc
