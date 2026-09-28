#pragma once
#include <string>
#include "ModelData.h"

namespace swc {

	// Assimp 장면의 수명은 Load 안에서 끝난다. 반환값에는 CPU 데이터만 남긴다.
	class AssimpModelLoader
	{
	public:
		bool Load(const wchar_t* path, ModelData& out, std::wstring& error);
	};
}
