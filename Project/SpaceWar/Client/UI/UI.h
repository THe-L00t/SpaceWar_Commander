#pragma once
#include <cstdint>
#include <vector>
#include "Client/UISprite.h"

// ============================================================
//  Client/UI/UI.h — 아키텍처 명세서 5절
//
//  화면 구성 요소. Scene 에 귀속되어 그 씬의 상태와 화면을 표현한다.
//
//  ★ 실행 위치 — 상태는 메인, 그리기는 렌더 (「멀티스레딩 분류 명세서」 5장)
//    UI 표시 데이터도 렌더 추출 ④ 의 프레임 패킷에 실어 보낸다 → Extract.
//  ★ 클라이언트에만 있다. 서버는 화면을 모른다.
//  ★ 지금 요소는 텍스처 사각형(UISprite) 하나뿐이다 (2026-10-09, 로딩 화면).
//    글자·버튼 같은 요소는 게임플레이가 정해진 뒤에 붙인다.
// ============================================================

namespace swc {

	class UI
	{
	public:
		using ElementId = uint32_t;

		UI();
		~UI();

		ElementId AddSprite(const UISprite& sprite);
		UISprite& Sprite(ElementId id) { return sprites[id]; }
		const UISprite& Sprite(ElementId id) const { return sprites[id]; }
		void Clear();

		// 렌더 추출 ④ — 그리는 순서(뒤 → 앞)대로 out 뒤에 복사해 붙인다.
		void Extract(std::vector<UISprite>& out) const;

	private:
		std::vector<UISprite> sprites;
	};

} // namespace swc
