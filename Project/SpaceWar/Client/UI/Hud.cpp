#include "Hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Client/GRenderer.h"
#include "Client/Resource/ResourceManager.h"

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT4;

namespace {

	// ── 팀원 HUD 패키지 값 (resource 1a567b7, docs/UI 아이콘/게임용_기본HUD/03_동적_UI_구현명세/hud-theme.json) ──
	//  좌표는 1920×1080 기준 px.
	//  ★ 테마 JSON 의 «…Center» 좌표는 x = 왼쪽 끝(수치는 오른쪽 끝) · y = 세로 가운데로 읽어야 시안 픽셀과 맞는다
	//    (2026-11-26 시안 픽셀 실측 — 막대 x 176 ~ 365, 채움 끝 방탄 297 · 체력 331 = 176 + 190 × 0.64 · 0.82).
	constexpr float kRefWidth = 1920.0f;
	constexpr float kRefHeight = 1080.0f;

	// ── 글꼴 — 6 개 모두 들인다(2026-11-26 사용자 «그거 제작한폰트야»). 원본은 팀원 패키지 docs/UI 아이콘/게임용_기본HUD/01_글꼴_TTF ·
	//  기호는 02_기호_이미지 — 빌드 때 Client.vcxproj CopyModelAssets 가 exe 옆 assets/ui/hud(· font)로 복사한다(저장소에 사본 없음).
	//  체력 · 방탄판 · 장탄수 · 나침반의 시안 글자 모양 = 숫자 · 방위 Rajdhani, 한글 Pretendard(팀원 안내 01_최종_UI_목록 «글꼴 지정» 과 같다).
	//  시안 굵기가 Regular 보다 굵어서 Medium 을 쓴다(2026-11-26 비교 — Resource_Work/HUD/글꼴비교_20261126).
	//  Orbitron · ChakraPetch 는 이 네 요소의 시안 글자와 모양이 다르다(같은 비교). 쓰는 자리가 정해지면 아래 줄만 바꾼다.
	constexpr const wchar_t* kFontPretendardRegular = L"ui\\hud\\font\\Pretendard-Regular.ttf";
	constexpr const wchar_t* kFontPretendardMedium = L"ui\\hud\\font\\Pretendard-Medium.ttf";
	constexpr const wchar_t* kFontRajdhaniRegular = L"ui\\hud\\font\\Rajdhani-Regular.ttf";
	constexpr const wchar_t* kFontRajdhaniMedium = L"ui\\hud\\font\\Rajdhani-Medium.ttf";
	constexpr const wchar_t* kFontOrbitronMedium = L"ui\\hud\\font\\Orbitron-Medium.ttf";
	constexpr const wchar_t* kFontChakraPetchMedium = L"ui\\hud\\font\\ChakraPetch-Medium.ttf";

	constexpr const wchar_t* kLabelFont = kFontPretendardMedium;   // 한글 이름
	constexpr const wchar_t* kDigitFont = kFontRajdhaniMedium;     // 숫자 · 방위

	constexpr float kBodySize = 21.0f;       // fontUsage.bodySize — 한글 이름
	constexpr float kNumberSize = 30.0f;     // fontUsage.numberSize — 체력 · 방탄판 수치, 장탄수
	constexpr float kCompassSize = 19.0f;    // 나침반 숫자 · 현재 각도 (안내 HTML 캔버스 19px)
	constexpr float kCardinalSize = 24.0f;   // 나침반 방위 (안내 HTML 캔버스 24px)

	// 그림판에 넣을 글자. 한글은 쓰는 글자만 넣는다(새 문구 = 여기에 글자를 더한다).
	//  소스 인코딩에 기대지 않게 한글은 \u 로 적는다: 체 력 방 탄 판
	constexpr const wchar_t* kLabelChars = L"체력방탄판";
	constexpr const wchar_t* kHealthLabel = L"체력";          // 체력
	constexpr const wchar_t* kArmorLabel = L"방탄판";     // 방탄판
	constexpr const wchar_t* kDegreeSign = L"°";                 // °

	// ── 색 (colors) — 스왑체인이 UNORM 이라 sRGB 값을 그대로 쓴다(LoadingScene.cpp 와 같음) ──
	constexpr uint32_t kForeground = 0xECF3F6;
	constexpr uint32_t kMuted = 0x8799A6;     // 빈 칸 — 시안 실측 #8698a5 와 같다(불투명)
	constexpr uint32_t kArmorColor = 0x8EDFE7;
	constexpr uint32_t kWarning = 0xDCB36B;   // 나침반 중앙 표시 · 현재 각도
	constexpr float    kCompassLineAlpha = 0.6f;   // 나침반 기준선 · 눈금 (안내 HTML #ecf3f699)

	XMFLOAT4 Color(uint32_t rgb, float alpha = 1.0f)
	{
		return XMFLOAT4(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
			float(rgb & 0xFF) / 255.0f, alpha);
	}

	// ── 체력 · 방탄판 (health · armor) ──
	struct VitalLayout
	{
		float iconBox[4];   // iconBox — 왼쪽 · 위 · 폭 · 높이
		float labelLeft;    // labelCenter.x
		float centerY;      // 줄의 세로 가운데
	};
	constexpr VitalLayout kArmorRow = { { 48.0f, 852.0f, 22.0f, 24.0f }, 84.0f, 862.0f };
	constexpr VitalLayout kHealthRow = { { 48.0f, 887.0f, 22.0f, 22.0f }, 84.0f, 899.0f };
	constexpr float kBarLeft = 176.0f;      // barCenter.x
	constexpr float kBarWidth = 190.0f;     // barWidth
	constexpr float kBarHeight = 7.0f;      // geometry.barHeight
	constexpr float kValueRight = 424.0f;   // valueRightCenter.x
	constexpr float kLineWidth = 1.0f;      // geometry.lineWidth

	// 구분선 — 테마 JSON 에 없다. 시안에서 잼(x 48 ~ 396, y 926 ~ 927, 글자 색).
	constexpr float kRuleLeft = 48.0f;
	constexpr float kRuleRight = 396.0f;
	constexpr float kRuleY = 926.5f;

	// ── 장탄수 — reload.ammoCountLeftCenter ──
	//  ★ 형식 = «현재 장탄수 / 최대 장탄수»(2026-11-27 사용자 «예비탄없애고 최대 장탄수만 나오게 해 … 현재 장탄수 / 최대 장탄수 이거야») —
	//    예: 라이플 «08 / 10» · 권총 «12 / 12» · 기관총 «099 / 100». 최대 = 그 총의 탄창(WeaponSpec::magazineSize).
	//    (그 전 시안 «08 / 096» 의 뒤 숫자는 예비탄이었다 — 예비탄은 Shared::Weapon 에서도 뺐다. 권총 무한 «∞» 도 함께 뺐다.)
	constexpr float kAmmoLeft = 143.0f;
	constexpr float kAmmoCenterY = 960.0f;
	//  ★ 숫자 고정 폭(2026-11-27 사용자 «남은 장탄수에 따라 텍스트 위치가 조금씩 바뀌는데 보정해줄 수 있어? 각 숫자들 따로 그리면서») —
	//    Rajdhani 숫자는 글자마다 폭이 달라(Medium 30 px 에서 1 = 9.7 · 7 = 11.9 · 0 = 15.6 · 4 · 8 = 15.8 px — hmtx 실측)
	//    통째로 그리면 탄이 줄 때 « / » 와 뒤 숫자가 흔들렸다.
	//    숫자(0 ~ 9)를 한 글자씩 따로, 같은 폭 칸(0 ~ 9 중 가장 넓은 폭 — Hud::ammoDigitWidth) 가운데에 그린다(AppendFixedDigits).
	//    자릿수도 고정 — 현재 · 최대 모두 탄창 크기의 자릿수(최소 2, 시안 «08»).
	//    그래서 기관총(탄창 100)은 «100» 다음이 «099» 다(«99» 로 줄면 « / » 가 한 칸 당겨진다).
	constexpr int kAmmoMinDigits = 2;

	int DigitCount(uint32_t value)
	{
		int count = 1;
		while (value >= 10) { value /= 10; ++count; }
		return count;
	}

	// ── 재장전 표시 (2026-11-27, 사용자 «무기에 현재 탄수를 추가해서 장전과 UI 연결을 구현해줘») ──
	//  자리 · 크기 = hud-theme.json reload(…LeftCenter 의 x = 왼쪽 끝 · …RightCenter 의 x = 오른쪽 끝, y = 세로 가운데 — 위 체력 줄과 같은 읽기).
	//  색 · 글자 크기 = 00_게임용_UI_안내.html — .reload(문구 #dcb36b · 19 px) · .key(테두리 1 px #dcb36b · Rajdhani 23 px · 좌우 8 px) ·
	//   진행률 카드(«재장전 중» #ecf3f6, 퍼센트 .reload 색, 막대 높이 7 px · 빈 칸 #8799a6 × 0.32 · 채움 #dcb36b).
	//  상태 · 계산 = 02_동적_상태와_계산식.md «재장전 표시: 상태 우선순위»(ExtractAmmo).
	//   ★ 예비탄이 없어(2026-11-27 사용자 «몇 번이고 장전가능하게 할거야») 명세의 «탄약 없음» · «예비 탄약 없음»(예비탄 0 일 때)은 나올 일이 없어 뺐다.
	//  글꼴 굵기는 위 네 요소처럼 Medium(2026-11-26 시안 비교 — 안내 HTML 은 Regular).
	constexpr float kPromptLeft = 333.0f;          // reload.promptLeftCenter
	constexpr float kPromptCenterY = 960.0f;
	constexpr float kKeycapHeight = 30.0f;         // reload.keycapHeight
	constexpr float kKeycapMinWidth = 27.0f;       // reload.keycapMinWidth
	constexpr float kKeycapMaxWidth = 100.0f;      // reload.keycapMaxWidth
	constexpr float kKeycapPaddingX = 8.0f;        // reload.keycapPaddingX
	constexpr float kKeyLabelGap = 10.0f;          // reload.keyLabelGap
	constexpr float kProgressLeft = 333.0f;        // reload.progressBarLeftCenter
	constexpr float kProgressCenterY = 988.0f;
	constexpr float kProgressWidth = 220.0f;       // reload.progressWidth
	constexpr float kPercentRight = 553.0f;        // reload.percentRightCenter
	constexpr float kPercentCenterY = 960.0f;
	constexpr float kLowAmmoPercent = 30.0f;       // reload.lowAmmoPercent — 탄창의 이만큼 이하면 «[R] 재장전»
	constexpr float kPulseFrequency = 1.5f;        // reload.pulseFrequency (Hz)
	constexpr float kPulseMinOpacity = 0.72f;      // reload.pulseMinOpacity
	constexpr float kEmptyTrackOpacity = 0.32f;    // geometry.emptyTrackOpacity — 진행률 빈 칸
	constexpr float kReloadTextSize = 19.0f;       // 안내 HTML .reload font-size — 문구 · 퍼센트
	constexpr float kReloadKeySize = 23.0f;        // 안내 HTML .key font-size — 키 이름
	constexpr const wchar_t* kReloadPrompt = L"재장전";            // reload.prompt
	constexpr const wchar_t* kReloadingLabel = L"재장전 중";       // reload.progressLabel
	constexpr const wchar_t* kReloadChars = L"재장전 중";          // 위 두 문구의 글자(띄어쓰기 포함 — 없으면 «재장전중» 으로 붙는다)

	// ── 나침반 (compass) ──
	constexpr float kCompassCenterX = 960.0f;
	constexpr float kCompassClipLeft = 680.0f;    // clipRect [680, 8, 560, 50]
	constexpr float kCompassClipWidth = 560.0f;
	constexpr float kPixelsPerDegree = 4.0f;
	constexpr int   kTickEveryDegrees = 5;
	constexpr int   kLabelEveryDegrees = 15;
	constexpr int   kCardinalEveryDegrees = 45;
	constexpr float kCompassLabelY = 26.0f;
	constexpr float kCompassBaselineY = 48.0f;
	constexpr float kCompassHeadingY = 64.0f;
	// 눈금 · 중앙 표시 — 테마 JSON 에 없어 안내 HTML 캔버스 값(캔버스 x 0 = 화면 680)
	constexpr float kTickTop = 42.0f;
	constexpr float kTickMinorBottom = 49.0f;
	constexpr float kTickMajorBottom = 56.0f;
	constexpr float kChevronHalfWidth = 5.0f;   // (955, 5) → (960, 10) → (965, 5)
	constexpr float kChevronTop = 5.0f;
	constexpr float kChevronBottom = 10.0f;
	constexpr const wchar_t* kDirections[8] = { L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW" };

	// ── 기호 영역 — sprite-regions.json (left · top 포함, right · bottom 제외) ──
	struct SymbolRegion
	{
		const wchar_t* file;
		uint32_t left, top, right, bottom;
	};
	constexpr SymbolRegion kHealthSymbol = { L"ui\\hud\\hp-base.png", 335, 304, 449, 414 };
	constexpr SymbolRegion kArmorSymbol = { L"ui\\hud\\armor-base.png", 375, 300, 489, 418 };

	std::wstring AsciiChars()
	{
		std::wstring chars;
		for (wchar_t c = 0x20; c < 0x7F; ++c) chars.push_back(c);
		return chars;
	}

	float Normalize360(float degrees)
	{
		const float r = std::fmod(degrees, 360.0f);
		return r < 0.0f ? r + 360.0f : r;
	}

	float Ratio(float current, float maximum)
	{
		return maximum > 0.0f ? std::clamp(current / maximum, 0.0f, 1.0f) : 0.0f;
	}

	// 2x2 상자 필터로 반씩 줄인다(LoadingScene.cpp HalveRgba8 와 같은 식 — premultiplied 라 그대로 평균해도 된다).
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

	// 기호 텍스처 — 원본 PNG 에서 기호 영역만 잘라(팀원 안내 «PNG 전체를 UI 로 표시하지 마세요») premultiplied 로 바꾸고,
	//  그릴 크기의 2 배 근처까지 반씩 줄인다(렌더러 텍스처에 밉이 없다 — LoadingScene.cpp PrepareForUI 와 같은 이유).
	//  원본 파일은 바꾸지 않는다. GPU 에 올린 뒤 RAM 사본은 버린다(명세 4.2).
	bool MakeSymbolTexture(swc::GRenderer& renderer, swc::ResourceManager& resources, const std::wstring& path,
		const SymbolRegion& region, float drawSize, swc::TextureHandle& texture, float& aspect, std::wstring& error)
	{
		const swc::TextureDataHandle data = resources.LoadTexture(path.c_str(), false);   // UI 는 UNORM — sRGB 변환 없이
		const swc::TextureData* loaded = resources.Get(data);
		if (!loaded)
		{
			error = path + L": " + resources.LastError();
			return false;
		}
		if (loaded->format != swc::TextureFormat::Rgba8 || loaded->arraySize != 1 ||
			region.right > loaded->width || region.bottom > loaded->height ||
			region.left >= region.right || region.top >= region.bottom)
		{
			error = path + L": 기호 영역이 이미지 밖이거나 RGBA8 이 아니다";
			resources.Release(data);
			return false;
		}

		swc::TextureData image;
		image.path = path + L" (기호 영역)";
		image.width = region.right - region.left;
		image.height = region.bottom - region.top;
		image.srgb = false;
		image.pixels.resize(size_t(image.width) * image.height * 4);
		for (uint32_t row = 0; row < image.height; ++row)
			std::memcpy(image.pixels.data() + size_t(row) * image.width * 4,
				loaded->pixels.data() + (size_t(region.top + row) * loaded->width + region.left) * 4,
				size_t(image.width) * 4);
		resources.Release(data);

		aspect = float(image.width) / float(image.height);

		for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
		{
			const unsigned alpha = image.pixels[i + 3];
			for (size_t ch = 0; ch < 3; ++ch)
				image.pixels[i + ch] = uint8_t((image.pixels[i + ch] * alpha + 127) / 255);
		}
		while (float((std::max)(image.width, image.height)) > drawSize * 2.0f && image.width > 1 && image.height > 1)
			HalveRgba8(image);

		texture = renderer.CreateTexture(image);
		if (texture == swc::kInvalidTexture)
		{
			error = path + L": " + renderer.StatusText();
			return false;
		}
		return true;
	}

} // namespace

namespace swc {

	bool Hud::Initialize(GRenderer& renderer, ResourceManager& resources, AssetPathFn assetPath,
		uint32_t viewportWidth, uint32_t viewportHeight, std::wstring& warning)
	{
		warning.clear();

		// uniform-fit — 남는 여백은 양쪽에 같게
		scale = (std::min)(float(viewportWidth) / kRefWidth, float(viewportHeight) / kRefHeight);
		offsetX = (float(viewportWidth) - kRefWidth * scale) * 0.5f;
		offsetY = (float(viewportHeight) - kRefHeight * scale) * 0.5f;

		// ① 글자 그림판 — 크기에 배율을 곱해 그 크기로 그린다(확대 · 축소로 흐려지지 않게)
		font = FontAtlas{};
		std::wstring digitChars = AsciiChars() + kDegreeSign;
		labelStyle = font.AddStyle(assetPath(kLabelFont), kBodySize * scale, kLabelChars);
		numberStyle = font.AddStyle(assetPath(kDigitFont), kNumberSize * scale, digitChars);
		compassStyle = font.AddStyle(assetPath(kDigitFont), kCompassSize * scale, digitChars);
		cardinalStyle = font.AddStyle(assetPath(kDigitFont), kCardinalSize * scale, digitChars);
		// 2026-11-27 재장전 표시(권총 무한 «∞» 모양은 같은 날 예비탄과 함께 뺐다)
		reloadLabelStyle = font.AddStyle(assetPath(kLabelFont), kReloadTextSize * scale, kReloadChars);
		reloadKeyStyle = font.AddStyle(assetPath(kDigitFont), kReloadKeySize * scale, AsciiChars());
		reloadNumberStyle = font.AddStyle(assetPath(kDigitFont), kReloadTextSize * scale, digitChars);

		TextureData atlas;
		std::wstring missing;
		if (!font.Bake(atlas, warning, missing))
			return false;
		const TextureHandle atlasTexture = renderer.CreateTexture(atlas);
		if (atlasTexture == kInvalidTexture)
		{
			warning = L"글자 그림판을 GPU 에 올리지 못했다 — " + renderer.StatusText();
			return false;
		}
		font.SetTexture(atlasTexture);
		if (!missing.empty())
			warning += L"글꼴에 없는 글자(그리지 않음): " + missing + L" ";

		// 장탄수 숫자 칸 폭 = 0 ~ 9 중 가장 넓은 글자 폭(2026-11-27 숫자 고정 폭 — 글자 폭은 Bake 뒤에 안다)
		ammoDigitWidth = 0.0f;
		for (wchar_t digit = L'0'; digit <= L'9'; ++digit)
			ammoDigitWidth = (std::max)(ammoDigitWidth, font.Measure(numberStyle, std::wstring(1, digit)));

		// ② 사각형 · 선 — 흰 텍스처 × tint
		TextureData whiteImage;
		whiteImage.path = L"(HUD 흰 사각형)";
		whiteImage.width = 4;
		whiteImage.height = 4;
		whiteImage.srgb = false;
		whiteImage.pixels.assign(4 * 4 * 4, 255);
		white = renderer.CreateTexture(whiteImage);
		if (white == kInvalidTexture)
		{
			warning = L"흰 사각형 텍스처를 GPU 에 올리지 못했다 — " + renderer.StatusText();
			return false;
		}

		// ③ 기호 — 못 읽으면 그 기호만 빠진다
		std::wstring symbolError;
		const float healthDraw = (std::max)(kHealthRow.iconBox[2], kHealthRow.iconBox[3]) * scale;
		if (!MakeSymbolTexture(renderer, resources, assetPath(kHealthSymbol.file), kHealthSymbol, healthDraw,
			healthIcon, healthIconAspect, symbolError))
			warning += L"체력 기호: " + symbolError + L" ";
		const float armorDraw = (std::max)(kArmorRow.iconBox[2], kArmorRow.iconBox[3]) * scale;
		if (!MakeSymbolTexture(renderer, resources, assetPath(kArmorSymbol.file), kArmorSymbol, armorDraw,
			armorIcon, armorIconAspect, symbolError))
			warning += L"방탄판 기호: " + symbolError + L" ";
		return true;
	}

	float Hud::HeadingDegrees(const Vec3d& up, const Vec3d& forward, float previous)
	{
		const Vec3d axis{ 0.0, 1.0, 0.0 };   // 북극 = 월드 +Y 쪽
		Vec3d north = axis - up * Dot(axis, up);
		const double length = Length(north);
		if (length < 1.0e-6) return previous;   // 극 바로 위 — 북쪽이 정해지지 않는다
		north = north * (1.0 / length);
		const Vec3d east = Cross(up, north);   // Camera::Right() = Cross(up, forward) 와 같은 식
		const double radians = std::atan2(Dot(forward, east), Dot(forward, north));
		const double degrees = radians * (180.0 / 3.14159265358979323846);
		return float(degrees < 0.0 ? degrees + 360.0 : degrees);
	}

	void Hud::PushRect(std::vector<UISprite>& out, float left, float top, float width, float height,
		const XMFLOAT4& color) const
	{
		if (width <= 0.0f || height <= 0.0f) return;
		UISprite sprite;
		sprite.texture = white;
		sprite.center = XMFLOAT2(left + width * 0.5f, top + height * 0.5f);
		sprite.halfSize = XMFLOAT2(width * 0.5f, height * 0.5f);
		sprite.tint = color;
		out.push_back(sprite);
	}

	void Hud::PushLine(std::vector<UISprite>& out, float x0, float y0, float x1, float y1, float thickness,
		const XMFLOAT4& color) const
	{
		const float dx = x1 - x0, dy = y1 - y0;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (length <= 0.0f) return;
		UISprite sprite;
		sprite.texture = white;
		sprite.center = XMFLOAT2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
		sprite.halfSize = XMFLOAT2(length * 0.5f, thickness * 0.5f);
		sprite.rotation = std::atan2(dy, dx);   // UI.hlsl — 화면(+y 아래)에서 시계 방향
		sprite.tint = color;
		out.push_back(sprite);
	}

	// 사각형 테두리 — 위 · 아래 변은 전체 폭, 왼쪽 · 오른쪽 변은 그 사이만(모서리가 겹쳐 반투명일 때 진해지지 않게).
	void Hud::PushFrame(std::vector<UISprite>& out, float left, float top, float width, float height, float thickness,
		const XMFLOAT4& color) const
	{
		if (width <= 0.0f || height <= 0.0f || thickness <= 0.0f) return;
		PushRect(out, left, top, width, thickness, color);
		PushRect(out, left, top + height - thickness, width, thickness, color);
		PushRect(out, left, top + thickness, thickness, height - 2.0f * thickness, color);
		PushRect(out, left + width - thickness, top + thickness, thickness, height - 2.0f * thickness, color);
	}

	// 글자 줄을 한 글자씩 따로 그린다(2026-11-27 숫자 고정 폭) — 숫자(0 ~ 9)는 digitWidth 칸 가운데, 나머지 글자(띄어쓰기 · /)는 제 폭.
	//  left = 줄 왼쪽 끝(px). 돌려주는 값 = 줄 오른쪽 끝(px) — 다음 글자를 이어 그릴 자리.
	float Hud::AppendFixedDigits(std::vector<UISprite>& out, FontAtlas::StyleId style, const std::wstring& text,
		float left, float centerY, float digitWidth, const XMFLOAT4& color) const
	{
		float x = left;
		for (const wchar_t ch : text)
		{
			const std::wstring one(1, ch);
			if (ch >= L'0' && ch <= L'9')
			{
				font.AppendText(out, style, one, x + digitWidth * 0.5f, centerY, FontAtlas::Align::Center, color);
				x += digitWidth;
			}
			else
			{
				font.AppendText(out, style, one, x, centerY, FontAtlas::Align::Left, color);
				x += font.Measure(style, one);
			}
		}
		return x;
	}

	// 장탄수 · 재장전 표시 (2026-11-27) — 팀원 명세 02_동적_상태와_계산식.md «재장전 표시: 상태 우선순위»에서 예비탄 상태를 뺀 것:
	//  1. 장전 중 = «재장전 중» + 진행률 막대 + 퍼센트(키 안내 숨김)
	//  (명세 2 «탄약 없음» · 3 «예비 탄약 없음» 은 예비탄 0 일 때라 뺐다 — 같은 날 사용자 «예비탄없애고 … 몇 번이고 장전가능하게 할거야»)
	//  2. 탄 부족 · 장전 가능 = [키] «재장전» (깜빡임 promptAlpha) — 탄창 0 도 여기
	//  3. 나머지 = 숨김. 장탄수 숫자는 늘 보인다(명세 «선택적으로 유지»).
	//  capacity = max(1, 탄창) · threshold = floor(capacity × 30 / 100) · lowAmmo = ammo < capacity 이고 ammo ≤ threshold ·
	//  canReload = 장전 중 아님 · ammo < capacity(예비탄 조건은 뺐다) · progress = clamp(elapsed / duration, 0, 1) · percent = floor(progress × 100).
	//  ★ UI 는 탄을 바꾸지 않는다(명세) — 값은 Engine 이 내 든 총(Shared::Weapon)에서 채운다.
	void Hud::ExtractAmmo(std::vector<UISprite>& out) const
	{
		if (!state.hasWeapon || scale <= 0.0f) return;

		const float s = scale;
		auto X = [this, s](float x) { return offsetX + x * s; };
		auto Y = [this, s](float y) { return offsetY + y * s; };
		const float line = (std::max)(1.0f, kLineWidth * s);
		const XMFLOAT4 foreground = Color(kForeground);
		const XMFLOAT4 warningColor = Color(kWarning);

		// 장탄수 «현재 / 최대» — 예: «08 / 10». 숫자는 한 글자씩 같은 폭 칸에(AppendFixedDigits), 자릿수 고정(kAmmoMinDigits 위 설명).
		wchar_t text[32];
		const int ammoDigits = (std::max)(kAmmoMinDigits, DigitCount((std::max)(state.magazineSize, state.ammo)));
		swprintf_s(text, L"%0*u / %0*u", ammoDigits, unsigned(state.ammo), ammoDigits, unsigned(state.magazineSize));
		AppendFixedDigits(out, numberStyle, text, X(kAmmoLeft), Y(kAmmoCenterY), ammoDigitWidth, foreground);

		const uint32_t capacity = (std::max)(1u, state.magazineSize);
		const uint32_t threshold = uint32_t(std::floor(float(capacity) * std::clamp(kLowAmmoPercent, 0.0f, 100.0f) / 100.0f));
		const bool lowAmmo = state.ammo < capacity && state.ammo <= threshold;
		const bool canReload = !state.reloading && state.ammo < capacity;

		// 1. 장전 중
		if (state.reloading)
		{
			const float progress = state.reloadDuration > 0.0f
				? std::clamp(state.reloadElapsed / state.reloadDuration, 0.0f, 1.0f) : 1.0f;
			font.AppendText(out, reloadLabelStyle, kReloadingLabel, X(kPromptLeft), Y(kPromptCenterY), FontAtlas::Align::Left, foreground);
			swprintf_s(text, L"%d%%", int(std::floor(progress * 100.0f + 0.0000001f)));
			font.AppendText(out, reloadNumberStyle, text, X(kPercentRight), Y(kPercentCenterY), FontAtlas::Align::Right, warningColor);
			const float barHalf = kBarHeight * 0.5f;
			PushRect(out, X(kProgressLeft), Y(kProgressCenterY - barHalf), kProgressWidth * s, kBarHeight * s,
				Color(kMuted, kEmptyTrackOpacity));
			PushRect(out, X(kProgressLeft), Y(kProgressCenterY - barHalf), kProgressWidth * s * progress, kBarHeight * s, warningColor);
			return;
		}

		// 2. [키] 재장전 — promptAlpha = 0.72 + 0.28 × (0.5 + 0.5 × cos(t × 2π × 1.5))
		if (lowAmmo && canReload)
		{
			constexpr float kTwoPi = 6.28318530717958647692f;
			const float pulse = kPulseMinOpacity + (1.0f - kPulseMinOpacity) *
				(0.5f + 0.5f * std::cos(state.timeSeconds * kTwoPi * kPulseFrequency));
			const XMFLOAT4 promptColor = Color(kWarning, pulse);
			const float keyWidth = std::clamp(font.Measure(reloadKeyStyle, state.reloadKey) / s + 2.0f * kKeycapPaddingX,
				kKeycapMinWidth, kKeycapMaxWidth);
			PushFrame(out, X(kPromptLeft), Y(kPromptCenterY - kKeycapHeight * 0.5f), keyWidth * s, kKeycapHeight * s, line, promptColor);
			font.AppendText(out, reloadKeyStyle, state.reloadKey, X(kPromptLeft + keyWidth * 0.5f), Y(kPromptCenterY),
				FontAtlas::Align::Center, promptColor);
			font.AppendText(out, reloadLabelStyle, kReloadPrompt, X(kPromptLeft + keyWidth + kKeyLabelGap), Y(kPromptCenterY),
				FontAtlas::Align::Left, promptColor);
		}
	}

	void Hud::PushIcon(std::vector<UISprite>& out, TextureHandle icon, float aspect,
		float boxLeft, float boxTop, float boxWidth, float boxHeight) const
	{
		if (icon == kInvalidTexture || aspect <= 0.0f) return;
		// 종횡비를 지키며 상자 안에 가운데 맞춤(sprite-regions.json preserveAspectRatio)
		float w = boxWidth, h = boxWidth / aspect;
		if (h > boxHeight) { h = boxHeight; w = boxHeight * aspect; }
		UISprite sprite;
		sprite.texture = icon;
		sprite.center = XMFLOAT2(boxLeft + boxWidth * 0.5f, boxTop + boxHeight * 0.5f);
		sprite.halfSize = XMFLOAT2(w * 0.5f, h * 0.5f);
		out.push_back(sprite);
	}

	void Hud::Extract(std::vector<UISprite>& out) const
	{
		if (!Ready()) return;

		const float s = scale;
		auto X = [this, s](float x) { return offsetX + x * s; };
		auto Y = [this, s](float y) { return offsetY + y * s; };
		const float line = (std::max)(1.0f, kLineWidth * s);   // 1 px 선이 배율로 사라지지 않게

		const XMFLOAT4 foreground = Color(kForeground);
		const XMFLOAT4 muted = Color(kMuted);
		const XMFLOAT4 armorColor = Color(kArmorColor);
		const XMFLOAT4 warningColor = Color(kWarning);
		const XMFLOAT4 compassLine = Color(kForeground, kCompassLineAlpha);

		const float healthRatio = Ratio(state.health, state.maxHealth);
		const float armorRatio = Ratio(state.armor, state.maxArmor);

		// ── ① 기호 ──
		PushIcon(out, armorIcon, armorIconAspect, X(kArmorRow.iconBox[0]), Y(kArmorRow.iconBox[1]),
			kArmorRow.iconBox[2] * s, kArmorRow.iconBox[3] * s);
		PushIcon(out, healthIcon, healthIconAspect, X(kHealthRow.iconBox[0]), Y(kHealthRow.iconBox[1]),
			kHealthRow.iconBox[2] * s, kHealthRow.iconBox[3] * s);

		// ── ② 빈 칸 → ③ 채움 (팀원 계산식: fillWidth = barWidth × clamp(current / maximum, 0, 1), 최대 0 이하면 0) ──
		const float barHalf = kBarHeight * 0.5f;
		PushRect(out, X(kBarLeft), Y(kArmorRow.centerY - barHalf), kBarWidth * s, kBarHeight * s, muted);
		PushRect(out, X(kBarLeft), Y(kArmorRow.centerY - barHalf), kBarWidth * s * armorRatio, kBarHeight * s, armorColor);
		PushRect(out, X(kBarLeft), Y(kHealthRow.centerY - barHalf), kBarWidth * s, kBarHeight * s, muted);
		PushRect(out, X(kBarLeft), Y(kHealthRow.centerY - barHalf), kBarWidth * s * healthRatio, kBarHeight * s, foreground);

		// 구분선 (시안)
		PushRect(out, X(kRuleLeft), Y(kRuleY) - line * 0.5f, (kRuleRight - kRuleLeft) * s, line, foreground);

		// ── ④ 글자 ──
		auto valueText = [](float current, float maximum)
		{
			const float shown = maximum > 0.0f ? std::clamp(current, 0.0f, maximum) : (std::max)(current, 0.0f);
			wchar_t text[16];
			swprintf_s(text, L"%.0f", shown);
			return std::wstring(text);
		};
		font.AppendText(out, labelStyle, kArmorLabel, X(kArmorRow.labelLeft), Y(kArmorRow.centerY), FontAtlas::Align::Left, foreground);
		font.AppendText(out, numberStyle, valueText(state.armor, state.maxArmor), X(kValueRight), Y(kArmorRow.centerY),
			FontAtlas::Align::Right, foreground);
		font.AppendText(out, labelStyle, kHealthLabel, X(kHealthRow.labelLeft), Y(kHealthRow.centerY), FontAtlas::Align::Left, foreground);
		font.AppendText(out, numberStyle, valueText(state.health, state.maxHealth), X(kValueRight), Y(kHealthRow.centerY),
			FontAtlas::Align::Right, foreground);

		// 장탄수 «현재 / 최대» + 재장전 표시 (2026-11-27 — 내 든 총의 실제 값, 빈손이면 그리지 않는다)
		ExtractAmmo(out);

		// ── 나침반 (팀원 계산식: delta = normalize360(tick − yaw + 180) − 180, tickX = centerX + delta × pixelsPerDegree) ──
		{
			const float clipLeft = X(kCompassClipLeft);
			const float clipRight = X(kCompassClipLeft + kCompassClipWidth);
			const float yaw = Normalize360(state.headingDegrees);

			// 기준선
			PushRect(out, clipLeft, Y(kCompassBaselineY) - line * 0.5f, clipRight - clipLeft, line, compassLine);

			for (int angle = 0; angle < 360; angle += kTickEveryDegrees)
			{
				const float delta = Normalize360(float(angle) - yaw + 180.0f) - 180.0f;
				const float x = X(kCompassCenterX + delta * kPixelsPerDegree);
				if (x < clipLeft - 40.0f * s || x > clipRight + 40.0f * s) continue;   // 글자가 걸칠 만큼만 더 본다

				const bool major = angle % kLabelEveryDegrees == 0;
				if (x >= clipLeft && x <= clipRight)
				{
					const float bottom = major ? kTickMajorBottom : kTickMinorBottom;
					PushRect(out, x - line * 0.5f, Y(kTickTop), line, (bottom - kTickTop) * s, compassLine);
				}

				if (major)
				{
					const bool cardinal = angle % kCardinalEveryDegrees == 0;
					const std::wstring label = cardinal ? std::wstring(kDirections[angle / kCardinalEveryDegrees])
						: std::to_wstring(angle);
					font.AppendText(out, cardinal ? cardinalStyle : compassStyle, label, x, Y(kCompassLabelY),
						FontAtlas::Align::Center, foreground, clipLeft, clipRight);
				}
			}

			// 고정 중앙 표시(∨) + 현재 각도 — 표시 영역 밖이라 자르지 않는다
			const float cx = X(kCompassCenterX);
			PushLine(out, cx - kChevronHalfWidth * s, Y(kChevronTop), cx, Y(kChevronBottom), line, warningColor);
			PushLine(out, cx, Y(kChevronBottom), cx + kChevronHalfWidth * s, Y(kChevronTop), line, warningColor);

			const int heading = int(std::lround(yaw)) % 360;
			font.AppendText(out, compassStyle, std::to_wstring(heading) + kDegreeSign, cx, Y(kCompassHeadingY),
				FontAtlas::Align::Center, warningColor);
		}
	}

} // namespace swc
