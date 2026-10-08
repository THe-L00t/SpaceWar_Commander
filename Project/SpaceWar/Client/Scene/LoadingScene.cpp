#include "LoadingScene.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "Client/GRenderer.h"
#include "Client/Resource/ResourceManager.h"

namespace {

	// ── 스피너 ──────────────────────────────────────────────
	//  이미지의 살은 12개다. 30도씩 끊어 돌려야 살이 제자리에서 밝기만 바뀌는 것처럼 보인다.
	//  매끄럽게 돌리면 살이 미끄러져 보인다.
	constexpr int   kSpinnerSpokes = 12;
	constexpr float kSpinnerTurnsPerSecond = 1.0f;

	// 사각형 크기(px). 이미지에서 살이 차지하는 것은 가운데 40% 정도라
	// 실제로 보이는 고리 지름은 이 값의 0.4배(약 64px)다.
	constexpr float kSpinnerSize = 160.0f;
	constexpr float kSpinnerRingRadius = kSpinnerSize * 0.2f;
	constexpr float kSpinnerMargin = 48.0f;    // 창 가장자리 ~ 고리 바깥까지

	constexpr float kPi = 3.14159265f;

	// 경로 끝 파일 이름 — 오류 메시지용
	std::wstring FileName(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	}

	// 2x2 박스 필터로 반씩 줄인다. 홀수 크기의 마지막 줄·열은 가장자리를 한 번 더 쓴다.
	void HalveRgba8(swc::TextureData& image)
	{
		const uint32_t srcW = image.width, srcH = image.height;
		const uint32_t dstW = (srcW > 1) ? srcW / 2 : 1;
		const uint32_t dstH = (srcH > 1) ? srcH / 2 : 1;
		std::vector<uint8_t> dst(size_t(dstW) * dstH * 4);

		for (uint32_t y = 0; y < dstH; ++y)
		{
			const uint32_t y0 = (y * 2 < srcH) ? y * 2 : srcH - 1;
			const uint32_t y1 = (y * 2 + 1 < srcH) ? y * 2 + 1 : y0;
			for (uint32_t x = 0; x < dstW; ++x)
			{
				const uint32_t x0 = (x * 2 < srcW) ? x * 2 : srcW - 1;
				const uint32_t x1 = (x * 2 + 1 < srcW) ? x * 2 + 1 : x0;
				const uint8_t* a = image.pixels.data() + (size_t(y0) * srcW + x0) * 4;
				const uint8_t* b = image.pixels.data() + (size_t(y0) * srcW + x1) * 4;
				const uint8_t* c = image.pixels.data() + (size_t(y1) * srcW + x0) * 4;
				const uint8_t* d = image.pixels.data() + (size_t(y1) * srcW + x1) * 4;
				uint8_t* o = dst.data() + (size_t(y) * dstW + x) * 4;
				for (int ch = 0; ch < 4; ++ch)
					o[ch] = uint8_t((unsigned(a[ch]) + b[ch] + c[ch] + d[ch] + 2) / 4);
			}
		}
		image.width = dstW;
		image.height = dstH;
		image.pixels = std::move(dst);
	}

	// UI 에 맞게 고친다.
	//  ① 알파를 미리 곱한다 — UI PSO 가 ONE / INV_SRC_ALPHA 로 합성한다(GRenderer.cpp).
	//     투명한 픽셀의 색(대개 흰색)이 줄이는 동안 섞여 가장자리에 테가 생기는 것도 막는다.
	//  ② 그릴 크기의 2배 근처까지 미리 줄인다 — 렌더러 텍스처에 밉이 없어서,
	//     원본(스피너 1254px)을 160px 로 바로 그리면 살이 반짝거린다.
	void PrepareForUI(swc::TextureData& image, float drawSize)
	{
		for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
		{
			const unsigned alpha = image.pixels[i + 3];
			for (size_t ch = 0; ch < 3; ++ch)
				image.pixels[i + ch] = uint8_t((image.pixels[i + ch] * alpha + 127) / 255);
		}

		if (drawSize <= 0.0f) return;
		while (float((std::max)(image.width, image.height)) > drawSize * 2.0f)
			HalveRgba8(image);
	}

	// RAM 으로 읽어 UI 용으로 고친 뒤 GPU 로 올리고, RAM 사본은 버린다.
	// drawSize = 화면에 그릴 대략의 크기(px). 0 이면 줄이지 않는다.
	swc::TextureHandle UploadTexture(swc::GRenderer& renderer, swc::ResourceManager& resources,
		const std::wstring& path, float drawSize, uint32_t& outWidth, uint32_t& outHeight, std::wstring& error)
	{
		// UI 는 스왑체인(UNORM)에 그대로 쓰므로 sRGB 변환 없이 읽는다.
		const swc::TextureDataHandle data = resources.LoadTexture(path.c_str(), false);
		const swc::TextureData* loaded = resources.Get(data);
		if (!loaded)
		{
			error = FileName(path) + L": " + resources.LastError();
			return swc::kInvalidTexture;
		}

		outWidth = loaded->width;
		outHeight = loaded->height;

		swc::TextureData image = *loaded;
		resources.Release(data);
		PrepareForUI(image, drawSize);

		const swc::TextureHandle texture = renderer.CreateTexture(image);
		if (texture == swc::kInvalidTexture)
			error = FileName(path) + L": " + renderer.StatusText();
		return texture;
	}

}

namespace swc {

	bool LoadingScene::Initialize(GRenderer& renderer, ResourceManager& resources,
		const std::wstring& backgroundPath, const std::wstring& spinnerPath,
		std::wstring& error)
	{
		ui.Clear();

		uint32_t spinnerWidth = 1, spinnerHeight = 1;
		// 배경은 창과 크기가 비슷해 그대로 올린다(0 = 줄이지 않음).
		const TextureHandle backgroundTexture = UploadTexture(renderer, resources,
			backgroundPath, 0.0f, backgroundWidth, backgroundHeight, error);
		if (backgroundTexture == kInvalidTexture) return false;

		const TextureHandle spinnerTexture = UploadTexture(renderer, resources,
			spinnerPath, kSpinnerSize, spinnerWidth, spinnerHeight, error);
		if (spinnerTexture == kInvalidTexture) return false;

		// 그리는 순서 = 추가 순서. 배경이 먼저(뒤), 스피너가 나중(앞).
		UISprite sprite;
		sprite.texture = backgroundTexture;
		background = ui.AddSprite(sprite);

		sprite.texture = spinnerTexture;
		spinner = ui.AddSprite(sprite);

		Layout(renderer.Width(), renderer.Height());
		return true;
	}

	void LoadingScene::Layout(uint32_t width, uint32_t height)
	{
		const float w = float(width);
		const float h = float(height);

		// 배경 — 비율을 지키며 창을 꽉 채우고(cover) 넘치는 쪽을 가운데 기준으로 자른다.
		UISprite& bg = ui.Sprite(background);
		bg.center = { w * 0.5f, h * 0.5f };
		bg.halfSize = { w * 0.5f, h * 0.5f };

		const float windowAspect = w / h;
		const float imageAspect = float(backgroundWidth) / float(backgroundHeight);
		if (imageAspect > windowAspect)
		{
			const float visible = windowAspect / imageAspect;      // 가로로 보이는 비율
			bg.uvMin = { 0.5f - visible * 0.5f, 0.0f };
			bg.uvMax = { 0.5f + visible * 0.5f, 1.0f };
		}
		else
		{
			const float visible = imageAspect / windowAspect;      // 세로로 보이는 비율
			bg.uvMin = { 0.0f, 0.5f - visible * 0.5f };
			bg.uvMax = { 1.0f, 0.5f + visible * 0.5f };
		}

		// 스피너 — 오른쪽 아래. 고리 바깥이 가장자리에서 kSpinnerMargin 떨어지게.
		UISprite& sp = ui.Sprite(spinner);
		const float inset = kSpinnerMargin + kSpinnerRingRadius;
		sp.center = { w - inset, h - inset };
		sp.halfSize = { kSpinnerSize * 0.5f, kSpinnerSize * 0.5f };
	}

	void LoadingScene::Update(float dt)
	{
		// 한 바퀴마다 시간을 되감아 float 정밀도가 무너지지 않게 한다.
		const float period = 1.0f / kSpinnerTurnsPerSecond;
		spinTime = std::fmod(spinTime + dt, period);

		const float stepsPerSecond = kSpinnerTurnsPerSecond * float(kSpinnerSpokes);
		const float step = std::floor(spinTime * stepsPerSecond);
		const float stepAngle = 2.0f * kPi / float(kSpinnerSpokes);

		ui.Sprite(spinner).rotation = step * stepAngle;
	}

} // namespace swc
