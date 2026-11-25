#include "ParticleTexture.h"
#include <vector>
#include "Client/GRenderer.h"

namespace swc {

	bool ParticleTexture::Create(GRenderer& renderer, const TextureData& colorData, const TextureData* normalData)
	{
		Release(renderer);
		lastError.clear();

		// ── 크기 검사 — 한 장(배열 아님), 노멀은 색과 같은 크기 ──
		if (colorData.width == 0 || colorData.height == 0)
		{
			lastError = L"파티클 텍스처 크기가 0 입니다.";
			return false;
		}
		if (colorData.arraySize != 1)
		{
			lastError = L"파티클 텍스처는 한 장(배열 아님)이어야 합니다.";
			return false;
		}
		if (normalData && (normalData->width != colorData.width || normalData->height != colorData.height ||
			normalData->arraySize != 1))
		{
			lastError = L"파티클 노멀 텍스처 크기가 색 텍스처와 다릅니다.";
			return false;
		}

		// ── GPU 로 올린다(업로드가 끝난 뒤 돌아온다) ──
		const TextureHandle newColor = renderer.CreateTexture(colorData);
		if (newColor == kInvalidTexture)
		{
			lastError = L"파티클 색 텍스처를 GPU 에 만들지 못했습니다.";
			return false;
		}
		TextureHandle newNormal = kInvalidTexture;
		if (normalData)
		{
			newNormal = renderer.CreateTexture(*normalData);
			if (newNormal == kInvalidTexture)
			{
				renderer.Release({}, { newColor }, {});
				lastError = L"파티클 노멀 텍스처를 GPU 에 만들지 못했습니다.";
				return false;
			}
		}

		color = newColor;
		normal = newNormal;
		return true;
	}

	void ParticleTexture::Release(GRenderer& renderer)
	{
		if (color == kInvalidTexture && normal == kInvalidTexture)
			return;
		std::vector<TextureHandle> textures;
		if (color != kInvalidTexture) textures.push_back(color);
		if (normal != kInvalidTexture) textures.push_back(normal);
		renderer.Release({}, textures, {});
		color = kInvalidTexture;
		normal = kInvalidTexture;
	}

	ParticleFlipbook ParticleTexture::Gpu() const
	{
		ParticleFlipbook out;   // 기본값 = 1 × 1 칸 · 1 프레임
		out.color = color;
		out.normal = normal;
		out.columns = 1;
		out.rows = 1;
		out.frameCount = 1;
		return out;
	}

} // namespace swc
