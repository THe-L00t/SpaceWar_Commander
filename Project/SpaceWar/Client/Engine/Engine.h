#pragma once

// ============================================================
//  Client/Engine/Engine.h — 아키텍처 명세서 2절
//
//  엔진의 최상위 계층. 하위 시스템(Renderer · Resource Manager · Scene Manager ·
//  Input Manager · State Manager · Animator · Sound Manager · Network · Game Timer)을
//  초기화하고, 실행 순서와 생명주기를 관리한다.
//
//  ★ 게임 로직을 직접 수행하지 않는다. 각 시스템이 돌아갈 환경만 만들어 준다.
//  ★ 실행 위치 — 메인 스레드 (「멀티스레딩 분류 명세서」 5장)
//    스레드 생성·종료 순서(9.1)와 프레임 루프를 조율한다. 잡으로 나누지 않는다.
//  ★ 지금은 선언만 있다. 실제로는 Client/main.cpp 가 이 역할을 대신하고 있고,
//    옮기는 작업은 아직 하지 않았다(도입 순서는 멀티스레딩 10장 M0~M7).
// ============================================================

namespace swc {

	class Engine
	{
	public:
		Engine();
		~Engine();
	};

} // namespace swc
