#pragma once
#include <array>
#include <string>
#include <vector>
#include "Model.h"
#include "LegacyScene.h"

namespace swc {

	class Camera;
	class GRenderer;

	// 하늘의 표시용 행성. 이동·중력·충돌 맵과 별도로 LOD 노드를 소유한다. (client2 2f15728)
	//  ★ 읽기는 Engine 의 로드 묶음이 한다(로딩 스레드 파싱 → 메인 업로드, 명세 ⑥)
	//    LOD 하나가 들어올 때마다 AddLod 로 GPU 에 올리고, 둘 다 들어오면 그리기 시작한다.
	class SkyPlanet
	{
	public:
		static constexpr size_t kLodCount = 2;   // [0] = LOD2, [1] = LOD3

		// exe 옆 assets\ 기준 상대 경로.
		static const wchar_t* LodAsset(size_t lod);

		// 메인 스레드. 업로드 뒤 data 의 CPU 사본은 호출자가 해제한다(ReleaseModel).
		bool AddLod(size_t lod, const ModelData&, GRenderer&);
		void Render(GRenderer&, const Camera&, uint32_t viewportHeight);

		const std::wstring& LastError() const { return lastError; }
		uint32_t SelectedLod() const { return selectedLod; }
		float DiameterPixels() const { return diameterPixels; }

	private:
		std::array<Model, kLodCount> models;
		std::array<NodeHandle, kLodCount> roots{ kInvalidNode, kInvalidNode };
		LegacyScene scene;
		FramePacket packet;
		DirectX::XMFLOAT3 center{ 0.0f, 1800.0f, 4200.0f };
		float radius = 350.0f;
		float diameterPixels = 0.0f;
		uint32_t selectedLod = 2;
		bool selected = false;
		bool initialized = false;
		std::wstring lastError;
	};
}
