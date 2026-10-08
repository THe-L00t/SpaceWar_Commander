#include "LoadingScene.h"

#include <cmath>
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

	// RAM 으로 읽어 GPU 로 올리고 RAM 사본은 버린다.
	swc::TextureHandle UploadTexture(swc::GRenderer& renderer, swc::ResourceManager& resources,
		const std::wstring& path, uint32_t& outWidth, uint32_t& outHeight, std::wstring& error)
	{
		const swc::TextureDataHandle data = resources.LoadTexture(path.c_str());
		const swc::TextureData* pixels = resources.Get(data);
		if (!pixels)
		{
			error = FileName(path) + L": " + resources.LastError();
			return swc::kInvalidTexture;
		}

		outWidth = pixels->width;
		outHeight = pixels->height;
		const swc::TextureHandle texture = renderer.CreateTexture(*pixels);
		resources.Release(data);

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
		const TextureHandle backgroundTexture = UploadTexture(renderer, resources,
			backgroundPath, backgroundWidth, backgroundHeight, error);
		if (backgroundTexture == kInvalidTexture) return false;

		const TextureHandle spinnerTexture = UploadTexture(renderer, resources,
			spinnerPath, spinnerWidth, spinnerHeight, error);
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
