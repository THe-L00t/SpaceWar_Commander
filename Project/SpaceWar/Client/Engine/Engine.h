#pragma once

// ============================================================
//  Client/Engine/Engine.h — 아키텍처 명세서 2절
//
//  엔진의 최상위 계층. 하위 시스템(Renderer · Resource Manager · Scene Manager ·
//  Input Manager · State Manager · Animator · Sound Manager · Network · Game Timer)을
//  초기화하고, 실행 순서와 생명주기를 관리한다.
//
//  ★ 게임 로직을 직접 수행하지 않는다. 각 시스템이 돌아갈 환경만 만들어 준다.
//  ★ 지금은 선언만 있다. 실제로는 Client/main.cpp 가 이 역할을 대신하고 있고,
//    옮기는 작업은 아직 하지 않았다.
// ============================================================

namespace swc {

	class Engine
	{
	public:
		Engine();
		~Engine();
	};

} // namespace swc
