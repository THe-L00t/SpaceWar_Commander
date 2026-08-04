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

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
