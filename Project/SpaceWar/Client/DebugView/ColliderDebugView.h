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
//
//  ★ 캐릭터 충돌 캡슐 (2026-10-29 사용자 «플레이어 충돌 캡슐도 그려줘, npc도»)
//    정적 충돌체 선 뒤에 매 프레임 붙인다 — ClearCapsules() 로 지난 프레임 캡슐을 떼고 AddCapsule() 로 붙인다.
//    모양 = Shared::PlayerMovement 캡슐(반지름 kCapsuleRadius · 키 kCapsuleHeight, 발 = 몸통 중심 − kGroundOffset).
//    서버 NPC 이동(Server/AI/NpcWorld)도 같은 캡슐로 막힌다. 색: 내 플레이어 흰색 · 다른 플레이어 보라 · NPC 분홍.
// ============================================================

namespace swc {

	class ColliderDebugView
	{
	public:
		static constexpr float  kRadius = 80.0f;                 // m — 이 안에 경계 상자가 닿는 충돌체만 그린다
		static constexpr double kRebuildDistance = 4.0;          // m — 이만큼 움직이면 다시 모은다
		static constexpr size_t kMaxVertices = 2u * 1024u * 1024u;   // GRenderer::kMaxDebugLineVertices 와 같다(선 100만 개)

		// 캡슐 하나 = 고리 2개 · 세로선 4개 · 반원 4개. 고리는 kCapsuleSegments 마디, 반원은 그 절반.
		static constexpr int    kCapsuleSegments = 16;
		static constexpr size_t kCapsuleVertices = size_t(2 * (4 * kCapsuleSegments + 4));   // 선 68개
		static constexpr size_t kCapsuleReserve = kCapsuleVertices * 512;   // 정적 충돌체 선이 캡슐 몫(512개)을 남긴다

		static constexpr uint32_t kBuildingColor = DebugColor(64, 224, 255);
		static constexpr uint32_t kPropColor = DebugColor(255, 214, 64);
		static constexpr uint32_t kRockColor = DebugColor(255, 140, 56);
		static constexpr uint32_t kTreeColor = DebugColor(96, 255, 112);
		static constexpr uint32_t kPlayerColor = DebugColor(255, 255, 255);        // 내 플레이어
		static constexpr uint32_t kRemotePlayerColor = DebugColor(176, 128, 255);  // 다른 플레이어
		static constexpr uint32_t kNpcColor = DebugColor(255, 72, 176);            // NPC

		// eye = 맵 공간 위치(월드 − 행성 중심). world 가 없거나 비었으면 선을 비운다.
		void Update(const Shared::CollisionWorld* world, const Vec3d& eye, bool force);
		void Clear();

		// 지난 프레임 캡슐을 뗀다(정적 충돌체 선은 남긴다). Update 뒤 · AddCapsule 앞에 부른다.
		void ClearCapsules();
		// mapPosition = 몸통 중심(맵 공간), up = 그 자리 위쪽(단위). 상한을 넘으면 그리지 않고 Truncated 를 세운다.
		void AddCapsule(const Vec3d& mapPosition, const Vec3d& up, uint32_t color);

		const std::vector<DebugLineVertex>& Vertices() const { return vertices; }
		size_t InstanceCount() const { return instanceCount; }   // 이번에 모은 충돌체 수
		size_t CapsuleCount() const { return capsuleCount; }     // 이번 프레임 캡슐 수
		size_t LineCount() const { return vertices.size() / 2; }
		bool   Truncated() const { return truncated; }            // 선이 상한을 넘어 먼 것을 뺐다

	private:
		const Shared::CollisionWorld* built = nullptr;
		Vec3d                         lastEye{};
		bool                          valid = false;
		std::vector<uint32_t>         query;
		std::vector<DebugLineVertex>  vertices;
		size_t                        staticVertexCount = 0;      // vertices 앞쪽 = 정적 충돌체 선, 뒤쪽 = 캡슐
		size_t                        instanceCount = 0;
		size_t                        capsuleCount = 0;
		bool                          truncated = false;
	};

} // namespace swc
