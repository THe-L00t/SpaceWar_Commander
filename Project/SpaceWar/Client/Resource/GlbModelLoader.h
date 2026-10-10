#pragma once
#include <string>
#include "ModelData.h"

namespace swc {

	// 정적 glTF 2.0(GLB) 모델을 읽고, 반복 노드가 같은 ModelMeshData를 참조하게 한다.
	class GlbModelLoader
	{
	public:
		bool Load(const wchar_t* path, ModelData& out, std::wstring& error);
	};
}
