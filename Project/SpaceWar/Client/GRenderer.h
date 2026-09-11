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

	// 렌더러 기동 옵션. 메모리 폭주 원인 규명 실험용 스위치다.
	// 둘 다 재빌드 없이 실행 인자로 갈아끼울 수 있어야 실험 왕복이 짧아진다.
	struct RendererOptions
	{
		// D3D12 디버그 계층. Debug 빌드에서만 의미가 있다.
		// 덤프에서 교수님 PC 가 이걸 켠 채로 돌고 있었다 (D3D12SDKLayers.dll 로드).
		bool debugLayer = true;

		// DXR 자체를 쓰지 않는다. rtSupported 를 꺼서 가속구조 초기화 / BLAS 빌드 /
		// TLAS 재빌드 / RT 루트 파라미터 / RT 셰이더가 전부 빠진다.
		// R 키 토글(rtParams.enabled)은 TLAS 재빌드만 끄므로 이것과 다르다.
		bool forceRaster = false;
	};

	class GRenderer
	{
	public:
		GRenderer();
		~GRenderer();

		bool Initialize(HWND, uint32_t, uint32_t, const RendererOptions& = RendererOptions());
		MeshHandle CreateMesh(const Vertex*, size_t, const uint32_t*, size_t);
		void BeginFrame();
		void Render(const RenderView&, const std::vector<RenderItem>&, const DirectX::XMFLOAT4X4*);
		void EndFrame();

		// ── 하이브리드 제어 (DX 타입 노출 없음) ──
		bool SupportsRaytracing() const;
		void SetRayTracingParams(const RayTracingParams&);
		const RayTracingParams& GetRayTracingParams() const;

		// 실험 계측용 — RT 를 인자로 강제로 끈 상태인가 / TLAS 를 몇 번 빌드했나
		bool IsRasterOnly() const;
		uint32_t TlasBuildCount() const;

		void SetSunDirection(const DirectX::XMFLOAT3&);
		void SetDebugMode(uint32_t);
		uint32_t DebugMode() const;

		// 초기화 실패 원인 / 어댑터 이름 등
		const std::wstring& StatusText() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
