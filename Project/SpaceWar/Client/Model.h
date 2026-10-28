#pragma once
#include <string>
#include <utility>
#include <vector>
#include "Handles.h"
#include "Resource/ModelData.h"

namespace swc {

	class GRenderer;
	class LegacyScene;   // 09-18 개명 — 게임 Scene 과 렌더 Render World 로 나뉘기 전의 임시 클래스

	// GPU 메시/재질은 한 번 만들고 인스턴스별로 Scene 노드만 추가한다.
	// 모델 로더 객체나 ResourceManager 내부 데이터의 포인터를 보관하지 않는다.
	class Model
	{
	public:
		bool Initialize(const ModelData&, GRenderer&);
		// 맵처럼 캐릭터의 높이/발 보정이 필요 없는 모델은 표시 변환을 직접 지정한다.
		// 닫힌 표시용 행성만 양면 설정을 덮어써 뒷면 제거를 강제할 수 있다.
		bool Initialize(const ModelData&, GRenderer&, const DirectX::XMFLOAT4X4& visualTransform,
			bool forceBackfaceCulling = false);
		// meshNodes(선택, 2026-10-28) — 만든 그리기 노드를 (ModelData 메시 번호, 노드) 쌍으로 덧붙인다.
		//  올린 뒤에 특정 메시의 재질만 바꿀 때 쓴다(예시 행성 지형 레이어 — PlanetDebugScene).
		NodeHandle Instantiate(LegacyScene&, NodeHandle parent = kInvalidNode,
			std::vector<std::pair<uint32_t, NodeHandle>>* meshNodes = nullptr) const;

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
