#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// ============================================================
//  Client/Resource/TextureData.h — 텍스처의 RAM 사본 (아키텍처 명세서 §4)
//
//  Resource Manager 가 RAM 에 들고 있는 형태. GPU 텍스처는 Renderer 가 이것을 받아 만든다.
//      Disk → (로딩 스레드) 디코딩 → TextureData → (메인) Renderer::CreateTexture → GPU
//
//  ★ 픽셀 형식은 RGBA8, «미리 곱한 알파(premultiplied)» 다
//    밉을 줄일 때 투명한 픽셀의 색(대개 흰색)이 섞여 가장자리에 테가 생기는 것을 막는다.
//    렌더러는 이 전제로 ONE / INV_SRC_ALPHA 로 합성한다.
//  ★ 밉 체인은 로딩 스레드에서 미리 만든다(박스 필터). 스피너처럼 원본보다
//    훨씬 작게 그리는 이미지가 밉 없이 축소되면 반짝거린다.
// ============================================================

namespace swc {

	struct TextureData
	{
		struct Mip
		{
			uint32_t width = 0;
			uint32_t height = 0;
			size_t   offset = 0;     // pixels 안의 시작 바이트. 행 간격 = width * 4
		};

		uint32_t             width = 0;
		uint32_t             height = 0;
		std::vector<Mip>     mips;      // [0] = 원본 크기
		std::vector<uint8_t> pixels;    // 모든 밉을 이어 붙인 RGBA8 (premultiplied)

		bool Valid() const { return width != 0 && height != 0 && !mips.empty(); }
	};

	// index 0 = 무효. ResourceManager 의 다른 핸들과 같은 모양이다.
	struct TextureDataHandle
	{
		uint32_t index = 0;
		uint32_t generation = 0;

		bool Valid() const { return index != 0; }
	};

} // namespace swc
