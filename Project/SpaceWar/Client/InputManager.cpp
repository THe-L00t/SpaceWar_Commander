#include "InputManager.h"
#include <cstring>

namespace swc {

	bool InputManager::Initialize(HWND h)
	{
		hwnd = h;

		RAWINPUTDEVICE rid = {};
		rid.usUsagePage = 0x01;   // Generic Desktop
		rid.usUsage = 0x02;       // Mouse
		rid.dwFlags = 0;          // 포커스 있을 때만 수신
		rid.hwndTarget = hwnd;
		return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
	}

	bool InputManager::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_INPUT:
		{
			RAWINPUT raw = {};
			UINT size = sizeof(raw);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
				&raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
				return false;

			if (raw.header.dwType == RIM_TYPEMOUSE)
			{
				// 절대좌표 장치(태블릿 등)는 델타로 못 쓰므로 무시
				if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
				{
					mouseDeltaX += float(raw.data.mouse.lLastX);
					mouseDeltaY += float(raw.data.mouse.lLastY);
				}

				const USHORT flags = raw.data.mouse.usButtonFlags;
				if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)   mouseButtons[0] = true;
				if (flags & RI_MOUSE_LEFT_BUTTON_UP)     mouseButtons[0] = false;
				if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)  mouseButtons[1] = true;
				if (flags & RI_MOUSE_RIGHT_BUTTON_UP)    mouseButtons[1] = false;
				if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) mouseButtons[2] = true;
				if (flags & RI_MOUSE_MIDDLE_BUTTON_UP)   mouseButtons[2] = false;
			}
			// WM_INPUT 은 DefWindowProc 이 버퍼를 정리해야 하므로 삼키지 않는다
			return false;
		}

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			keys[wParam & 0xFF] = true;
			return false;

		case WM_KEYUP:
		case WM_SYSKEYUP:
			keys[wParam & 0xFF] = false;
			return false;

		case WM_KILLFOCUS:
			Clear();
			SetCaptured(false);
			return false;

		case WM_SIZE:
		case WM_MOVE:
			if (captured) ClipToWindow();
			return false;
		}
		return false;
	}

	void InputManager::BeginFrame()
	{
		memcpy(prevKeys, keys, sizeof(keys));
		memcpy(prevMouseButtons, mouseButtons, sizeof(mouseButtons));
		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;

		// ★ 우리 창이 앞에 있을 때만 되돌린다.
		//   다른 창을 보고 있는데 커서를 끌어오면 작업을 방해한다.
		//   (포커스를 잃으면 WM_KILLFOCUS 에서 캡처가 풀리지만, 그 사이 프레임을 위한 방어다)
		if (captured && GetForegroundWindow() == hwnd)
			CenterCursor();
	}

	void InputManager::SetCaptured(bool c)
	{
		if (captured == c) return;
		captured = c;

		if (captured)
		{
			ShowCursor(FALSE);
			ClipToWindow();
			CenterCursor();   // 잡는 순간 가운데로. 누른 자리에 커서가 남아 있으면 첫 클릭이 엉뚱하게 들어간다
		}
		else
		{
			ClipCursor(nullptr);
			ShowCursor(TRUE);
		}

		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;
	}

	void InputManager::Clear()
	{
		memset(keys, 0, sizeof(keys));
		memset(mouseButtons, 0, sizeof(mouseButtons));
		memset(prevMouseButtons, 0, sizeof(prevMouseButtons));
		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;
	}

	// 창 중앙(클라이언트 영역 기준)으로 커서를 옮긴다.
	//
	// ★ SetCursorPos 는 Raw Input 이벤트를 만들지 않는다
	//   장치가 실제로 움직인 것이 아니므로 델타가 오염되지 않는다. 그래서 매 프레임 불러도
	//   시점이 미끄러지거나 되돌아가지 않는다. (WM_MOUSEMOVE 는 생기지만 쓰지 않는다)
	void InputManager::CenterCursor()
	{
		if (!hwnd) return;

		RECT rc;
		if (!GetClientRect(hwnd, &rc)) return;
		if (rc.right <= rc.left || rc.bottom <= rc.top) return;   // 최소화 중이면 0 이 된다

		POINT center = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
		ClientToScreen(hwnd, &center);
		SetCursorPos(center.x, center.y);
	}

	void InputManager::ClipToWindow()
	{
		if (!hwnd) return;

		RECT rc;
		GetClientRect(hwnd, &rc);
		POINT lt = { rc.left, rc.top };
		POINT rb = { rc.right, rc.bottom };
		ClientToScreen(hwnd, &lt);
		ClientToScreen(hwnd, &rb);

		RECT clip = { lt.x, lt.y, rb.x, rb.y };
		ClipCursor(&clip);
	}
}
