#include "Diag.h"
#include <windows.h>
#include <psapi.h>
#include <string>

#pragma comment(lib, "psapi.lib")

namespace {

	// 드라이버 푸시버퍼 1개의 크기. 덤프에서 1,985개가 정확히 이 크기로 나왔다.
	constexpr uint64_t kBlockBytes = 32ull * 1024ull * 1024ull;
	constexpr double   kSampleInterval = 1.0;

	uint64_t ToMB(uint64_t bytes)
	{
		return bytes / (1024ull * 1024ull);
	}

	FILE* OpenLogNextToExe()
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		std::wstring p(exePath);
		const size_t slash = p.find_last_of(L"\\/");
		p = (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
		p += L"diag_client.log";

		FILE* f = nullptr;
		if (_wfopen_s(&f, p.c_str(), L"a") != 0)
			return nullptr;
		return f;
	}
}

namespace swc {

	bool Diag::Initialize(const char* options)
	{
		file = OpenLogNextToExe();
		if (!file)
			return false;

		Sample();
		baseCommitBytes = commitBytes;
		lastCommitBytes = commitBytes;
		peakCommitBytes = commitBytes;

		SYSTEMTIME now = {};
		GetLocalTime(&now);

#if defined(_DEBUG)
		const char* config = "Debug";
#else
		const char* config = "Release";
#endif

		fprintf(file,
			"\n"
			"================================================================\n"
			" SpaceWar 진단 로그\n"
			" 시작 %04u-%02u-%02u %02u:%02u:%02u   PID %lu   빌드 %s\n"
			" 옵션 %s\n"
			" 시작 커밋 %llu MB\n"
			"\n"
			" 지문: 커밋이 32MB 배수로 계단식 증가하면 그것이 이 문제다.\n"
			"       작업집합은 거의 늘지 않는다 (잡아만 놓고 쓰지 않는 메모리).\n"
			"================================================================\n",
			unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
			unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond),
			static_cast<unsigned long>(GetCurrentProcessId()),
			config, options ? options : "(없음)",
			ToMB(commitBytes));
		fflush(file);

		return true;
	}

	Diag::~Diag()
	{
		if (!file)
			return;

		fprintf(file,
			"---- 종료  경과 %.0f초  최종 커밋 %llu MB  최대 %llu MB  증가 %llu MB (32MB 칸 %u개) ----\n",
			elapsed, ToMB(commitBytes), ToMB(peakCommitBytes),
			ToMB(commitBytes - baseCommitBytes), blocks32MB);
		fflush(file);
		fclose(file);
		file = nullptr;
	}

	void Diag::Sample()
	{
		PROCESS_MEMORY_COUNTERS_EX pmc = {};
		pmc.cb = sizeof(pmc);
		if (GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
		{
			commitBytes = pmc.PrivateUsage;
			workingSetBytes = pmc.WorkingSetSize;
		}
	}

	void Diag::Tick(float dt, float fps, uint32_t tlasBuilds)
	{
		elapsed += dt;
		if (elapsed < nextSampleAt)
			return;
		nextSampleAt = elapsed + kSampleInterval;

		Sample();
		if (commitBytes > peakCommitBytes)
			peakCommitBytes = commitBytes;

		const uint64_t grown = (commitBytes > baseCommitBytes) ? (commitBytes - baseCommitBytes) : 0;
		blocks32MB = static_cast<uint32_t>(grown / kBlockBytes);

		// 이번 1초 동안의 증감. 32MB 로 나눠 떨어지는지가 판별점이라 칸 수도 같이 찍는다.
		const long long deltaBytes =
			static_cast<long long>(commitBytes) - static_cast<long long>(lastCommitBytes);
		const double deltaBlocks = double(deltaBytes) / double(kBlockBytes);
		lastCommitBytes = commitBytes;

		if (!file)
			return;

		fprintf(file,
			"[%6.0fs] fps %5.1f  dt %6.1fms  커밋 %7llu MB  (%+7.1f MB = %+6.2f 칸)  "
			"작업집합 %7llu MB  누적칸 %5u  TLAS %u\n",
			elapsed, fps, dt * 1000.0f,
			ToMB(commitBytes),
			double(deltaBytes) / (1024.0 * 1024.0), deltaBlocks,
			ToMB(workingSetBytes), blocks32MB, tlasBuilds);
		fflush(file);
	}
}
