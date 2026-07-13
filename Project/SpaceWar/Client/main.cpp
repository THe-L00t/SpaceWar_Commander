#include <windows.h>
#include <vector>
#include <DirectXMath.h>
#include "GRenderer.h"
#include "Scene.h"

using namespace DirectX;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	HWND hwnd = nullptr;

	swc::GRenderer renderer;
	renderer.Initialize(hwnd, 1280, 720);

	swc::Scene scene;
	swc::MeshHandle shipMesh = 0;
	swc::NodeHandle ship = scene.AddNode(swc::kInvalidNode, shipMesh, 0);

	std::vector<swc::RenderItem> items;

	bool running = true;
	while (running)
	{
		scene.SetLocalTransform(ship, XMMatrixTranslation(0.0f, 0.0f, 5.0f));

		scene.UpdateWorldTransforms();
		scene.Extract(items);

		renderer.BeginFrame();
		swc::RenderView view{ };
		renderer.Render(view, items, scene.WorldData());
		renderer.EndFrame();
	}
	return 0;
}
