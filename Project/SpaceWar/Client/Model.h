#pragma once
#include <string>
#include <vector>
#include "Handles.h"
#include "Resource/ModelData.h"

namespace swc {

	class GRenderer;
	class LegacyScene;   // 09-18 개명 — 게임 Scene 과 렌더 Render World 로 나뉘기 전의 임시 클래스

	// GPU 메시/재질은 한 번 만들고 인스턴스별로 Scene 노드만 추가한다.
	// FBX SDK 객체나 ResourceManager 내부 데이터의 포인터를 보관하지 않는다.
	class Model
	{
	public:
		bool Initialize(const ModelData&, GRenderer&);
		NodeHandle Instantiate(LegacyScene&, NodeHandle parent = kInvalidNode) const;

		const std::wstring& LastError() const { return lastError; }

	private:
		std::vector<ModelNodeData> nodes;
		std::vector<MeshHandle> meshes;
		std::vector<MaterialHandle> meshMaterials;
		DirectX::XMFLOAT4X4 visualLocal{};
		std::wstring lastError;
		bool initialized = false;
	};
}
