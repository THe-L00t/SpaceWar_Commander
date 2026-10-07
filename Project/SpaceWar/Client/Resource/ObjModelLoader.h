#pragma once
#include <string>
#include "ModelData.h"

namespace swc {

	// OBJ/MTL을 직접 읽는다. 반환값에는 렌더러와 독립적인 CPU 데이터만 남긴다.
	class ObjModelLoader
	{
	public:
		bool Load(const wchar_t* path, ModelData& out, std::wstring& error);
	};
}
