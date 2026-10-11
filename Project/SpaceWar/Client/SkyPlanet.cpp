#include "SkyPlanet.h"
#include "Camera.h"
#include "GRenderer.h"
#include "Resource/ModelData.h"
#include <cmath>

using namespace DirectX;

namespace {
	// 제공한 LOD들은 같은 미터 좌표를 사용하며 원본 Blender 배율이 이미 반영되어 있다.
	constexpr float kAssetRadius = 124.11954f;
	constexpr float kWorldRadius = 350.0f;
	constexpr float kLodBoundaryPixels = 80.0f;
	constexpr float kLodHysteresis = 0.12f;
	constexpr const wchar_t* kLodAssets[swc::SkyPlanet::kLodCount] = {
		L"model\\sky_planet\\LOD2\\future_ruins_fps_lod2.glb",
		L"model\\sky_planet\\LOD3\\future_ruins_fps_lod3.glb" };
}

namespace swc {

	const wchar_t* SkyPlanet::LodAsset(size_t lod)
	{
		return lod < kLodCount ? kLodAssets[lod] : L"";
	}

	bool SkyPlanet::AddLod(size_t lod, const ModelData& data, GRenderer& renderer)
	{
		lastError.clear();
		if (lod >= kLodCount || roots[lod] != kInvalidNode)
		{
			lastError = L"하늘 행성 LOD 번호가 잘못됐거나 이미 올렸습니다.";
			return false;
		}

		const float scale = kWorldRadius / kAssetRadius;
		XMFLOAT4X4 visual;
		XMStoreFloat4x4(&visual, XMMatrixScaling(scale, scale, scale));
		// 내보내기의 양면 설정 대신 닫힌 원거리 행성에는 뒷면 제거를 사용한다.
		if (!models[lod].Initialize(data, renderer, visual, true))
		{
			lastError = models[lod].LastError();
			return false;
		}
		roots[lod] = models[lod].Instantiate(scene);
		if (roots[lod] == kInvalidNode)
		{
			lastError = L"하늘 행성 LOD를 장면에 등록하지 못했습니다.";
			return false;
		}
		scene.SetLocalTransform(roots[lod],
			XMMatrixRotationY(XM_PIDIV4) * XMMatrixTranslation(center.x, center.y, center.z));
		scene.SetVisible(roots[lod], lod == 0);

		// 두 LOD 가 다 올라와야 그린다. 순서는 로딩 스레드가 끝낸 순서라 정해져 있지 않다.
		for (const NodeHandle root : roots)
			if (root == kInvalidNode) return true;
		// Extract가 프레임마다 새 메모리를 할당하지 않도록 처음에 공간을 확보한다.
		items.reserve(scene.NodeCount());
		radius = kWorldRadius;
		initialized = true;
		return true;
	}

	void SkyPlanet::Render(GRenderer& renderer, const Camera& camera, uint32_t viewportHeight)
	{
		if (!initialized) return;

		const XMFLOAT3& eye = camera.EyePosition();
		const double dx = double(center.x) - eye.x;
		const double dy = double(center.y) - eye.y;
		const double dz = double(center.z) - eye.z;
		const double distanceSquared = dx * dx + dy * dy + dz * dz;
		const double tangentFov = std::tan(double(camera.VerticalFov()) * 0.5);
		// 구의 접선 각도로 화면상의 지름을 계산한다. 거리·조준 FOV·렌더 높이를 함께 반영한다.
		diameterPixels = distanceSquared > double(radius) * radius && tangentFov > 0.0
			? float(double(viewportHeight) * radius /
				(std::sqrt(distanceSquared - double(radius) * radius) * tangentFov))
			: float(viewportHeight) * 10.0f;

		uint32_t next = selectedLod;
		if (!selected)
			next = diameterPixels >= kLodBoundaryPixels ? 2u : 3u;
		else if (selectedLod == 2 && diameterPixels < kLodBoundaryPixels * (1.0f - kLodHysteresis))
			next = 3;
		else if (selectedLod == 3 && diameterPixels > kLodBoundaryPixels * (1.0f + kLodHysteresis))
			next = 2;

		if (!selected || next != selectedLod)
		{
			selectedLod = next;
			selected = true;
			for (size_t i = 0; i < roots.size(); ++i)
				scene.SetVisible(roots[i], i + 2 == selectedLod);
		}
		scene.UpdateWorldTransforms();
		scene.Extract(items);

		// 배경은 10m near 평면으로 먼저 그려 먼 구조물의 깊이 정밀도를 유지한다.
		// 렌더러가 배경 뒤 깊이를 초기화하므로 맵·캐릭터는 언제나 행성보다 앞에 표시된다.
		RenderView view{};
		view.viewProj = camera.BackgroundViewProj();
		view.eyePosition = eye;
		view.background = true;
		renderer.Render(view, items, scene.WorldData());
	}
}
