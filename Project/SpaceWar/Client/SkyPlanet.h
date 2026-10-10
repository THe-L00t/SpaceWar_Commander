#pragma once
#include <array>
#include <string>
#include <vector>
#include "Model.h"
#include "LegacyScene.h"

namespace swc {

	class Camera;
	class GRenderer;
	class ResourceManager;

	// 하늘의 표시용 행성. 이동·중력·충돌 맵과 별도로 LOD 노드를 소유한다.
	class SkyPlanet
	{
	public:
		bool Initialize(ResourceManager&, GRenderer&, const std::wstring& assetFolder);
		void Render(GRenderer&, const Camera&, uint32_t viewportHeight);

		const std::wstring& LastError() const { return lastError; }
		uint32_t SelectedLod() const { return selectedLod; }
		float DiameterPixels() const { return diameterPixels; }

	private:
		std::array<Model, 2> models;
		std::array<NodeHandle, 2> roots{ kInvalidNode, kInvalidNode };
		LegacyScene scene;
		std::vector<InstanceData> items;
		DirectX::XMFLOAT3 center{ 0.0f, 1800.0f, 4200.0f };
		float radius = 350.0f;
		float diameterPixels = 0.0f;
		uint32_t selectedLod = 2;
		bool selected = false;
		bool initialized = false;
		std::wstring lastError;
	};
}
