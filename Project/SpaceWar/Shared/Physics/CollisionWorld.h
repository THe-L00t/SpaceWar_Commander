#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../Vec3.h"

// ============================================================
//  Shared/Physics/CollisionWorld.h — 정적 충돌 월드 (2026-10-29)
//
//  맵 하나(게임 맵 또는 예시 행성)의 «정적 오브젝트 충돌체» 를 들고 있다.
//  근거: 명세 §13 Physics(클라 예측 · 서버 권위가 같은 계산을 쓴다) · §6.3.2 Collision Box,
//        노션 「충돌 처리 ② 현재 서버 상태 · B안 전면 수정 계획」 3절 Shared 표 · 「③ 충돌 모양 비용 비교」 8절.
//  사용자 지시(2026-10-29): «정말 작은 식물의 경우 우선은 충돌에서 빼줘. 엄청 작은 돌들도 플레이어 기준 높이가
//    15%아래라면 충돌체도 만들지 마. 나머진은 저폴리로 충돌 가자» · «나머진 너의 판단대로».
//
//  ★ 데이터 — 충돌체 묶음(glb) + 설명 JSON (Resource_Work/_tools/collision/col_build.py 가 만든다)
//    · kind "types"  (예시 행성): 노드 «COL_<종류 이름>» = 종류 하나의 충돌 메시(종류 glb 루트 공간).
//      배치 JSON 의 인스턴스(AddPlacement)를 그대로 씌운다 — 화면의 오브젝트와 같은 S · R · T.
//    · kind "placed" (게임 맵): 노드 «COL_<맵 노드 이름>» = 오브젝트 하나. 노드 matrix = 그 오브젝트의 월드 행렬.
//    · 설명 JSON 의 class(prop · rock · building · tree)와 min_scale 을 쓴다.
//      작은 식물(method none)은 묶음에 노드가 없다 → 충돌 없음.
//      돌(rock)은 인스턴스 배율 < min_scale 이면 충돌 없음 — min_scale = (1.8 m × 15 %) ÷ 종류 높이(설명 JSON rules).
//  ★ 좌표 — «맵 공간» = 맵 glb 공간 = 행성 중심 기준 glTF 좌표(배치 JSON 과 같다). 월드로 갈 때는 행성 중심을 더한다
//    (PlanetDebugScene::Build · Engine::BuildGameLoad 가 맵 루트를 행성 중심으로 옮기는 것과 같다).
//  ★ 하는 일 — 읽기 · 인스턴스 만들기 · 주변 찾기(QuerySphere) · 디버그 선(edges) ·
//    캡슐 겹침(DeepestCapsuleContact, 2026-10-29 — 플레이어 이동 Shared::PlayerMovement 가 쓴다).
//    두 단계 구조: 인스턴스는 격자, 종류 메시는 삼각형 BVH(노션 「충돌 처리 ③」 8절 «구조»).
//    레이(사격 가림)는 아직 없다. 지형은 여기 없다(PlanetSurface 몫).
//  ★ 스레드 — LoadPack · AddPlacement · Finalize 는 로딩 스레드에서 한 번(멤버만 쓴다). 그 뒤는 읽기만 한다.
//  ★ DirectXMath 를 쓰지 않는다 — 서버도 같은 코드를 쓴다(ModelSource.h 머리 주석).
// ============================================================

namespace Shared {

	enum class CollisionClass : uint8_t { Prop = 0, Rock, Building, Tree, Count };

	// 배치 JSON 인스턴스 한 개 — 클라의 PlanetPlacement::Instance 와 뜻 · 배치가 같다.
	//  위치는 맵 공간(오브젝트 원점 = 바닥 가운데), 회전은 쿼터니언 x y z w, 배율은 균일.
	struct PlacedInstance
	{
		uint32_t type = 0;              // 배치 JSON objects 번호(AddPlacement 의 typeNames 번호)
		float    position[3] = {};
		float    rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float    scale = 1.0f;
	};

	// 종류 메시의 삼각형 BVH 마디 — count > 0 이면 잎(bvhTris[first .. first + count)), 아니면 왼쪽 = 바로 다음 마디 · 오른쪽 = right
	struct CollisionBvhNode
	{
		Vec3     boundsMin{};
		Vec3     boundsMax{};
		uint32_t first = 0;
		uint32_t count = 0;
		uint32_t right = 0;
	};

	struct CollisionMesh
	{
		std::string           name;                         // 묶음 노드 이름에서 «COL_» 을 뗀 것
		CollisionClass        cls = CollisionClass::Prop;
		float                 minScale = 0.0f;              // 돌: 이보다 작은 배율의 인스턴스는 충돌 없음
		std::vector<Vec3>     vertices;                     // 메시 공간(용접됨)
		std::vector<uint32_t> indices;                      // 삼각형 목록
		std::vector<uint32_t> edges;                        // 디버그 선 — 정점 번호 쌍. 경계 · 꺾인 모서리만(같은 평면 대각선은 뺀다)
		Vec3                  boundsMin{};
		Vec3                  boundsMax{};
		std::vector<CollisionBvhNode> bvh;                  // 삼각형 BVH (마디 0 = 뿌리)
		std::vector<uint32_t>         bvhTris;              // 잎이 가리키는 삼각형 번호
	};

	struct CollisionInstance
	{
		uint32_t       mesh = 0;
		CollisionClass cls = CollisionClass::Prop;
		// 메시 공간 → 맵 공간. 행 벡터 3 × 4: p' = x · r0 + y · r1 + z · r2 + t
		//  (r0 = xf[0..2], r1 = xf[3..5], r2 = xf[6..8], t = xf[9..11]) — GltfReader 의 Mat4 · DirectXMath 와 같은 규약.
		float          xf[12] = { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 0.0f };
		// 맵 공간 → 메시 공간 (xf 의 역). 같은 배치 — p = (p' − t) 를 이 3 × 3 으로 곱한다(inv[9..11] = t).
		float          inv[12] = { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 0.0f };
		Vec3           boundsMin{};                          // 맵 공간
		Vec3           boundsMax{};
	};

	// 캡슐이 충돌체와 가장 깊이 겹친 곳 — 맵 공간 단위 법선(충돌체 → 캡슐 쪽)과 겹친 깊이(m)
	struct CapsuleContact
	{
		Vec3  normal{};
		float depth = 0.0f;
	};

	class CollisionWorld
	{
	public:
		static constexpr float kCellSize = 16.0f;            // 공간 격자 칸(m)

		// 묶음 glb + 설명 JSON 을 읽는다. 실패하면 error 에 원인. 이전 내용은 지운다.
		bool LoadPack(const wchar_t* packPath, const wchar_t* manifestPath, std::wstring& error);

		// kind "types" 묶음에 배치 인스턴스를 넣는다. typeNames[i] = 배치 JSON objects 의 i 번째 이름.
		//  충돌체가 없는 종류(작은 식물)와 작은 돌은 넣지 않고 센다. 넣은 수를 돌려준다.
		size_t AddPlacement(const std::vector<std::string>& typeNames, const std::vector<PlacedInstance>& instances);

		// 공간 격자를 만든다. LoadPack(+ AddPlacement) 뒤 한 번 부른다.
		void Finalize();

		bool   Loaded() const { return loaded; }
		bool   PlacedPack() const { return placed; }
		size_t MeshCount() const { return meshes.size(); }
		size_t InstanceCount() const { return instances.size(); }
		size_t MeshTriangleCount() const;                    // 고유 메시 삼각형 합
		size_t SkippedSmallRocks() const { return skippedSmallRocks; }
		size_t SkippedNoCollider() const { return skippedNoCollider; }

		const CollisionMesh&     Mesh(size_t index) const { return meshes[index]; }
		const CollisionInstance& Instance(size_t index) const { return instances[index]; }

		static Vec3 Transform(const CollisionInstance& instance, const Vec3& p);

		// 맵 공간 center 에서 radius 안에 경계 상자가 닿는 인스턴스 번호. out 은 지우고 채운다(번호 오름차순).
		void QuerySphere(const Vec3& center, float radius, std::vector<uint32_t>& out) const;

		// 맵 공간 캡슐(두 끝 구의 중심 a · b, 반지름 radius)이 충돌체 삼각형과 가장 깊이 겹친 곳. 겹치지 않으면 false.
		//  법선은 충돌체에서 캡슐 쪽 — 캡슐을 normal × depth 만큼 밀면 그 삼각형에서 빠진다(가장 깊은 것부터 여러 번 부른다).
		//  상태를 바꾸지 않는다(여러 스레드에서 불러도 된다 — 서버는 틱 스레드 하나).
		bool DeepestCapsuleContact(const Vec3& a, const Vec3& b, float radius, CapsuleContact& out) const;

	private:
		bool loaded = false;
		bool placed = false;
		std::vector<CollisionMesh>                meshes;
		std::unordered_map<std::string, uint32_t> meshOfName;   // kind "types": 종류 이름 → 메시
		std::vector<CollisionInstance>            instances;
		size_t skippedSmallRocks = 0;
		size_t skippedNoCollider = 0;

		std::unordered_map<uint64_t, std::vector<uint32_t>> grid;   // 칸 → 인스턴스
		std::vector<uint32_t>                                large;  // 칸을 너무 많이 덮는 인스턴스(늘 후보)
	};

} // namespace Shared
