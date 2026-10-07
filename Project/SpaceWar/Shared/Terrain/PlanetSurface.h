#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
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
