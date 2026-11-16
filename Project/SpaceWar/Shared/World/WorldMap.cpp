#include "WorldMap.h"

#include <cmath>
#include <string_view>
#include <vector>
#include "../PlanetConst.h"
#include "../Model/ModelReader.h"
#include "../Terrain/PlanetSurface.h"

// 머리 주석은 WorldMap.h.

namespace {

	// 행 벡터 규약 행렬 곱 — GltfReader.cpp Multiply 와 같은 식(노드 월드 = 로컬 · 부모 월드)
	Shared::Mat4 Multiply(const Shared::Mat4& a, const Shared::Mat4& b)
	{
		Shared::Mat4 r;
		for (int row = 0; row < 4; ++row)
			for (int col = 0; col < 4; ++col)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; ++k)
					sum += a.m[row * 4 + k] * b.m[k * 4 + col];
				r.m[row * 4 + col] = sum;
			}
		return r;
	}

} // namespace

namespace Shared {

	bool LoadWorldGround(const wchar_t* path, const char* spawnNode, PlanetSurface& surface,
		double spawnUp[3], bool& spawnFromNode, std::wstring& error)
	{
		error.clear();

		// 기본 스폰 방향 = 게임 맵의 Landing L1 (PlanetConst.h) — 클라 PlanetDebugScene::Build 의 기본값과 같다.
		const double length = std::sqrt(kPlanetSpawnUpX * kPlanetSpawnUpX +
			kPlanetSpawnUpY * kPlanetSpawnUpY + kPlanetSpawnUpZ * kPlanetSpawnUpZ);
		spawnUp[0] = kPlanetSpawnUpX / length;
		spawnUp[1] = kPlanetSpawnUpY / length;
		spawnUp[2] = kPlanetSpawnUpZ / length;
		spawnFromNode = false;

		if (!path || !*path)
		{
			error = L"맵 경로가 비어 있습니다.";
			return false;
		}

		// PlanetSurface::Load 와 같은 읽기 — 위치만, 접지 오브젝트의 면을 collected 에 모은다.
		ReadOptions options;
		options.geometryOnly = true;
		options.generateTangents = false;
		options.generateCollision = false;
		options.readAnimation = false;
		options.collectObject = &PlanetSurface::IsGroundObject;

		ModelSource source;
		if (!ModelReader().Load(path, source, error, options))
			return false;
		if (!surface.Build(source.collected, error))
			return false;

		if (!spawnNode)
			return true;

		// 스폰 노드 — 부모가 먼저 온다(ModelSource.h SourceNode). 월드 = 로컬 · 부모 월드 를 앞에서부터 쌓는다.
		const std::string_view wanted(spawnNode);
		std::vector<Mat4> world(source.nodes.size());
		for (size_t i = 0; i < source.nodes.size(); ++i)
		{
			const SourceNode& node = source.nodes[i];
			world[i] = (node.parent == kInvalidIndex || node.parent >= i) ? node.local : Multiply(node.local, world[node.parent]);
			if (node.name != wanted) continue;

			const double x = world[i].m[12], y = world[i].m[13], z = world[i].m[14];
			const double offset = std::sqrt(x * x + y * y + z * z);
			if (offset > 1.0)
			{
				spawnUp[0] = x / offset;
				spawnUp[1] = y / offset;
				spawnUp[2] = z / offset;
				spawnFromNode = true;
			}
			break;   // 클라와 같이 같은 이름의 첫 노드만 본다
		}
		return true;
	}

} // namespace Shared
