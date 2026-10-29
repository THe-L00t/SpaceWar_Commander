#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Client/DebugLine.h"
#include "Client/Vec3d.h"
#include "Shared/Physics/CollisionWorld.h"

// ============================================================
//  Client/DebugView/ColliderDebugView.h — C 키 충돌체 와이어프레임 (2026-10-29)
//  ★ 폴더 이름이 Debug 가 아닌 이유 — .gitignore 의 «[Dd]ebug/»(빌드 출력 폴더)에 걸려 커밋되지 않는다.
//
//  사용자 지시(2026-10-29): «저폴리로 충돌체 만들면 c키를 눌러 저폴리 충돌체를 와이어 프레임으로 볼 수있게 해줘».
//  플레이어 둘레 kRadius 안의 충돌체(Shared::CollisionWorld) 모서리를 디버그 선으로 모은다.
//  그리기는 GRenderer::DrawDebugLines(톤맵 뒤 백버퍼, 가려진 부분은 옅게).
//
//  ★ 색 = 종류(설명 JSON class) — 건물 하늘색 · 소품 노랑 · 돌 주황 · 나무 초록.
//    작은 식물 · 작은 돌(배율 < min_scale)은 충돌체가 없어 선도 없다 — 빠진 것을 눈으로 확인할 수 있다.
//  ★ 선 = 메시의 경계 · 꺾인 모서리(CollisionMesh::edges). 평면을 나눈 대각선은 긋지 않는다.
//  ★ 좌표 = 맵 공간(행성 중심 기준). DrawDebugLines 의 origin 에 행성 중심(월드)을 넘긴다.
//  ★ kRebuildDistance 만큼 움직이거나 월드가 바뀌면 다시 모은다. 그 사이에는 모은 선을 그대로 쓴다.
//  ★ 메인 스레드 전용. 화면 확인용이라 판정에는 쓰지 않는다.
// ============================================================

namespace swc {

	class ColliderDebugView
	{
	public:
		static constexpr float  kRadius = 80.0f;                 // m — 이 안에 경계 상자가 닿는 충돌체만 그린다
		static constexpr double kRebuildDistance = 4.0;          // m — 이만큼 움직이면 다시 모은다
		static constexpr size_t kMaxVertices = 2u * 1024u * 1024u;   // GRenderer::kMaxDebugLineVertices 와 같다(선 100만 개)

		static constexpr uint32_t kBuildingColor = DebugColor(64, 224, 255);
		static constexpr uint32_t kPropColor = DebugColor(255, 214, 64);
		static constexpr uint32_t kRockColor = DebugColor(255, 140, 56);
		static constexpr uint32_t kTreeColor = DebugColor(96, 255, 112);

		// eye = 맵 공간 위치(월드 − 행성 중심). world 가 없거나 비었으면 선을 비운다.
		void Update(const Shared::CollisionWorld* world, const Vec3d& eye, bool force);
		void Clear();

		const std::vector<DebugLineVertex>& Vertices() const { return vertices; }
		size_t InstanceCount() const { return instanceCount; }   // 이번에 모은 충돌체 수
		size_t LineCount() const { return vertices.size() / 2; }
		bool   Truncated() const { return truncated; }            // 선이 상한을 넘어 먼 것을 뺐다

	private:
		const Shared::CollisionWorld* built = nullptr;
		Vec3d                         lastEye{};
		bool                          valid = false;
		std::vector<uint32_t>         query;
		std::vector<DebugLineVertex>  vertices;
		size_t                        instanceCount = 0;
		bool                          truncated = false;
	};

} // namespace swc
