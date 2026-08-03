#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "Vertex.h"
#include "SimTypes.h"

// 테스트용 더미 메쉬 생성 (Assimp 등 파일 로딩 없이 코드로 만든다)
// 면마다 노멀이 달라야 하므로 박스는 정점을 공유하지 않고 24개로 만든다.
namespace swc {
	struct MeshData {
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	inline MeshData MakeBox(float sx, float sy, float sz, DirectX::XMFLOAT3 color)
	{
		using DirectX::XMFLOAT3;
		const float hx = sx * 0.5f;
		const float hy = sy * 0.5f;
		const float hz = sz * 0.5f;

		// 면당 4정점, 바깥쪽을 향하는 노멀. 와인딩은 시계방향(=D3D 기본 앞면).
		const XMFLOAT3 corners[6][4] = {
			{ { -hx, -hy, -hz }, { -hx,  hy, -hz }, {  hx,  hy, -hz }, {  hx, -hy, -hz } },  // -Z
			{ {  hx, -hy,  hz }, {  hx,  hy,  hz }, { -hx,  hy,  hz }, { -hx, -hy,  hz } },  // +Z
			{ { -hx, -hy,  hz }, { -hx,  hy,  hz }, { -hx,  hy, -hz }, { -hx, -hy, -hz } },  // -X
			{ {  hx, -hy, -hz }, {  hx,  hy, -hz }, {  hx,  hy,  hz }, {  hx, -hy,  hz } },  // +X
			{ { -hx, -hy, -hz }, {  hx, -hy, -hz }, {  hx, -hy,  hz }, { -hx, -hy,  hz } },  // -Y
			{ { -hx,  hy, -hz }, { -hx,  hy,  hz }, {  hx,  hy,  hz }, {  hx,  hy, -hz } },  // +Y
		};
		const XMFLOAT3 normals[6] = {
			{ 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f },
			{ -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
			{ 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
		};

		MeshData m;
		m.vertices.reserve(24);
		m.indices.reserve(36);
		for (uint32_t f = 0; f < 6; ++f)
		{
			const uint32_t base = f * 4;
			for (uint32_t v = 0; v < 4; ++v)
				m.vertices.push_back({ corners[f][v], normals[f], color });

			m.indices.push_back(base + 0);
			m.indices.push_back(base + 1);
			m.indices.push_back(base + 2);
			m.indices.push_back(base + 0);
			m.indices.push_back(base + 2);
			m.indices.push_back(base + 3);
		}
		return m;
	}

	inline MeshData MakeCube(float size, DirectX::XMFLOAT3 color)
	{
		return MakeBox(size, size, size, color);
	}

	// 구면 위 지형 패치. 접평면 격자를 구에 투영하고 지형 높이를 얹는다.
	// 월드 원점이 스폰 지점(구 표면)이고 구 중심이 (0,-R,0) 인 규약을 따른다.
	//
	// ★ 높이는 반드시 planet.SurfaceHeight() 를 거친다.
	//   여기서 자체 계산하면 충돌 판정과 갈라져 플레이어가 지형에 파묻힌다.
	//
	// ★ 노멀은 이웃 정점 차분으로 구한다.
	//   방향 벡터를 그대로 쓰면 지형이 울퉁불퉁한데 조명은 매끈한 구처럼 보인다.
	//
	// ★ 정점 생성은 double 로 한다. float 로 (R+고도) 를 다루면
	//   R=120,000 근처에서 7.8mm 단위로 양자화되어 지형이 떨린다.
	inline MeshData MakeSpherePatch(const Planet& planet, double extent, int grid,
		DirectX::XMFLOAT3 color)
	{
		if (grid < 2) grid = 2;

		const double R = planet.radius;
		const double half = extent * 0.5;
		const double stepSize = extent / double(grid - 1);
		const size_t count = size_t(grid) * grid;

		// 노멀 계산에 이웃이 필요하므로 방향·높이를 먼저 전부 채운다
		std::vector<Vec3d>  dirs(count);
		std::vector<double> heights(count);

		for (int j = 0; j < grid; ++j)
		{
			const double z = -half + stepSize * j;
			for (int i = 0; i < grid; ++i)
			{
				const double x = -half + stepSize * i;
				const double len = std::sqrt(x * x + R * R + z * z);
				const Vec3d d{ x / len, R / len, z / len };

				const size_t k = size_t(j) * grid + i;
				dirs[k] = d;
				heights[k] = planet.SurfaceHeight(d);   // ★ 반드시 이 경로
			}
		}

		auto WorldAt = [&](int i, int j) -> Vec3d
		{
			i = i < 0 ? 0 : (i >= grid ? grid - 1 : i);
			j = j < 0 ? 0 : (j >= grid ? grid - 1 : j);
			const size_t k = size_t(j) * grid + i;
			return planet.PositionAt(dirs[k], heights[k]);
		};

		MeshData m;
		m.vertices.reserve(count);
		m.indices.reserve(size_t(grid - 1) * (grid - 1) * 6);

		for (int j = 0; j < grid; ++j)
		{
			for (int i = 0; i < grid; ++i)
			{
				const size_t k = size_t(j) * grid + i;
				const Vec3d p = planet.PositionAt(dirs[k], heights[k]);

				// 중앙 차분 (가장자리는 한쪽 차분으로 자동 축소)
				const Vec3d du = WorldAt(i + 1, j) - WorldAt(i - 1, j);
				const Vec3d dv = WorldAt(i, j + 1) - WorldAt(i, j - 1);
				const Vec3d n = Normalize(Cross(dv, du));

				Vertex v;
				v.position = ToFloat3(p);
				v.normal = ToFloat3(n);
				v.color = color;
				m.vertices.push_back(v);
			}
		}

		for (int j = 0; j < grid - 1; ++j)
		{
			for (int i = 0; i < grid - 1; ++i)
			{
				const uint32_t a = uint32_t(j * grid + i);
				const uint32_t b = uint32_t((j + 1) * grid + i);
				const uint32_t c = uint32_t((j + 1) * grid + i + 1);
				const uint32_t d = uint32_t(j * grid + i + 1);
				m.indices.insert(m.indices.end(), { a, b, c, a, c, d });
			}
		}
		return m;
	}

	inline MeshData MakeGround(float size, DirectX::XMFLOAT3 color)
	{
		const float h = size * 0.5f;
		const DirectX::XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
		MeshData m;
		m.vertices = {
			{ { -h, 0.0f, -h }, up, color },
			{ { -h, 0.0f,  h }, up, color },
			{ {  h, 0.0f,  h }, up, color },
			{ {  h, 0.0f, -h }, up, color },
		};
		m.indices = { 0, 1, 2, 0, 2, 3 };
		return m;
	}
}
