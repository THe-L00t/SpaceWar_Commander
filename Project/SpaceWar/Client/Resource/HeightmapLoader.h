#pragma once
#include <string>
#include "Shared/HeightmapData.h"

// 16비트 그레이스케일 PNG 로더 (WIC 사용).
//
// ★ WIC 는 COM 객체다. 호출 전에 CoInitializeEx 가 되어 있어야 한다.
//   안 하면 CO_E_NOTINITIALIZED 로 조용히 실패한다.
//
// ★ 경로는 와이드 문자열로만 다룬다.
//   윈도우 경로는 원래 UTF-16 이다. char* 로 받으면 ANSI(CP949) 인지 UTF-8 인지
//   알 수 없어, 경로에 한글이 있을 때 조용히 깨진다. (실제로 한 번 걸렸다)
//
// ★ 로드 시 min-max 정규화한다 (2026-09-05).
//   샘플을 0~65535 전체로 늘려서 TerrainConfig::relief 가 조각의 실제 봉우리-골 높이가 되게 한다.
//   평평한 조각(min == max)은 그대로 둔다.
namespace swc {

	// 실패 시 false, 사유는 error 에 담긴다.
	bool LoadHeightmapPng(const wchar_t* path, Shared::HeightmapData& out, std::wstring& error);
}
