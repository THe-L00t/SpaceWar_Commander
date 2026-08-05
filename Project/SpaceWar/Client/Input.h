#pragma once
#include <windows.h>

// 키보드 상태 + Raw Input 마우스 델타. WndProc 에서 HandleMessage 로 먹인다.
namespace swc {
	class Input
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
