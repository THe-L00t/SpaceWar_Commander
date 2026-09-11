#pragma once
#include <cstdint>
#include <cstdio>

// 메모리 폭주 계측.
// 교수님 PC 증상의 지문은 "커밋(PrivateUsage)이 32MB 단위로 계단식 증가" 다.
// 덤프 실측: 32MB 블록 1,985개 = 62GB, 작업집합은 3.2GB 뿐이었다.
// 즉 작업집합이 아니라 커밋을 봐야 하고, 증가분이 32MB 의 배수인지가 판별점이다.
//
// 프로세스가 멈춰 선 채로 강제 종료될 수 있으므로 한 줄 쓸 때마다 flush 한다.
namespace swc {
	class Diag
	{
	public:
		~Diag();

		// exe 옆 diag_client.log 에 이어 쓴다. options 는 헤더에 남길 실행 옵션 문자열.
		bool Initialize(const char* options);

		// 매 프레임 호출. 내부에서 1초에 한 번만 기록한다.
		void Tick(float dt, float fps, uint32_t tlasBuilds);

		uint64_t CommitBytes() const { return commitBytes; }
		uint64_t WorkingSetBytes() const { return workingSetBytes; }

		// 시작 시점 대비 커밋 증가량이 32MB 블록 몇 개분인가. 이것이 지문이다.
		uint32_t Blocks32MB() const { return blocks32MB; }

	private:
		void Sample();

		FILE*    file = nullptr;
		double   elapsed = 0.0;
		double   nextSampleAt = 0.0;
		uint64_t commitBytes = 0;
		uint64_t workingSetBytes = 0;
		uint64_t baseCommitBytes = 0;
		uint64_t lastCommitBytes = 0;
		uint64_t peakCommitBytes = 0;
		uint32_t blocks32MB = 0;
	};
}
