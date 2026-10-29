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

	// ── DDS (2026-10-28 지형 레이어) ──
	//  resource 브랜치 dfa4fe4 의 밉맵 DDS(DirectXTex texconv, DX10 머리말)를 밉 전부 그대로 읽는다. COM 이 필요 없다.
	//  받는 형식: R8G8B8A8_UNORM_SRGB(색) · R8G8B8A8_UNORM(데이터) · R8_UNORM(높이 · 눈밭), 2D 한 장, 무압축.
	//  출력 = TextureData(format · srgb · mipLevels · arraySize 1, pixels = 밉 0 부터 차례로).
	bool LoadDdsTexture(const wchar_t* path, TextureData& out, std::wstring& error);

	// 같은 크기 · 형식 · 밉 수의 텍스처 여러 장을 배열 텍스처 하나로 묶는다(slices 순서 = 배열 원소 순서).
	//  slices 의 픽셀은 옮겨 가므로 끝나면 비어 있다.
	bool CombineTextureArray(std::vector<TextureData>& slices, TextureData& out, std::wstring& error);
}
