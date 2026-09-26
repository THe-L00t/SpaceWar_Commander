#pragma once
#include <string>
#include "ModelData.h"

namespace swc {

	// FBX SDK 객체의 수명은 Load 안에서 끝난다. 반환값에는 CPU 데이터만 남긴다.
	class FbxModelLoader
	{
	public:
		bool Load(const wchar_t* path, ModelData& out, std::wstring& error);
	};
}
