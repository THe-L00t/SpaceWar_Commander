#pragma once
#include <windows.h>

// 키보드 상태 + Raw Input 마우스 델타. WndProc 에서 HandleMessage 로 먹인다.
//
// ★ 이름을 노션 기준으로 맞췄다 (Input → InputManager, 2026-09-18). 명세서 12절.
// ★ 실행 위치 — 메인 스레드 (「멀티스레딩 분류 명세서」 5장)
//   창 메시지는 창을 만든 스레드만 받는다. 프레임 대기 직후 입력 스냅샷 ① 을 만든다.
// ★ 명세 12절의 «입력을 함수 객체로 파싱해 Class Bridge 로» 는 아직 없다.
//   지금은 PlayerController 가 이 클래스를 직접 읽는다(명세 6.6.3 위반, 정리 대상).
namespace swc {
	class InputManager
	{
	public:
		bool Initialize(HWND);
		bool HandleMessage(UINT, WPARAM, LPARAM);   // true 면 WndProc 이 0 반환하고 종료

		void BeginFrame();                          // 엣지 스냅샷 + 마우스 델타 초기화

		bool IsDown(int vk) const { return keys[vk & 0xFF]; }
		bool WasPressed(int vk) const { return keys[vk & 0xFF] && !prevKeys[vk & 0xFF]; }
		bool MouseDown(int button) const { return mouseButtons[button & 3]; }

		float MouseDeltaX() const { return mouseDeltaX; }
		float MouseDeltaY() const { return mouseDeltaY; }

		void SetCaptured(bool);                     // 커서 숨김 + 창 안에 가두기
		bool Captured() const { return captured; }

	private:
		void Clear();
		void ClipToWindow();

		HWND  hwnd = nullptr;
		bool  keys[256] = {};
		bool  prevKeys[256] = {};
		bool  mouseButtons[4] = {};
		float mouseDeltaX = 0.0f;
		float mouseDeltaY = 0.0f;
		bool  captured = false;
	};
}
