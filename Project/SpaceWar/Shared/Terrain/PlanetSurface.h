#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
//  Shared/Terrain/PlanetSurface.h — 모델 맵의 접지 표면
//
//  클라이언트와 서버가 같은 OBJ/GLB의 지면 부분을 읽는다.
//  렌더 재질·텍스처를 제외하고 위치와 삼각형만 보관한다.
//  한 번 만든 BVH는 이후 읽기만 하므로 서버의 여러 스레드가 공유할 수 있다.
//
//  구면 맵은 Height()의 방사 방향 조회, 평면 맵은 HeightAt()의 수직 조회를 쓴다.
//  OBJ/GLB 위치는 같은 DirectX 좌수계로 보관한다.
//  GLB는 PlanetConst.h의 kPlanetSurfaceNode와 구면 자산의 보행로·착륙장만 읽는다.
//  벽·지붕·장식물과 잘못 배치된 COL_Landing 프록시는 지면으로 사용하지 않는다.
// ============================================================

namespace Shared {

	class PlanetSurface
	{
	public:
		bool Load(const wchar_t* path, std::wstring& error);

		bool Valid() const { return !nodes.empty(); }
		size_t TriangleCount() const { return triangles.size(); }

		// 입력은 DirectX 좌표계의 방향. 반환값은 기준구 위 높이(m)다.
		double Height(double upX, double upY, double upZ, double planetRadius) const;

		// 평면 맵의 DirectX 월드 X/Z에서 지면 Y(m). 맵 밖이면 0이다.
		double HeightAt(double worldX, double worldZ) const;

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
		bool LoadGlb(const wchar_t* path, std::wstring& error);
		uint32_t BuildNode(uint32_t first, uint32_t count);
		bool Intersects(const Node&, const double direction[3], double bestDistance) const;
		double IntersectTriangle(const Triangle&, const double direction[3]) const;

		std::vector<Position> positions;
		std::vector<Triangle> triangles;
		std::vector<Node> nodes;
	};

} // namespace Shared
