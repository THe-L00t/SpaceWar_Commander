#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <string>
#include <DirectXMath.h>
#include "InstanceData.h"
#include "Vertex.h"
#include "RayTracingParams.h"
#include "Resource/ModelData.h"

struct HWND__;
using HWND = HWND__*;

namespace swc {
	// CPU에서 확인한 래스터 제출량. GPU 실행 시간이나 실제 FPS 측정값은 아니다.
	struct RenderStats
	{
		size_t submitted = 0;
		size_t culled = 0;
		size_t drawCalls = 0;
		uint64_t triangles = 0;
	};

	class GRenderer
	{
	public:
		GRenderer();
		~GRenderer();

		bool Initialize(HWND, uint32_t, uint32_t);
		MeshHandle CreateMesh(const Vertex*, size_t, const uint32_t*, size_t);
		// GPU 리소스 등록은 Initialize 이후, BeginFrame 밖에서 수행한다.
		TextureHandle CreateTexture(const TextureData&);
		MaterialHandle CreateMaterial(const MaterialData&, const std::array<TextureHandle, kMaterialTextureCount>&);
		void BeginFrame();
		// BeginFrame마다 배경(background=true), 메인 패스를 각 한 번 호출한다.
		void Render(const RenderView&, const std::vector<InstanceData>&, const DirectX::XMFLOAT4X4*);
		void EndFrame();

		// ── 하이브리드 제어 (DX 타입 노출 없음) ──
		bool SupportsRaytracing() const;
		void SetRayTracingParams(const RayTracingParams&);
		const RayTracingParams& GetRayTracingParams() const;

		void SetSunDirection(const DirectX::XMFLOAT3&);
		void SetDebugMode(uint32_t);
		uint32_t DebugMode() const;
		const RenderStats& Stats() const;

		// 초기화 실패 원인 / 어댑터 이름 등
		const std::wstring& StatusText() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
