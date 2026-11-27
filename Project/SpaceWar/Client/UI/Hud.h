#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Client/Handles.h"
#include "Client/UISprite.h"
#include "Client/Vec3d.h"
#include "Client/UI/FontAtlas.h"

// ============================================================
//  Client/UI/Hud.h — 게임 화면 기본 HUD (2026-11-26)
//
//  범위(2026-11-26 사용자 «HP, 장탄수, 방탄, 화면 상단의 방위»): 체력 · 방탄판 · 장탄수 · 나침반.
//  ★ 2026-11-27 재장전 표시를 더했다(사용자 «무기에 현재 탄수를 추가해서 장전과 UI 연결을 구현해줘») — 팀원 명세 02_동적_상태와_계산식.md
//    «재장전 표시: 상태 우선순위»: 장전 중(문구 · 진행률 · 퍼센트) → [키] 재장전(깜빡임) → 숨김.
//    (명세의 «탄약 없음» · «예비 탄약 없음» 은 예비탄 0 일 때라 같은 날 예비탄과 함께 뺐다 — 사용자 «예비탄없애고 … 몇 번이고 장전가능하게 할거야».)
//    자리 · 크기 = hud-theme.json reload, 색 · 글자 크기 = 00_게임용_UI_안내.html(.reload · .key · .progress).
//  데미지 숫자 · 시안의 나머지(분대 · 성장 · 스킬 · 목표 · 함선)는 넣지 않았다.
//
//  ★ 근거 = 팀원 HUD 패키지(resource 1a567b7, docs/UI 아이콘/게임용_기본HUD)
//    배치 · 색 · 크기 = 03_동적_UI_구현명세/hud-theme.json, 계산 = 02_동적_상태와_계산식.md,
//    기호 영역 = 02_기호_이미지/sprite-regions.json. 값은 Hud.cpp 상수로 옮겼다(JSON 을 읽지 않는다).
//    리소스 = 실행 때 exe 옆 assets/ui/hud/ (기호 PNG 2장) · assets/ui/hud/font/ (TTF 6개 + 라이선스) —
//    빌드가 위 패키지 02_기호_이미지 · 01_글꼴_TTF · 99_라이선스_출처 에서 복사한다(Client.vcxproj CopyModelAssets, 저장소에 사본 없음).
//  ★ 화면 배율 — 1920×1080 기준 좌표를 uniform scale = min(창 폭/1920, 창 높이/1080) 로 줄이고 남는 여백을 양쪽에 나눈다.
//    글자 크기 · 선 굵기도 같은 배율. 창 크기는 Initialize 때 한 번 받는다(지금 창은 크기가 바뀌지 않는다).
//  ★ 실행 위치 — 상태는 메인(Engine 이 State 를 채운다), 그리기는 렌더 추출 ④ 때 UISprite 로 복사(Extract).
//    명세 5절 «UI 도 Scene 에 귀속» — 게임 씬은 아직 Engine 이 직접 돌아서(SceneManager.h) 크로스헤어처럼 Engine 이 든다.
//  ★ «값은 임시»(2026-11-26 사용자) — 방탄판은 게임에 아직 값이 없어 HudState 기본값(팀원 runtime-state 예시)을 보인다.
//    장탄수 · 장전은 2026-11-27 부터 내 든 총(Engine::localPlayer.weapon — Shared::Weapon)의 실제 값이다.
//    장탄수 = «현재 장탄수 / 최대 장탄수»(예 «08 / 35» — 2026-11-27 사용자, 시안 «08 / 096» 의 뒤 숫자였던 예비탄은 뺐다).
//    장탄수 숫자는 한 글자씩 같은 폭 칸에 그린다(2026-11-27 — 탄이 줄 때 « / » · 뒤 숫자가 흔들리지 않게, AppendFixedDigits).
//    체력 · 최대 체력은 내 Player(Shared::DynamicObject health · maxHealth — Engine::localPlayer)를 쓴다(2026-11-26).
//    health = 서버가 판정한 값(모르면 최대값), maxHealth = Shared::Player 기본값 100(서버 Shared::kMaxHealth 와 같은 한 값 — 서버가 따로 보내지 않는다).
// ============================================================

namespace swc {

	class GRenderer;
	class ResourceManager;

	// HUD 에 보일 값 — Engine 이 매 프레임 채운다(메인 스레드).
	//  기본값 = 팀원 runtime-state.example.json 의 예시(체력 82 는 서버 값으로 덮인다).
	struct HudState
	{
		float    health = 100.0f;
		float    maxHealth = 100.0f;          // 2026-11-26 — Engine 이 매 프레임 내 Player 의 maxHealth 로 덮는다(이 기본값은 덮기 전 값)
		float    armor = 64.0f;           // ★ 임시 — 게임에 방탄판이 없다
		float    maxArmor = 100.0f;
		float    headingDegrees = 0.0f;   // 나침반 — 북 0 · 동 90 · 남 180 · 서 270 (HeadingDegrees)

		// ── 장탄수 · 재장전 (2026-11-27) — Engine 이 매 프레임 내 든 총(Shared::Weapon)에서 채운다. 팀원 runtime-state «weapon» 과 같은 칸 ──
		bool         hasWeapon = false;       // 빈손이면 장탄수 · 재장전 표시를 그리지 않는다
		uint32_t     ammo = 0;                // 현재 탄수(탄창에 남은 탄)
		uint32_t     magazineSize = 0;        // 탄창 크기(capacity) = 최대 장탄수 — «현재 / 최대» 의 뒤 숫자(예비탄 칸은 2026-11-27 뺐다)
		bool         reloading = false;       // isReloading
		float        reloadElapsed = 0.0f;    // reloadElapsedSeconds
		float        reloadDuration = 0.0f;   // reloadDurationSeconds
		std::wstring reloadKey = L"R";        // 장전 키 이름 — 실제 입력 키(Engine kReloadKey)
		float        timeSeconds = 0.0f;      // 안내 깜빡임 시계
	};

	class Hud
	{
	public:
		using AssetPathFn = std::wstring(*)(const wchar_t* relative);

		// 글자 그림판 · 기호 텍스처 · 흰 사각형 텍스처를 만들어 GPU 에 올린다. 게임 리소스 로드가 끝난 뒤 메인에서 한 번.
		//  글꼴을 못 읽으면 false(HUD 를 그리지 않는다). 기호 · 빠진 글자처럼 일부만 실패하면 true + warning.
		bool Initialize(GRenderer& renderer, ResourceManager& resources, AssetPathFn assetPath,
			uint32_t viewportWidth, uint32_t viewportHeight, std::wstring& warning);

		bool Ready() const { return font.Ready() && white != kInvalidTexture; }

		HudState& State() { return state; }
		const HudState& State() const { return state; }

		// 렌더 추출 ④ — 그리는 순서(기호 → 빈 칸 → 채움 → 글자, 팀원 계산식 문서 «표시 순서»)대로 out 뒤에 붙인다.
		void Extract(std::vector<UISprite>& out) const;

		// 행성 위 나침반 각도(도, 0 ~ 360). up = 그 자리의 위(단위), forward = 접평면 앞(단위, Camera::Forward).
		//  북쪽 = 월드 +Y 극 쪽을 이 자리의 접평면에 내린 방향, 동쪽 = 북을 바라볼 때 오른쪽(Camera::Right 와 같은 외적).
		//  극 바로 위처럼 북쪽이 정해지지 않으면 previous 를 그대로 돌려준다.
		//  ★ 2026-11-26 보고한 기본안이다. 북쪽 정의가 바뀌면 이 함수만 고친다.
		static float HeadingDegrees(const Vec3d& up, const Vec3d& forward, float previous);

	private:
		void PushRect(std::vector<UISprite>& out, float left, float top, float width, float height,
			const DirectX::XMFLOAT4& color) const;
		void PushLine(std::vector<UISprite>& out, float x0, float y0, float x1, float y1, float thickness,
			const DirectX::XMFLOAT4& color) const;
		void PushIcon(std::vector<UISprite>& out, TextureHandle icon, float aspect,
			float boxLeft, float boxTop, float boxWidth, float boxHeight) const;
		void PushFrame(std::vector<UISprite>& out, float left, float top, float width, float height, float thickness,
			const DirectX::XMFLOAT4& color) const;        // 사각형 테두리(키캡)
		void ExtractAmmo(std::vector<UISprite>& out) const;   // 장탄수 · 재장전 표시(2026-11-27)
		// 한 글자씩 따로 그린다 — 숫자는 digitWidth 칸 가운데, 나머지는 제 폭. 돌려주는 값 = 줄 오른쪽 끝(2026-11-27 숫자 고정 폭)
		float AppendFixedDigits(std::vector<UISprite>& out, FontAtlas::StyleId style, const std::wstring& text,
			float left, float centerY, float digitWidth, const DirectX::XMFLOAT4& color) const;

		HudState      state;
		FontAtlas     font;
		FontAtlas::StyleId labelStyle = 0;      // 한글 이름(체력 · 방탄판)
		FontAtlas::StyleId numberStyle = 0;     // 체력 · 방탄판 수치, 장탄수
		FontAtlas::StyleId compassStyle = 0;    // 나침반 숫자 · 현재 각도
		FontAtlas::StyleId cardinalStyle = 0;   // 나침반 방위 N · NE …
		FontAtlas::StyleId reloadLabelStyle = 0;    // 재장전 · 재장전 중(2026-11-27)
		FontAtlas::StyleId reloadKeyStyle = 0;      // 키캡 안 키 이름(2026-11-27)
		FontAtlas::StyleId reloadNumberStyle = 0;   // 진행 퍼센트(2026-11-27)
		float              ammoDigitWidth = 0.0f;   // 장탄수 숫자 칸 폭(px) = numberStyle 의 0 ~ 9 중 가장 넓은 폭 — Initialize 가 Bake 뒤에 잰다(2026-11-27)

		TextureHandle white = kInvalidTexture;        // 사각형 · 선 (흰 텍스처 × tint)
		TextureHandle healthIcon = kInvalidTexture;   // 체력 십자 (hp-base.png 기호 영역만)
		TextureHandle armorIcon = kInvalidTexture;    // 방탄판 방패 (armor-base.png 기호 영역만)
		float         healthIconAspect = 1.0f;        // 폭 / 높이 — 기호 영역 그대로(종횡비 유지)
		float         armorIconAspect = 1.0f;

		float scale = 1.0f;     // 1920×1080 → 창
		float offsetX = 0.0f;   // 남는 여백(양쪽에 나눔)
		float offsetY = 0.0f;
	};

} // namespace swc
