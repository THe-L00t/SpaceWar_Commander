#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>
#include "Client/Handles.h"
#include "Client/Resource/ModelData.h"
#include "Client/UISprite.h"

// ============================================================
//  Client/UI/FontAtlas.h — 글자 그림판 (2026-11-26 HUD)
//
//  TTF 글꼴에서 쓸 글자만 미리 그려 텍스처 한 장에 모은다. 글자 하나 = UISprite 하나(그림판의 한 칸).
//  지금 UI 가 그릴 수 있는 것은 텍스처 사각형(UISprite)뿐이라, 글자도 같은 길로 그린다(새 셰이더 · PSO 없음).
//
//  ★ 그리는 법 — Windows SDK 의 DirectWrite 로 CPU 에서 글리프를 8비트 덮임(회색조)으로 그린다.
//    글꼴 파일을 직접 읽는다(설치 안 함 — 팀원 HUD 안내 «TTF 를 게임 패키지에서 읽으며 사용자 PC 에 글꼴을 미리 설치하는 방식에 의존하지 않는다»).
//    Direct2D · D3D11On12 는 쓰지 않는다 — 결과는 보통 RAM 이미지(TextureData)이고 GRenderer::CreateTexture 로 올린다.
//  ★ 모양(Style) = 글꼴 파일 하나 × 픽셀 크기 하나. 크기마다 따로 그려서 확대 · 축소로 흐려지지 않게 한다.
//    화면 배율(HUD 1920×1080 기준 → 창)을 곱한 크기로 넣는다(Hud.cpp).
//  ★ 그림판에 없는 글자는 그리지 않고 건너뛴다. 새 한글 문구를 쓰려면 AddStyle 의 chars 에 그 글자를 더한다.
//  ★ 그림판 텍스처는 premultiplied 흰 글자다(색 = 알파 = 덮임). 색은 UISprite tint 로 준다(UI.hlsl).
//  ★ 커닝 · 합자는 하지 않는다 — 글자 폭(advance)만 더한다. HUD 숫자 · 짧은 한글 낱말에는 충분하다.
// ============================================================

namespace swc {

	class FontAtlas
	{
	public:
		using StyleId = uint32_t;

		enum class Align : uint8_t
		{
			Left,     // x = 글자 줄 왼쪽 끝
			Center,   // x = 글자 줄 가운데
			Right,    // x = 글자 줄 오른쪽 끝
		};

		// 모양 하나를 넣는다 — Bake 전에. chars = 그릴 글자(같은 글자가 겹쳐도 된다).
		StyleId AddStyle(const std::wstring& fontPath, float pixelSize, const std::wstring& chars);

		// 넣은 모양을 모두 그려 RAM 이미지(RGBA8 premultiplied)를 만든다.
		//  글꼴 파일을 못 읽으면 false + error. 글꼴에 없는 글자는 missing 에 모아 돌려준다(그리지 않음).
		bool Bake(TextureData& out, std::wstring& error, std::wstring& missing);

		// Bake 결과를 GPU 에 올린 핸들 — 부르는 쪽이 CreateTexture 뒤에 넣는다.
		void SetTexture(TextureHandle handle) { texture = handle; }
		bool Ready() const { return texture != kInvalidTexture; }

		// 글자 줄의 폭(px) — 글자 폭의 합
		float Measure(StyleId style, const std::wstring& text) const;

		// 글자 줄을 UISprite 로 out 뒤에 붙인다.
		//  x = align 의 기준점, centerY = 글자 줄의 세로 가운데(웹 캔버스 textBaseline 'middle' — em 상자 가운데).
		//  가로로 clipLeft ~ clipRight 밖은 잘라 낸다(나침반 표시 영역). 글자 기준점은 정수 px 에 맞춰 흐려지지 않게 한다.
		void AppendText(std::vector<UISprite>& out, StyleId style, const std::wstring& text,
			float x, float centerY, Align align, const DirectX::XMFLOAT4& color,
			float clipLeft = -1.0e9f, float clipRight = 1.0e9f) const;

	private:
		struct Glyph
		{
			float advance = 0.0f;   // 다음 글자까지 (px)
			int   left = 0;         // 기준선 원점에서 그림 왼쪽 위까지 (px, +y 아래)
			int   top = 0;
			int   width = 0;        // 그림 크기 (0 = 그림 없음 — 띄어쓰기)
			int   height = 0;
			int   atlasX = 0;       // 그림판 안 자리 (px)
			int   atlasY = 0;
		};

		struct Style
		{
			std::wstring fontPath;
			float        pixelSize = 0.0f;
			std::wstring chars;
			float        ascent = 0.0f;    // 기준선 위 (px)
			float        descent = 0.0f;   // 기준선 아래 (px, 양수)
			std::unordered_map<wchar_t, Glyph> glyphs;
		};

		std::vector<Style> styles;
		uint32_t           atlasWidth = 0;
		uint32_t           atlasHeight = 0;
		TextureHandle      texture = kInvalidTexture;
	};

} // namespace swc
