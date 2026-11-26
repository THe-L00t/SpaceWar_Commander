#include "FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <windows.h>
#include <wrl/client.h>
#include <dwrite_2.h>

#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace {

	constexpr uint32_t kAtlasWidth = 512;   // 그림판 폭(px). 높이는 다 넣은 뒤 4 의 배수로 정한다
	constexpr int      kPadding = 1;        // 칸 사이 빈 줄(px) — 선형 필터가 옆 칸을 끌어오지 않게

	// 글리프 하나의 8비트 덮임 그림. left · top = 기준선 원점에서 그림 왼쪽 위까지(px, +y 아래).
	struct GlyphBitmap
	{
		int left = 0, top = 0, width = 0, height = 0;
		std::vector<uint8_t> coverage;
	};

	std::wstring HrText(HRESULT hr)
	{
		wchar_t text[16];
		swprintf_s(text, L"0x%08X", unsigned(hr));
		return text;
	}

	std::wstring FileName(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	}

	// 글리프 하나를 그린다.
	//  ① 회색조 안티에일리어싱 — DWRITE_TEXTURE_ALIASED_1x1 텍스처에 0 ~ 255 덮임이 나온다.
	//  ② ①의 텍스처가 비면(드라이버 · OS 차이 대비) ClearType 3x1 로 다시 그려 세 칸을 평균한다.
	//  그림이 없는 글자(띄어쓰기)는 width = 0 으로 true.
	bool RasterizeGlyph(IDWriteFactory2* factory, IDWriteFontFace* face, float emSize, UINT16 glyphIndex,
		GlyphBitmap& out, std::wstring& error)
	{
		FLOAT advance = 0.0f;
		DWRITE_GLYPH_OFFSET offset = {};
		DWRITE_GLYPH_RUN run = {};
		run.fontFace = face;
		run.fontEmSize = emSize;
		run.glyphCount = 1;
		run.glyphIndices = &glyphIndex;
		run.glyphAdvances = &advance;
		run.glyphOffsets = &offset;

		ComPtr<IDWriteGlyphRunAnalysis> analysis;
		HRESULT hr = factory->CreateGlyphRunAnalysis(&run, nullptr,
			DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
			DWRITE_GRID_FIT_MODE_DEFAULT, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
			0.0f, 0.0f, &analysis);
		if (FAILED(hr))
		{
			error = L"CreateGlyphRunAnalysis(회색조) 실패 " + HrText(hr);
			return false;
		}

		RECT bounds = {};
		hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds);
		if (SUCCEEDED(hr) && bounds.right > bounds.left && bounds.bottom > bounds.top)
		{
			const UINT32 w = UINT32(bounds.right - bounds.left);
			const UINT32 h = UINT32(bounds.bottom - bounds.top);
			out.coverage.assign(size_t(w) * h, 0);
			hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, out.coverage.data(), w * h);
			if (FAILED(hr))
			{
				error = L"CreateAlphaTexture(회색조) 실패 " + HrText(hr);
				return false;
			}
			out.left = bounds.left;
			out.top = bounds.top;
			out.width = int(w);
			out.height = int(h);
			return true;
		}

		ComPtr<IDWriteGlyphRunAnalysis> clearType;
		hr = factory->CreateGlyphRunAnalysis(&run, nullptr,
			DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
			DWRITE_GRID_FIT_MODE_DEFAULT, DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE,
			0.0f, 0.0f, &clearType);
		if (FAILED(hr))
		{
			error = L"CreateGlyphRunAnalysis(ClearType) 실패 " + HrText(hr);
			return false;
		}

		bounds = {};
		hr = clearType->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
		if (FAILED(hr))
		{
			error = L"GetAlphaTextureBounds(ClearType) 실패 " + HrText(hr);
			return false;
		}
		if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
		{
			out = GlyphBitmap{};   // 그림 없음
			return true;
		}

		const UINT32 w = UINT32(bounds.right - bounds.left);
		const UINT32 h = UINT32(bounds.bottom - bounds.top);
		std::vector<uint8_t> rgb(size_t(w) * h * 3, 0);
		hr = clearType->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, rgb.data(), UINT32(rgb.size()));
		if (FAILED(hr))
		{
			error = L"CreateAlphaTexture(ClearType) 실패 " + HrText(hr);
			return false;
		}
		out.coverage.resize(size_t(w) * h);
		for (size_t i = 0; i < out.coverage.size(); ++i)
			out.coverage[i] = uint8_t((unsigned(rgb[i * 3]) + rgb[i * 3 + 1] + rgb[i * 3 + 2] + 1) / 3);
		out.left = bounds.left;
		out.top = bounds.top;
		out.width = int(w);
		out.height = int(h);
		return true;
	}

} // namespace

namespace swc {

	FontAtlas::StyleId FontAtlas::AddStyle(const std::wstring& fontPath, float pixelSize, const std::wstring& chars)
	{
		Style style;
		style.fontPath = fontPath;
		style.pixelSize = pixelSize;
		style.chars = chars;
		styles.push_back(std::move(style));
		return StyleId(styles.size() - 1);
	}

	bool FontAtlas::Bake(TextureData& out, std::wstring& error, std::wstring& missing)
	{
		missing.clear();

		ComPtr<IDWriteFactory2> factory;
		HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
			reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
		if (FAILED(hr))
		{
			error = L"DirectWrite 팩토리 생성 실패 " + HrText(hr);
			return false;
		}

		// 그린 글리프 — 다 그린 뒤 한꺼번에 그림판에 놓는다
		struct Pending
		{
			StyleId     style = 0;
			wchar_t     ch = 0;
			float       advance = 0.0f;
			GlyphBitmap bitmap;
			int         atlasX = 0, atlasY = 0;
		};
		std::vector<Pending> pending;

		for (StyleId s = 0; s < StyleId(styles.size()); ++s)
		{
			Style& style = styles[s];
			style.glyphs.clear();
			const std::wstring name = FileName(style.fontPath);

			ComPtr<IDWriteFontFile> file;
			hr = factory->CreateFontFileReference(style.fontPath.c_str(), nullptr, &file);
			if (FAILED(hr))
			{
				error = name + L": 글꼴 파일을 열지 못했다 " + HrText(hr) + L" (" + style.fontPath + L")";
				return false;
			}

			BOOL supported = FALSE;
			DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
			DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
			UINT32 faceCount = 0;
			hr = file->Analyze(&supported, &fileType, &faceType, &faceCount);
			if (FAILED(hr) || !supported || faceCount == 0)
			{
				error = name + L": DirectWrite 가 읽을 수 없는 글꼴 " + HrText(hr);
				return false;
			}

			IDWriteFontFile* files[] = { file.Get() };
			ComPtr<IDWriteFontFace> face;
			hr = factory->CreateFontFace(faceType, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, &face);
			if (FAILED(hr))
			{
				error = name + L": CreateFontFace 실패 " + HrText(hr);
				return false;
			}

			DWRITE_FONT_METRICS metrics = {};
			face->GetMetrics(&metrics);
			const float unitsToPixels = style.pixelSize / float((std::max)(UINT16(1), metrics.designUnitsPerEm));
			style.ascent = float(metrics.ascent) * unitsToPixels;
			style.descent = float(metrics.descent) * unitsToPixels;

			std::wstring done;   // 이 모양에서 이미 그린 글자
			for (const wchar_t ch : style.chars)
			{
				if (done.find(ch) != std::wstring::npos) continue;
				done.push_back(ch);

				const UINT32 codePoint = UINT32(ch);
				UINT16 glyphIndex = 0;
				hr = face->GetGlyphIndices(&codePoint, 1, &glyphIndex);
				if (FAILED(hr) || glyphIndex == 0)
				{
					missing += name + L" '" + std::wstring(1, ch) + L"' ";
					continue;
				}

				DWRITE_GLYPH_METRICS glyphMetrics = {};
				face->GetDesignGlyphMetrics(&glyphIndex, 1, &glyphMetrics, FALSE);

				Pending p;
				p.style = s;
				p.ch = ch;
				p.advance = float(glyphMetrics.advanceWidth) * unitsToPixels;
				std::wstring glyphError;
				if (!RasterizeGlyph(factory.Get(), face.Get(), style.pixelSize, glyphIndex, p.bitmap, glyphError))
				{
					error = name + L": " + glyphError;
					return false;
				}
				pending.push_back(std::move(p));
			}
		}

		// 선반 쌓기 — 왼쪽부터 한 줄씩. 넘치면 다음 줄.
		int x = kPadding, y = kPadding, rowHeight = 0;
		for (Pending& p : pending)
		{
			if (p.bitmap.width == 0) continue;
			if (x + p.bitmap.width + kPadding > int(kAtlasWidth))
			{
				x = kPadding;
				y += rowHeight + kPadding;
				rowHeight = 0;
			}
			p.atlasX = x;
			p.atlasY = y;
			x += p.bitmap.width + kPadding;
			rowHeight = (std::max)(rowHeight, p.bitmap.height);
		}
		const uint32_t height = (uint32_t(y + rowHeight + kPadding) + 3u) & ~3u;

		atlasWidth = kAtlasWidth;
		atlasHeight = (std::max)(height, 4u);

		out = TextureData{};
		out.path = L"(HUD 글자 그림판)";
		out.width = atlasWidth;
		out.height = atlasHeight;
		out.srgb = false;
		out.pixels.assign(size_t(atlasWidth) * atlasHeight * 4, 0);

		for (Pending& p : pending)
		{
			Glyph g;
			g.advance = p.advance;
			g.left = p.bitmap.left;
			g.top = p.bitmap.top;
			g.width = p.bitmap.width;
			g.height = p.bitmap.height;
			g.atlasX = p.atlasX;
			g.atlasY = p.atlasY;

			// 흰 글자 premultiplied — 색 = 알파 = 덮임
			for (int row = 0; row < g.height; ++row)
				for (int col = 0; col < g.width; ++col)
				{
					const uint8_t c = p.bitmap.coverage[size_t(row) * g.width + col];
					uint8_t* texel = &out.pixels[(size_t(g.atlasY + row) * atlasWidth + size_t(g.atlasX + col)) * 4];
					texel[0] = texel[1] = texel[2] = texel[3] = c;
				}

			styles[p.style].glyphs[p.ch] = g;
		}
		return true;
	}

	float FontAtlas::Measure(StyleId styleId, const std::wstring& text) const
	{
		if (styleId >= styles.size()) return 0.0f;
		const Style& style = styles[styleId];
		float width = 0.0f;
		for (const wchar_t ch : text)
		{
			const auto it = style.glyphs.find(ch);
			if (it != style.glyphs.end()) width += it->second.advance;
		}
		return width;
	}

	void FontAtlas::AppendText(std::vector<UISprite>& out, StyleId styleId, const std::wstring& text,
		float x, float centerY, Align align, const DirectX::XMFLOAT4& color, float clipLeft, float clipRight) const
	{
		if (!Ready() || styleId >= styles.size() || atlasWidth == 0 || atlasHeight == 0) return;
		const Style& style = styles[styleId];

		const float width = Measure(styleId, text);
		float penX = x;
		if (align == Align::Center) penX -= width * 0.5f;
		else if (align == Align::Right) penX -= width;

		// em 상자 가운데 → 기준선 (웹 캔버스 'middle' 과 같은 식)
		const float baseline = std::round(centerY + (style.ascent - style.descent) * 0.5f);
		const float invW = 1.0f / float(atlasWidth);
		const float invH = 1.0f / float(atlasHeight);

		for (const wchar_t ch : text)
		{
			const auto it = style.glyphs.find(ch);
			if (it == style.glyphs.end()) continue;
			const Glyph& g = it->second;

			if (g.width > 0 && g.height > 0)
			{
				float left = std::round(penX) + float(g.left);
				float right = left + float(g.width);
				const float top = baseline + float(g.top);
				const float bottom = top + float(g.height);
				float u0 = float(g.atlasX);
				float u1 = float(g.atlasX + g.width);

				if (right > clipLeft && left < clipRight)
				{
					if (left < clipLeft) { u0 += clipLeft - left; left = clipLeft; }
					if (right > clipRight) { u1 -= right - clipRight; right = clipRight; }

					UISprite sprite;
					sprite.texture = texture;
					sprite.center = DirectX::XMFLOAT2((left + right) * 0.5f, (top + bottom) * 0.5f);
					sprite.halfSize = DirectX::XMFLOAT2((right - left) * 0.5f, (bottom - top) * 0.5f);
					sprite.uvMin = DirectX::XMFLOAT2(u0 * invW, float(g.atlasY) * invH);
					sprite.uvMax = DirectX::XMFLOAT2(u1 * invW, float(g.atlasY + g.height) * invH);
					sprite.tint = color;
					out.push_back(sprite);
				}
			}
			penX += g.advance;
		}
	}

} // namespace swc
