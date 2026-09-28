#pragma once

// ============================================================
//  Client/Bridge/ClassBridge.h — 아키텍처 명세서 9절
//
//  시스템 사이의 직접 의존을 끊는 중간 전달 계층이다.
//      Input Manager → Class Bridge → Game Logic → GameObject
//      Input Manager → Class Bridge → Network → Server
//
//  ★ 실행 위치 — 메인 스레드 (「멀티스레딩 분류 명세서」 5장)
//    전달 계층이지 스레드 경계가 아니다. 서버로 가는 입력은 송신 큐에 넣는다.
//  ★ 클라이언트에만 있다. 입력이 클라에만 있으므로 서버는 이 계층이 필요 없다.
//  ★ 지금은 선언만 있다. 입력을 무엇으로 넘길지(이벤트 · 함수 객체)는 미정이다.
// ============================================================

namespace swc {

	class ClassBridge
	{
	public:
		ClassBridge();
		~ClassBridge();
	};

} // namespace swc
