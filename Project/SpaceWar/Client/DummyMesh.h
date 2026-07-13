#pragma once
#include <vector>
#include <cstdint>
#include "Vertex.h"

// 테스트용 더미 메쉬 생성 (Assimp 등 파일 로딩 없이 코드로 만든다)
namespace swc {
	struct MeshData {
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	inline MeshData MakeCube(float size, DirectX::XMFLOAT3 color)
	{
		const float h = size * 0.5f;
		MeshData m;
		m.vertices = {
			{ { -h, -h, -h }, color }, { { -h,  h, -h }, color },
			{ {  h,  h, -h }, color }, { {  h, -h, -h }, color },
			{ { -h, -h,  h }, color }, { { -h,  h,  h }, color },
			{ {  h,  h,  h }, color }, { {  h, -h,  h }, color },
		};
		m.indices = {
			0, 1, 2, 0, 2, 3,
			4, 6, 5, 4, 7, 6,
			4, 5, 1, 4, 1, 0,
			3, 2, 6, 3, 6, 7,
			1, 5, 6, 1, 6, 2,
			4, 0, 3, 4, 3, 7,
		};
		return m;
	}

	inline MeshData MakeGround(float size, DirectX::XMFLOAT3 color)
	{
		const float h = size * 0.5f;
		MeshData m;
		m.vertices = {
			{ { -h, 0.0f, -h }, color },
			{ { -h, 0.0f,  h }, color },
			{ {  h, 0.0f,  h }, color },
			{ {  h, 0.0f, -h }, color },
		};
		m.indices = { 0, 1, 2, 0, 2, 3 };
		return m;
	}
}
