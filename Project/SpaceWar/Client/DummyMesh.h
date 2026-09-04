#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "Vertex.h"
#include "Planet.h"
#include "Shared/Units.h"

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

	// 큐브 구(6면) 행성 전체 메시. 면마다 격자를 구에 투영하고 지형 높이를 얹는다.
	// 월드 원점이 스폰 지점(구 표면)이고 구 중심이 (0,-R,0) 인 규약을 따른다.
	// 스폰(up = (0,1,0))은 +Y 면의 정중앙이다.
	//
	// ★ 높이는 반드시 planet.SurfaceHeight() 를 거친다.
	//   여기서 자체 계산하면 충돌 판정과 갈라져 플레이어가 지형에 파묻힌다.
	//
	// ★ 노멀은 격자 이웃이 아니라 «방향 공간» 차분으로 구한다.
	//   면 경계에서는 이웃 정점이 다른 면에 있어 격자 차분은 이음새에 밝기 선을 남긴다.
	//   방향만으로 정해지는 값이라 6면 어디서나 같은 결과가 나온다.
	//
	// ★ 면 격자는 tan 으로 균등각 매핑한다.
	//   [-1,1] 을 그대로 투영하면 면 중앙(=스폰)의 정점 간격이 가장자리의 2배로 벌어진다.
	//   가장자리 ±1 은 tan(π/4) 의 반올림 오차를 피해 정확히 ±1 로 못박는다.
	//   그래야 이웃 면과 정점이 비트 단위로 일치해 틈이 안 생긴다.
	//
	// ★ 정점 생성은 double 로 하고 마지막에 float 로 내린다.
	inline MeshData MakeCubeSphere(const Planet& planet, int faceGrid, DirectX::XMFLOAT3 color)
	{
		if (faceGrid < 2) faceGrid = 2;
		const int N = faceGrid;
		const size_t perFace = size_t(N) * N;

		// (right, forward, up). right × forward = -up 이 여섯 면 모두 성립하도록 잡았다.
		// 그래야 +Y 면(기존 접평면 패치와 같은 배치)의 인덱스 순서를 그대로 써도
		// 여섯 면의 앞면 방향이 같다.
		struct Face { Vec3d right, forward, up; };
		const Face faces[6] = {
			{ {  1.0, 0.0, 0.0 }, { 0.0, 0.0,  1.0 }, { 0.0,  1.0, 0.0 } },   // +Y (스폰 면)
			{ {  1.0, 0.0, 0.0 }, { 0.0, 0.0, -1.0 }, { 0.0, -1.0, 0.0 } },   // -Y
			{ {  0.0, 0.0, 1.0 }, { 0.0, 1.0,  0.0 }, {  1.0, 0.0, 0.0 } },   // +X
			{ {  0.0, 0.0,-1.0 }, { 0.0, 1.0,  0.0 }, { -1.0, 0.0, 0.0 } },   // -X
			{ {  0.0, 1.0, 0.0 }, { 1.0, 0.0,  0.0 }, { 0.0, 0.0,  1.0 } },   // +Z
			{ {  0.0,-1.0, 0.0 }, { 1.0, 0.0,  0.0 }, { 0.0, 0.0, -1.0 } },   // -Z
		};

		// 격자 좌표 [-1,1] → 균등각 [-1,1]. 정수 분자로 만들어 ±대칭이 정확히 맞는다.
		std::vector<double> warp(N);
		for (int i = 0; i < N; ++i)
		{
			const double t = double(2 * i - (N - 1)) / double(N - 1);
			if (i == 0)          warp[i] = -1.0;
			else if (i == N - 1) warp[i] = 1.0;
			else
			{
				const double a = std::tan(std::fabs(t) * (Shared::kPiD * 0.25));
				warp[i] = t < 0.0 ? -a : a;
			}
		}

		auto DirAt = [&](const Face& f, int i, int j) -> Vec3d
		{
			return Normalize(f.right * warp[i] + f.up + f.forward * warp[j]);
		};
		auto SurfaceAt = [&](const Vec3d& d) -> Vec3d
		{
			return planet.PositionAt(d, planet.SurfaceHeight(d));   // ★ 반드시 이 경로
		};

		// 방향 공간 중앙 차분. 격자 한 칸 각도만큼 떨어진 네 점으로 접선 두 개를 만든다.
		const double dTheta = (Shared::kPiD * 0.5) / double(N - 1);
		auto NormalAt = [&](const Vec3d& d) -> Vec3d
		{
			const Vec3d ref = (std::fabs(d.y) < 0.9) ? Vec3d{ 0.0, 1.0, 0.0 } : Vec3d{ 1.0, 0.0, 0.0 };
			const Vec3d t1 = ProjectOntoPlane(ref, d);
			const Vec3d t2 = Cross(d, t1);
			const Vec3d du = SurfaceAt(Normalize(d + t1 * dTheta)) - SurfaceAt(Normalize(d - t1 * dTheta));
			const Vec3d dv = SurfaceAt(Normalize(d + t2 * dTheta)) - SurfaceAt(Normalize(d - t2 * dTheta));
			Vec3d n = Normalize(Cross(dv, du));
			if (Dot(n, d) < 0.0) n = n * -1.0;   // 항상 바깥쪽
			return n;
		};

		MeshData m;
		m.vertices.reserve(perFace * 6);
		m.indices.reserve(size_t(N - 1) * (N - 1) * 6 * 6);

		double refSign = 0.0;
		for (int f = 0; f < 6; ++f)
		{
			const Face& face = faces[f];

			for (int j = 0; j < N; ++j)
			{
				for (int i = 0; i < N; ++i)
				{
					const Vec3d d = DirAt(face, i, j);
					Vertex v;
					v.position = SurfaceAt(d).ToFloat3();
					v.normal = NormalAt(d).ToFloat3();
					v.color = color;
					m.vertices.push_back(v);
				}
			}

			// 앞면 방향 검사. 여섯 면의 첫 삼각형이 바깥을 향하는 부호가 +Y 면과 같아야 한다.
			// 위 손방향 규약이 맞으면 절대 뒤집히지 않지만, 규약이 깨졌을 때 한 면만
			// 안 보이는 사고를 막기 위해 실물로 확인한다.
			const Vec3d pa = SurfaceAt(DirAt(face, 0, 0));
			const Vec3d pb = SurfaceAt(DirAt(face, 0, 1));
			const Vec3d pc = SurfaceAt(DirAt(face, 1, 1));
			const double sign = Dot(Cross(pb - pa, pc - pa), pa - planet.center);
			if (f == 0) refSign = sign;
			const bool flip = (sign * refSign) < 0.0;

			const uint32_t base = uint32_t(f * perFace);
			for (int j = 0; j < N - 1; ++j)
			{
				for (int i = 0; i < N - 1; ++i)
				{
					const uint32_t a = base + uint32_t(j * N + i);
					const uint32_t b = base + uint32_t((j + 1) * N + i);
					const uint32_t c = base + uint32_t((j + 1) * N + i + 1);
					const uint32_t d = base + uint32_t(j * N + i + 1);
					if (!flip) m.indices.insert(m.indices.end(), { a, b, c, a, c, d });
					else       m.indices.insert(m.indices.end(), { a, c, b, a, d, c });
				}
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
