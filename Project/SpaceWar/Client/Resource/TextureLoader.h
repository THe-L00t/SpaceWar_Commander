#pragma once
#include <string>
#include "Client/Resource/TextureData.h"

// ============================================================
//  Client/Resource/TextureLoader.h — 이미지 파일 → TextureData
//
//  WIC 로 PNG·JPG 등을 읽어 premultiplied RGBA8 로 바꾸고 밉 체인을 만든다.
//  ★ 어느 스레드에서 불러도 된다. 단 그 스레드에서 COM 이 초기화돼 있어야 한다
//    (메인은 Engine::Initialize, 로딩 스레드는 LoadQueue 가 초기화한다).
// ============================================================

namespace swc {

	bool LoadTextureFile(const wchar_t* path, TextureData& out, std::wstring& error);

} // namespace swc
