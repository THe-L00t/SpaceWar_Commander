#pragma once
#include <string>
#include <utility>
#include <vector>
#include "Handles.h"
#include "InstanceData.h"
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

		// ModelData 메시 번호의 GPU 재질(2026-10-29) — 올린 재질에 렌더러 설정을 더할 때(예시 행성 얼음 호수 물가 페이드).
		MaterialHandle MeshMaterial(uint32_t mesh) const
		{
			return mesh < meshMaterials.size() ? meshMaterials[mesh] : kInvalidMaterial;
		}

		// 2026-11-17 예시 행성 해제 — 이 모델이 만든 GPU 메시 · 텍스처 · 재질을 놓고(GRenderer::Release) 빈 모델로 돌아간다.
		//  프레임 밖에서 부른다. 이 모델로 펼친 장면 노드는 호출자가 먼저 비운다(LegacyScene::ClearSubtree). 실패하면 false.
		//  빈 모델(Initialize 전 · 실패)에 불러도 된다.
		bool Release(GRenderer&);

		// ── TLAS 목록 분리 (2026-11-17) — 장면 노드 없이 메시마다 월드 행렬을 계산해 out 에 덧붙인다 ──
		//  Instantiate 와 같은 변환 사슬(노드 local × … × 표시 보정 × rootWorld). rootWorld = Instantiate 루트가 놓일 월드 행렬.
		//  prevWorld = world(정적), 행렬식 ≈ 0 이면 kInstanceDegenerate · 음수면 kInstanceMirrored.
		void AppendInstances(const DirectX::XMFLOAT4X4& rootWorld, std::vector<InstanceData>& out) const;
		// 이 모델이 만든 GPU 메시(ModelData 메시 번호 순)
		const std::vector<MeshHandle>& Meshes() const { return meshes; }

		// ── 메시 LOD (2026-11-20 render/tlas-select 시험 — 노션 「LOD 적용 준비」 8.4 이름 규칙) ──
		//  노드 이름이 «기준_LOD1» ~ «_LOD3» 이면 그 노드는 «기준» 노드의 단계 메시다. Instantiate · AppendInstances 는 단계 노드를
		//  그리지 않는다(LOD0 만 장면 · 래스터 목록 · TLAS 에 든다). RegisterLods 가 LOD0 메시마다 단계 메시를 렌더러에 잇는다 —
		//  고르기는 렌더러 GPU 컬링. 이름 끝이 _LOD1 ~ _LOD3 이면 1 ~ 3, 아니면 0(base 에 기준 이름).
		static int LodLevelOfNodeName(const std::string& name, std::string* base = nullptr);
		//  objectCenter · objectExtents = 단계 판정용 물체 전체 경계(노드 공간 AABB — 단계 노드 변환이 단위 행렬인 모델만, 아니면 잇지 않는다).
		//  이은 LOD0 메시 수를 돌려준다. 단계 노드의 메시 수 · 재질 순서가 LOD0 노드와 다르면 그 노드는 건너뛴다.
		uint32_t RegisterLods(GRenderer&, const DirectX::XMFLOAT3& objectCenter, const DirectX::XMFLOAT3& objectExtents) const;
		// LOD0 노드(단계 노드가 아닌 것)의 메시 수 — 래스터 목록에서 인스턴스 하나가 차지하는 칸
		uint32_t BaseMeshCount() const;

		const std::wstring& LastError() const { return lastError; }

	private:
		std::vector<ModelNodeData> nodes;
		std::vector<uint8_t> nodeLod;           // 2026-11-20 — 노드마다 LOD 단계(0 = 그리는 노드, 1 ~ 3 = 단계 노드)
		std::vector<MeshHandle> meshes;
		std::vector<MaterialHandle> meshMaterials;
		std::vector<TextureHandle> textures;    // 2026-11-17 — 이 모델이 만든 텍스처(Release 용)
		std::vector<MaterialHandle> materials;  // 2026-11-17 — 이 모델이 만든 재질(Release 용, 0번 기본 재질은 없다)
		DirectX::XMFLOAT4X4 visualLocal{};
		std::wstring lastError;
		bool initialized = false;
	};
}
