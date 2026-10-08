#pragma once
#include <cstdint>
#include <string>
#include "Client/Scene/Scene.h"

// ============================================================
//  Client/Scene/LoadingScene.h — 로딩 씬 (2026-10-09)
//
//  리소스가 아직 다 올라오지 않은 동안 보여 주는 씬. 배경 한 장 + 도는 스피너.
//  SceneManager 가 LoadBatch 를 받아 전환할 때, 묶음이 끝날 때까지 이 씬을 띄운다.
//  처음 시작할 때도, 씬을 바꿀 때도, 같은 씬 안에서 큰 리소스를 더 읽을 때도 같은 씬을 쓴다.
//
//  ★ 이 씬의 이미지는 동기로 읽는다
//    로딩 화면을 그리려면 로딩 화면 이미지가 먼저 있어야 한다. 두 장뿐이라 짧다.
//    GPU 로 올린 뒤 RAM 사본은 바로 버린다(명세서 원칙 5).
//  ★ 메인 스레드 소유. 진행률은 SceneManager 가 묶음에서 받아 넣는다.
// ============================================================

namespace swc {

	class GRenderer;
	class ResourceManager;

	class LoadingScene : public Scene
	{
	public:
		bool Initialize(GRenderer& renderer, ResourceManager& resources,
			const std::wstring& backgroundPath, const std::wstring& spinnerPath,
			std::wstring& error);

		// 창 크기에 맞춰 배경을 채우고(cover) 스피너를 오른쪽 아래에 둔다.
		void Layout(uint32_t width, uint32_t height);

		void Update(float dt) override;

		void SetProgress(float value) { progress = value; }
		float Progress() const { return progress; }

	private:
		UI::ElementId background = 0;
		UI::ElementId spinner = 0;
		uint32_t      backgroundWidth = 1;
		uint32_t      backgroundHeight = 1;

		float         spinTime = 0.0f;
		float         progress = 0.0f;
	};

} // namespace swc
