#include "Input.h"
#include <cstring>

namespace swc {

	bool Input::Initialize(HWND h)
	{
		hwnd = h;

		RAWINPUTDEVICE rid = {};
		rid.usUsagePage = 0x01;   // Generic Desktop
		rid.usUsage = 0x02;       // Mouse
		rid.dwFlags = 0;          // 포커스 있을 때만 수신
		rid.hwndTarget = hwnd;
		return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
	}

	bool Input::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
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

	void Input::BeginFrame()
	{
		memcpy(prevKeys, keys, sizeof(keys));
		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;
	}

	void Input::SetCaptured(bool c)
	{
		if (captured == c) return;
		captured = c;

		if (captured)
		{
			ShowCursor(FALSE);
			ClipToWindow();
		}
		else
		{
			ClipCursor(nullptr);
			ShowCursor(TRUE);
		}

		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;
	}

	void Input::Clear()
	{
		memset(keys, 0, sizeof(keys));
		memset(mouseButtons, 0, sizeof(mouseButtons));
		mouseDeltaX = 0.0f;
		mouseDeltaY = 0.0f;
	}

	void Input::ClipToWindow()
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
