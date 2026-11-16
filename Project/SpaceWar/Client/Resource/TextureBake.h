#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "ModelData.h"

// ============================================================
//  Client/Resource/TextureBake.h — WIC 로 푼 텍스처 픽셀을 디스크에 bake 해 두는 캐시 (2026-10-29)
//
//  모델 디스크 캐시(Shared/Model/ModelCache.h)는 메시만 bake 하고, 텍스처는 PNG 압축 그대로 들고 있어
//  실행할 때마다 WIC 로 다시 풀었다(행성 하나에 RGBA 수백 MB — 두 번째 실행도 로딩이 길었던 원인).
//  사용자 지시(10-29) «WIC 로 PNG 읽은 거를 bake 해줘» — 밉맵 DDS 로 바꿨더니 멀리 있는 것이 번쩍거려 되돌렸다.
//  그 뒤 render/lab 텍스처 LOD(b2235ce, client1 bbb06ec)가 밉맵 DDS 를 먼저 읽게 됐다 → 지금 bake 는
//  옆에 DDS 가 없거나 맞지 않아 PNG 를 풀어야 하는 이미지에만 쓰인다(ModelBuilder.cpp TextureCache).
//
//  첫 실행: PNG 를 WIC 로 풀고(예전과 같은 길) 그 RGBA8 픽셀을 그대로 이 파일로 쓴다.
//  다음부터: 같은 PNG 바이트면 파일을 픽셀 배열에 한 번에 읽는다 — 풀기가 없다.
//  화질은 예전과 같다(WIC 결과 그대로, 밉 1단).
//
//  ★ 키 = PNG 바이트의 내용 해시 + 바이트 수 + sRGB 여부
//    같은 이미지는 모델 · 빌드(Debug · Release) · 클론이 달라도 한 벌만 굽는다.
//    원본이 바뀌면 해시가 바뀌어 새로 굽는다(옛 파일은 지우지 않고 남는다).
//  ★ kTextureBakeVersion 은 «결과 픽셀이 달라지는» 변경(WIC 변환 규칙 등)마다 올린다.
//  ★ 쓰기는 임시 파일에 쓴 뒤 이름을 바꾼다 — 끊겨도 반쪽 파일이 남지 않는다. 실패해도 게임은 계속된다.
//  ★ 같은 기계 전용이다(머리말 구조체 배치 · 바이트 순서 그대로). 폴더는 호출 쪽이 정한다.
// ============================================================

namespace swc {

	// 1 = 첫 판 (2026-10-29) — WIC GUID_WICPixelFormat32bppRGBA, 밉 1단
	inline constexpr uint32_t kTextureBakeVersion = 1;

	struct TextureBakeKey
	{
		uint64_t hash = 0;          // 원본(PNG · JPEG) 바이트의 내용 해시
		uint64_t sourceBytes = 0;   // 원본 바이트 수
		bool     srgb = false;      // 같은 이미지라도 색(sRGB) · 데이터(선형) 슬롯은 따로 굽는다
	};

	TextureBakeKey MakeTextureBakeKey(const uint8_t* bytes, size_t size, bool srgb);

	// 폴더 안의 파일 경로. 이름(이미지 · 파일 이름)은 사람이 알아보라고 붙인다 — 구분은 해시가 한다.
	std::wstring TextureBakePath(const std::wstring& directory, const std::wstring& name, const TextureBakeKey& key);

	// 파일이 있고 키가 맞으면 out 을 채우고 true. 없거나 낡았거나 깨졌으면 false(out 은 그대로).
	bool ReadTextureBake(const std::wstring& path, const TextureBakeKey& key, TextureData& out);

	// RGBA8 · 밉 1단 · 배열 1장만 쓴다. 실패하면 false(다음 실행에서 다시 풀 뿐이다).
	bool WriteTextureBake(const std::wstring& path, const TextureBakeKey& key, const TextureData& data);
}
