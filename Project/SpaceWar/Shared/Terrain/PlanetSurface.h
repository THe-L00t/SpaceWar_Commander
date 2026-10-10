#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ============================================================
//  Shared/Terrain/PlanetSurface.h — OBJ 행성의 접지 표면
//
//  클라이언트와 서버가 같은 OBJ의 지면 부분을 읽는다.
//  렌더 재질·텍스처를 제외하고 위치와 삼각형만 보관한다.
//  한 번 만든 BVH는 이후 읽기만 하므로 서버의 여러 스레드가 공유할 수 있다.
//
//  ★ 기존 구면 이동 규약을 유지한다
//    행성 중심에서 바깥으로 쏜 레이가 만나는 가장 바깥 지면을 높이로 쓴다.
//    건물 벽·동굴·교량 아래의 충돌은 이 방향당 높이 하나인 형식의 범위 밖이다.
//    지붕·부유 잔해·대기는 접지 대상으로 읽지 않는다.
//  ★ 행성 OBJ 를 한 번만 파싱한다 (2026-10-09)
//    렌더 메시를 읽는 같은 파싱에서 ReadOptions::collectObject = IsGroundObject 로
//    접지 오브젝트의 면을 ModelSource::collected 에 함께 모은 뒤 Build 로 넘긴다(클라).
//    서버처럼 렌더가 필요 없으면 Load(path) 가 같은 방식으로 읽는다.
// ============================================================

namespace Shared {

	struct CollectedGeometry;

	class PlanetSurface
	{
	public:
		// 파일을 직접 읽는다(서버). 내부적으로 collectObject + Build 와 같은 길이다.
		bool Load(const wchar_t* path, std::wstring& error);

		// 이미 파싱한 결과로 만든다(클라 — 렌더용 파싱과 한 번에).
		// ground 는 collectObject = IsGroundObject 로 모은 것이어야 한다.
		bool Build(const CollectedGeometry& ground, std::wstring& error);

		// 이 맵의 접지 오브젝트 이름 규칙. ReadOptions::collectObject 에 그대로 넘긴다.
		// 지붕·벽까지 접지면으로 읽으면 그 아래를 걷는 플레이어도 지붕 위로 밀려난다.
		static bool IsGroundObject(std::string_view name);

		// 위 규칙의 이름표. 모델 캐시(ModelCache)의 태그로 쓴다 — 함수는 비교할 수 없으므로
		// ★ IsGroundObject 의 규칙을 바꾸면 이 문자열의 버전을 올린다. 안 올리면 예전 접지면을 캐시에서 읽는다.
		static constexpr const char* kGroundRuleTag = "PlanetSurface.IsGroundObject.v1";

		bool Valid() const { return !nodes.empty(); }
		size_t TriangleCount() const { return triangles.size(); }

		// 입력은 DirectX 좌표계의 방향. 반환값은 기준구 위 높이(m)다.
		double Height(double upX, double upY, double upZ, double planetRadius) const;

	private:
		struct Position
		{
			float x, y, z;
		};

		struct Triangle
		{
			uint32_t vertices[3];
		};

		struct Node
		{
			Position lo{}, hi{};
			uint32_t left = 0;
			uint32_t right = 0;
			uint32_t first = 0;
			uint32_t count = 0;   // 0이면 내부 노드, 아니면 삼각형 구간
		};

		void CompactPositions();
		uint32_t BuildNode(uint32_t first, uint32_t count);
		bool Intersects(const Node&, const double direction[3], double bestDistance) const;
		double IntersectTriangle(const Triangle&, const double direction[3]) const;

		std::vector<Position> positions;
		std::vector<Triangle> triangles;
		std::vector<Node> nodes;
	};

} // namespace Shared
