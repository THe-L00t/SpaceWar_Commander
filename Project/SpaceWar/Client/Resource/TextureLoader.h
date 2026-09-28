#pragma once
#include <string>
#include "ModelData.h"

namespace swc {

	// 호출 전에 CoInitializeEx가 필요하다. 출력은 위쪽 행부터 저장한 RGBA8이다.
	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error);
}
