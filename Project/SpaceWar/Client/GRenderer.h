#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include "RenderItem.h"
#include "Vertex.h"

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

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
