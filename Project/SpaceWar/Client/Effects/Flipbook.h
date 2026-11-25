#pragma once
#include <cstdint>
#include <string>
#include "Client/Handles.h"
#include "Client/InstanceData.h"

// ============================================================
//  Client/Effects/Flipbook.h — 파티클 플립북 (2026-11-23)
//
//  사용자(2026-11-23): «파티클 시스템에서 텍스처를 직접 로드하진 않지? 플립북 클래스도 하나 구성하고
//  리소스 매니저에서 읽어 넘겨주면 플립북으로 다룰 수 있도록 해줘».
//
//  역할 나눔(명세 원칙 5 · §4 — Resource Manager 는 RAM 까지, GPU 자원은 Renderer):
//    ResourceManager  — 파일을 읽어 RAM 이미지(TextureData)를 준다(로딩 스레드 → 메인). 텍스처 파일을 읽는 곳은 여기뿐이다.
//    Flipbook         — 받은 RAM 이미지(색 · 노멀)를 Renderer 로 GPU 텍스처로 올리고, 칸 배치 · 프레임 고르기를 든다.
//    ParticleSystem   — 층마다 Flipbook 을 가리키고(AddFlipbook — 2026-11-24 층마다 여러 장), 파티클 나이로 프레임을 골라 패킷에 넣는다. 파일은 모른다.
//    Renderer         — 패킷의 ParticleFlipbook(텍스처 번호 · 칸 배치)을 바인드리스 번호로 바꿔 [7b] 에서 샘플한다.
//
//  텍스처 규칙(Resource_Work/HitParticles/블렌더시험_20261123 의 피 플립북과 같다):
//    · 프레임 = 열 × 행 칸, 왼쪽 위부터 오른쪽 → 다음 줄. 모든 칸 크기가 같다.
//    · 색 = 바탕색(빛을 굽지 않음, sRGB) + 알파. 노멀 = 카메라 공간(X 오른쪽 · Y 위 · Z 카메라 쪽, 선형) + 알파 — 없어도 된다.
//    · 게임이 해 · 하늘빛을 비춘다(2026-11-23 사용자 «게임 빛을 주는 방향으로 가야지»).
// ============================================================

namespace swc {

	class GRenderer;
	struct TextureData;

	struct FlipbookLayout
	{
		uint32_t columns = 1;      // 칸 열 수
		uint32_t rows = 1;         // 칸 행 수
		uint32_t frameCount = 1;   // 쓰는 프레임 수(≤ 열 × 행 — 마지막 줄이 덜 찬 시트)
	};

	// ── 사각형이 보는 쪽 (2026-11-25 사용자 «맞은 지점 기준으로 정면 측면 둘다 출력하면 되잖아. 왜 골라») ──
	//  Camera = 늘 카메라를 본다(예전 그대로 — 먼지 · 연기).
	//  Front · Side = 맞은 자리에 고정한 판. 뿜는 쪽 d = 튀는 쪽(EmitImpact normal — 몸은 쏜 방향의 반대)을 수평으로 눕힌 것, 위 = 그 자리의 행성 위.
	//    Front = d 를 보는 판(노멀 Z = d, 오른쪽 = d × 위 — 쏜 사람에게 그대로 보인다),
	//    Side  = d 를 품은 세로 판(칸 오른쪽 = d, 노멀 Z = 위 × d).
	//  정면 · 측면 시트를 같은 무리(ParticleSystem::AddFlipbook group)로 붙이면 한 번에 둘 다 나와 엇갈려 선다 — 보는 각도에 따라
	//  한 장이 옆면(선)이 되어 저절로 덜 보이므로 고르는 코드가 없다.
	enum class FlipbookFacing : uint8_t { Camera, Front, Side };

	// ── 플립북 재생 설정 (2026-11-25 사용자 «플립북 파티클 클래스에 해당 값도 넣어서 설정할 수 있게해줘») ──
	//  0 · 기본값이면 층 표(ParticleSystem kLayers)의 값을 그대로 쓴다 — 먼지 · 연기 플립북은 예전과 같다.
	//  피 유체 시뮬 시트(Resource_Work/HitParticles/블렌더시험_20261124_유체2/목록.md)의 값: 한 변 1.0 m · 0.4 s · 흰색 ·
	//  상처 자리 정면 (0.5, 0.32) · 측면 (0.15, 0.32) · 맞은 자리에 머묾 · 층 옅어짐 안 씀 · 레이 몫 = 칸 평균 알파.
	struct FlipbookSettings
	{
		FlipbookFacing    facing = FlipbookFacing::Camera;
		float             size = 0.0f;            // 칸 한 변(m). 0 = 층 표 크기(무작위 · 수명 끝 배율). 0 보다 크면 그 크기 그대로(배율 없음)
		float             life = 0.0f;            // 한 바퀴 시간(s) = 파티클 수명. 0 = 층 표 수명
		DirectX::XMFLOAT2 pivot{ 0.5f, 0.5f };    // 맞은 점이 놓이는 칸 안 자리(u = 왼쪽 0 → 오른쪽 1, v = 위 0 → 아래 1). 기본 = 가운데
		DirectX::XMFLOAT4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };   // 텍스처 색에 곱할 색 · a = 불투명도(층 표의 색 대신 — 텍스처에 색이 있다)
		float             standOff = 0.0f;        // 보는 쪽(노멀 Z)으로 맞은 면에서 띄우는 거리(m) — 고정 판만. 깊이 페이드(0.3 m)에 지워지지 않게
		bool              stayAtHit = false;      // true = 맞은 자리에 머문다(층 표의 속도 · 중력 · 저항 · 땅 규칙 안 씀 — 움직임은 칸에 구워져 있다)
		bool              layerFade = true;       // false = 층 표의 옅어짐(처음 10 % 짙어짐 · 끝으로 옅어짐)을 안 곱한다 — 끝맺음이 텍스처 알파에 있을 때
		float             rayCoverage = kParticleRayCoverageRound;   // 레이(그림자 · AO · GI · 반사)가 받는 불투명도 몫 = 칸 평균 알파
	};

	class Flipbook
	{
	public:
		// 리소스 매니저가 읽은 RAM 이미지를 받아 GPU 텍스처를 만든다. normal 은 nullptr 이어도 된다.
		//  ★ 메인 스레드 · 프레임 밖(로드 묶음의 완료 콜백 같은 곳)에서 부른다 — GRenderer::CreateTexture 와 같은 조건.
		//  ★ 받은 RAM 이미지는 건드리지 않는다. 다 만든 뒤 부르는 쪽이 ResourceManager::Release 로 버린다.
		//  실패하면 false(LastError) — 만든 것이 있으면 놓고 빈 상태로 돌아간다.
		bool Create(GRenderer& renderer, const TextureData& color, const TextureData* normal, const FlipbookLayout& layout,
			const FlipbookSettings& settings = {});

		// GPU 텍스처를 놓는다(프레임 밖). 이 플립북을 가리키는 층은 먼저 ParticleSystem::ClearImages(층) 로 끊는다.
		void Release(GRenderer& renderer);

		bool Valid() const { return color != kInvalidTexture; }
		bool HasNormal() const { return normal != kInvalidTexture; }
		const FlipbookLayout& Layout() const { return layout; }

		// 재생 설정 — 언제든 바꿀 수 있다(이미 태어난 파티클: 크기 · 수명 · 색 · 머묾은 태어날 때 값, 기준점 · 옅어짐 · 레이 몫은 다음 Extract 부터).
		const FlipbookSettings& Settings() const { return settings; }
		void SetSettings(const FlipbookSettings& newSettings) { settings = newSettings; }

		// 수명 비율(0 → 1)에 맞는 프레임 위치 — 0 ~ frameCount − 1. 소수 = 다음 프레임과 섞을 몫(셰이더가 두 칸을 섞는다).
		//  한 번만 재생한다(파티클 수명 = 플립북 한 바퀴). 1 을 넘으면 마지막 프레임에 머문다.
		float FrameAt(float life01) const;

		// 렌더러에 넘길 한 줄(텍스처 번호 · 칸 배치)
		ParticleFlipbook Gpu() const;

		const std::wstring& LastError() const { return lastError; }

	private:
		TextureHandle    color = kInvalidTexture;
		TextureHandle    normal = kInvalidTexture;
		FlipbookLayout   layout;
		FlipbookSettings settings;   // 2026-11-25
		std::wstring     lastError;
	};

} // namespace swc
