#pragma once
#include <cstdint>
#include <string>
#include "ModelData.h"

namespace swc {

	// 호출 전에 CoInitializeEx가 필요하다. 출력은 위쪽 행부터 저장한 RGBA8이다.
	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error);

	// .glb 안에 들어 있던 이미지처럼 «파일이 아닌» 바이트에서 읽는다 (2026-10-07).
	// label 은 오류 메시지·캐시 키에 쓰는 이름이다(파일 경로가 없으므로).
	bool LoadTextureImageFromMemory(const uint8_t* bytes, size_t size, bool srgb,
		const wchar_t* label, TextureData& out, std::wstring& error);
}
