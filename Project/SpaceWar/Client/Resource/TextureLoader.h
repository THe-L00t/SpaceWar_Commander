#pragma once
#include <cstddef>
#include <string>
#include "ModelData.h"

namespace swc {

	// 호출 전에 CoInitializeEx가 필요하다. 출력은 위쪽 행부터 저장한 RGBA8이다.
	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error);
	// 모델 안의 압축 이미지(PNG/JPEG 등)를 임시 파일 없이 읽는다.
	bool LoadTextureImageFromMemory(const void* bytes, size_t byteCount, const wchar_t* sourceName,
		bool srgb, TextureData& out, std::wstring& error);
}
