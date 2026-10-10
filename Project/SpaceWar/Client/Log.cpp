#include "Log.h"

#include <windows.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

	std::mutex                            g_mutex;
	HANDLE                                g_console = nullptr;
	std::thread::id                       g_mainThread;
	std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

}

namespace swc {

	void OpenLogConsole()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_console) return;

		g_mainThread = std::this_thread::get_id();
		if (!AllocConsole()) return;
		SetConsoleTitleW(L"SpaceWar 로그");
		g_console = GetStdHandle(STD_OUTPUT_HANDLE);
		if (g_console == INVALID_HANDLE_VALUE) g_console = nullptr;
	}

	void Log(const wchar_t* format, ...)
	{
		wchar_t body[1024];
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(body, _TRUNCATE, format, args);
		va_end(args);

		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - g_start).count();
		const wchar_t* thread = (std::this_thread::get_id() == g_mainThread) ? L"메인" : L"로딩";

		wchar_t line[1152];
		_snwprintf_s(line, _TRUNCATE, L"[%8.3fs][%s] %s\r\n", seconds, thread, body);
		const size_t length = wcslen(line);   // 잘렸어도 앞부분은 찍는다
		if (length == 0) return;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_console)
		{
			DWORD written = 0;
			WriteConsoleW(g_console, line, DWORD(length), &written, nullptr);
		}
		OutputDebugStringW(line);
	}

} // namespace swc
