#pragma once
#include <vector>
#include <cstdint>
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
