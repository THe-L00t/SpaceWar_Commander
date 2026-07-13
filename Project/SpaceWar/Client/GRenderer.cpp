#include "GRenderer.h"

// DirectX 헤더는 여기서만 (다음 단계)
// #include <d3d12.h>
// #include <dxgi1_6.h>
// #include <wrl.h>

namespace swc {
	struct GRenderer::Impl
	{
		uint32_t width = 0;
		uint32_t height = 0;
	};

	GRenderer::GRenderer()
		: impl(std::make_unique<Impl>())
	{
	}

	GRenderer::~GRenderer() = default;

	bool GRenderer::Initialize(HWND, uint32_t width, uint32_t height)
	{
		impl->width = width;
		impl->height = height;
		// TODO: 디바이스/커맨드큐/스왑체인/RTV·DSV 힙/펜스 생성
		return true;
	}

	void GRenderer::BeginFrame()
	{
		// TODO: allocator 리셋, 커맨드리스트 열기, 백버퍼 배리어, RTV 클리어
	}

	void GRenderer::Render(const RenderView&, const std::vector<RenderItem>& items, const DirectX::XMFLOAT4X4* worlds)
	{
		for (const RenderItem& it : items)
		{
			const DirectX::XMFLOAT4X4& world = worlds[it.node];
			(void)world;
			// TODO: material→PSO, world/viewProj→상수버퍼, mesh→오프셋으로 DrawIndexed
		}
	}

	void GRenderer::EndFrame()
	{
		// TODO: 배리어, 커맨드리스트 닫기, ExecuteCommandLists, Present, 펜스 대기
	}
}
