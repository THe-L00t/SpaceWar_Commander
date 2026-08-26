#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include "RenderItem.h"
#include "Vertex.h"
#include "RayTracingParams.h"

struct HWND__;
using HWND = HWND__*;

namespace swc {

	// ── 진단 정보 ────────────────────────────────────────────
	//  메모리 누수가 "시스템 메모리" 인지 "GPU 메모리" 인지 가리려면
	//  둘을 따로 봐야 한다. 그리고 디바이스가 죽은 순간을 붙잡아야
	//  왜 먹통이 됐는지 알 수 있다.
	struct DiagInfo
	{
		uint64_t privateBytes = 0;   // 프로세스 커밋 (시스템 메모리)
		uint64_t workingSet = 0;     // 실제 상주
		uint64_t vramUsed = 0;       // 전용 VRAM (DXGI LOCAL)
		uint64_t sharedUsed = 0;     // 공유 시스템 메모리 (DXGI NON_LOCAL)
		uint64_t tlasBuilds = 0;     // TLAS 재빌드 누적 횟수
		bool         deviceRemoved = false;
		std::wstring removedReason;
	};

	class GRenderer
	{
	public:
		GRenderer();
		~GRenderer();

		bool Initialize(HWND, uint32_t, uint32_t);
		MeshHandle CreateMesh(const Vertex*, size_t, const uint32_t*, size_t);
		void BeginFrame();
		void Render(const RenderView&, const std::vector<RenderItem>&, const DirectX::XMFLOAT4X4*);
		void EndFrame();

		// ── 하이브리드 제어 (DX 타입 노출 없음) ──
		bool SupportsRaytracing() const;
		void SetRayTracingParams(const RayTracingParams&);
		const RayTracingParams& GetRayTracingParams() const;

		void SetSunDirection(const DirectX::XMFLOAT3&);
		void SetDebugMode(uint32_t);
		uint32_t DebugMode() const;

		// 초기화 실패 원인 / 어댑터 이름 등
		const std::wstring& StatusText() const;

		// 진단 — 매 프레임 갱신됨
		const DiagInfo& Diagnostics() const;

		// ★ 디바이스가 죽었는가.
		//   죽은 뒤에도 렌더 루프를 계속 돌리면 화면은 검은 채로
		//   메모리만 폭주한다. 호출자는 이 값이 true 면 렌더를 멈춰야 한다.
		bool IsDeviceLost() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
