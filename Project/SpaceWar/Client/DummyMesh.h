#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "Vertex.h"

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

	// 구면 위 지형 패치. 접평면 격자를 반지름 radius 구에 투영한다.
	// 월드 원점이 스폰 지점(구 표면)이고 구 중심이 (0,-radius,0) 인 규약을 따른다.
	//
	// ★ 정점 생성은 double 로 한다. float 로 (R+고도) 를 다루면
	//   R=120,000 근처에서 7.8mm 단위로 양자화되어 지형이 떨린다.
	inline MeshData MakeSpherePatch(double radius, double extent, int grid,
		DirectX::XMFLOAT3 color)
	{
		if (grid < 2) grid = 2;

		MeshData m;
		m.vertices.reserve(size_t(grid) * grid);
		m.indices.reserve(size_t(grid - 1) * (grid - 1) * 6);

		const double half = extent * 0.5;
		const double stepSize = extent / double(grid - 1);

		for (int j = 0; j < grid; ++j)
		{
			const double z = -half + stepSize * j;
			for (int i = 0; i < grid; ++i)
			{
				const double x = -half + stepSize * i;

				// 스폰 지점 기준 접평면 좌표 -> 구 중심 기준 방향
				const double len = std::sqrt(x * x + radius * radius + z * z);
				const double dx = x / len;
				const double dy = radius / len;
				const double dz = z / len;

				// 월드 위치 = 중심 + 방향 * R,  중심 = (0, -R, 0)
				Vertex v;
				v.position = { float(dx * radius),
							   float(dy * radius - radius),
							   float(dz * radius) };
				v.normal = { float(dx), float(dy), float(dz) };
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
