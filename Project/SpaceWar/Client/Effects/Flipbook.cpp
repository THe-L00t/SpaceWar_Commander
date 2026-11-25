#include "Flipbook.h"
#include <algorithm>
#include <cmath>
#include "Client/GRenderer.h"

namespace swc {

	bool Flipbook::Create(GRenderer& renderer, const TextureData& colorData, const TextureData* normalData,
		const FlipbookLayout& newLayout, const FlipbookSettings& newSettings)
	{
		Release(renderer);
		lastError.clear();

		// ── 칸 배치 검사 — 칸이 정수 픽셀로 나뉘고, 프레임 수가 칸 수 안이어야 한다 ──
		if (newLayout.columns == 0 || newLayout.rows == 0 || newLayout.frameCount == 0 ||
			newLayout.frameCount > newLayout.columns * newLayout.rows)
		{
			lastError = L"플립북 칸 배치가 올바르지 않습니다(열 · 행 · 프레임 수).";
			return false;
		}
		if (colorData.width == 0 || colorData.height == 0 ||
			colorData.width % newLayout.columns != 0 || colorData.height % newLayout.rows != 0)
		{
			lastError = L"플립북 색 텍스처 크기가 칸 수로 나누어떨어지지 않습니다.";
			return false;
		}
		if (colorData.arraySize != 1)
		{
			lastError = L"플립북 텍스처는 한 장(배열 아님)이어야 합니다.";
			return false;
		}
		if (normalData && (normalData->width != colorData.width || normalData->height != colorData.height ||
			normalData->arraySize != 1))
		{
			lastError = L"플립북 노멀 텍스처 크기가 색 텍스처와 다릅니다.";
			return false;
		}

		// ── GPU 로 올린다(업로드가 끝난 뒤 돌아온다) ──
		const TextureHandle newColor = renderer.CreateTexture(colorData);
		if (newColor == kInvalidTexture)
		{
			lastError = L"플립북 색 텍스처를 GPU 에 만들지 못했습니다.";
			return false;
		}
		TextureHandle newNormal = kInvalidTexture;
		if (normalData)
		{
			newNormal = renderer.CreateTexture(*normalData);
			if (newNormal == kInvalidTexture)
			{
				renderer.Release({}, { newColor }, {});
				lastError = L"플립북 노멀 텍스처를 GPU 에 만들지 못했습니다.";
				return false;
			}
		}

		color = newColor;
		normal = newNormal;
		layout = newLayout;
		settings = newSettings;
		return true;
	}

	void Flipbook::Release(GRenderer& renderer)
	{
		if (color == kInvalidTexture && normal == kInvalidTexture)
			return;
		std::vector<TextureHandle> textures;
		if (color != kInvalidTexture) textures.push_back(color);
		if (normal != kInvalidTexture) textures.push_back(normal);
		renderer.Release({}, textures, {});
		color = kInvalidTexture;
		normal = kInvalidTexture;
		layout = FlipbookLayout{};
		settings = FlipbookSettings{};
	}

	float Flipbook::FrameAt(float life01) const
	{
		if (layout.frameCount <= 1)
			return 0.0f;
		const float last = float(layout.frameCount - 1);
		// 수명 0 → 첫 프레임, 수명 1 → 마지막 프레임. 그 사이는 고르게(프레임 간격 = 수명 / (프레임 수 − 1)).
		return std::clamp(life01, 0.0f, 1.0f) * last;
	}

	ParticleFlipbook Flipbook::Gpu() const
	{
		ParticleFlipbook out;
		out.color = color;
		out.normal = normal;
		out.columns = layout.columns;
		out.rows = layout.rows;
		out.frameCount = layout.frameCount;
		return out;
	}

} // namespace swc
