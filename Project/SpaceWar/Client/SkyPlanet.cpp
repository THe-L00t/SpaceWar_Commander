#include "SkyPlanet.h"
#include "Camera.h"
#include "GRenderer.h"
#include "Resource/ResourceManager.h"
#include <cmath>

using namespace DirectX;

namespace {
	// 제공한 LOD들은 같은 미터 좌표를 사용하며 원본 Blender 배율이 이미 반영되어 있다.
	constexpr float kAssetRadius = 124.11954f;
	constexpr float kWorldRadius = 350.0f;
	constexpr float kLodBoundaryPixels = 80.0f;
	constexpr float kLodHysteresis = 0.12f;
	constexpr const wchar_t* kLodAssets[] = {
		L"LOD2\\future_ruins_fps_lod2.glb",
		L"LOD3\\future_ruins_fps_lod3.glb" };
}

namespace swc {

	bool SkyPlanet::Initialize(ResourceManager& resources, GRenderer& renderer, const std::wstring& assetFolder)
	{
		lastError.clear();
		if (initialized)
		{
			lastError = L"이미 초기화된 하늘 행성입니다.";
			return false;
		}

		const float scale = kWorldRadius / kAssetRadius;
		XMFLOAT4X4 visual;
		XMStoreFloat4x4(&visual, XMMatrixScaling(scale, scale, scale));
		for (size_t i = 0; i < models.size(); ++i)
		{
			const std::wstring path = assetFolder + kLodAssets[i];
			const ModelHandle handle = resources.LoadModel(path.c_str());
			const ModelData* data = resources.Get(handle);
			if (!data)
			{
				lastError = resources.LastError();
				return false;
			}
			// 내보내기의 양면 설정 대신 닫힌 원거리 행성에는 뒷면 제거를 사용한다.
			const bool uploaded = models[i].Initialize(*data, renderer, visual, true);
			// 한 단계씩 업로드 후 CPU 정점·픽셀을 해제해 두 LOD의 로딩 피크를 겹치지 않는다.
			resources.ReleaseModel(handle);
			if (!uploaded)
			{
				lastError = models[i].LastError();
				return false;
			}
			roots[i] = models[i].Instantiate(scene);
			if (roots[i] == kInvalidNode)
			{
				lastError = L"하늘 행성 LOD를 장면에 등록하지 못했습니다.";
				return false;
			}
			scene.SetLocalTransform(roots[i],
				XMMatrixRotationY(XM_PIDIV4) * XMMatrixTranslation(center.x, center.y, center.z));
			scene.SetVisible(roots[i], i == 0);
		}
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
