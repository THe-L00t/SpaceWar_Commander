#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "ModelData.h"

namespace swc {

	// 호출 전에 CoInitializeEx가 필요하다. 출력은 위쪽 행부터 저장한 RGBA8이다.
	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error);
	// 호출이 끝날 때까지 bytes가 유효해야 한다. 인코딩된 이미지 바이트는 복사하지 않는다.
	bool LoadTextureImageFromMemory(const uint8_t* bytes, size_t byteCount, const wchar_t* label,
		bool srgb, TextureData& out, std::wstring& error);
}
