#pragma once
#include <cstdint>
#include <string>
#include "Client/Handles.h"
#include "Client/InstanceData.h"

// ============================================================
//  Client/Effects/ParticleTexture.h — 파티클 낱장 텍스처 (2026-11-24)
//
//  사용자(2026-11-24): «일반 텍스처용 클래스 하나 만들어줘. 회전이 가능하게».
//
//  Flipbook(Client/Effects/Flipbook.h)과 다른 점:
//    · 칸 · 프레임이 없다 — 이미지 한 장이 사각형 하나를 덮는다(연기 · 먼지 덩이 같은 것).
//    · 이 텍스처를 붙인 층의 사각형은 «돈다» — 태어날 때 무작위 각도 · 수명 동안 스핀(ParticleSystem::EmitLayer, 둥근 모양과 같은 값).
//      같은 그림이 겹쳐 쌓여도 반복이 덜 보인다. 플립북은 안에 구운 «아래»(피가 떨어지는 방향) 때문에 돌리지 않는다.
//
//  역할 나눔은 Flipbook 과 같다(명세 원칙 5 · §4 — Resource Manager 는 RAM 까지, GPU 자원은 Renderer):
//    ResourceManager  — 파일을 읽어 RAM 이미지(TextureData)를 준다. 텍스처 파일을 읽는 곳은 여기뿐이다.
//    ParticleTexture  — 받은 RAM 이미지(색 · 노멀)를 Renderer 로 GPU 텍스처로 올린다.
//    ParticleSystem   — 층마다 ParticleTexture 를 가리킨다(AddTexture — 2026-11-24 층마다 여러 장, 파티클마다 하나를 고른다). 파일은 모른다.
//    Renderer         — 패킷의 표 한 줄(ParticleFlipbook — 1 × 1 칸 · 1 프레임)을 [7b] 에서 샘플한다.
//                       렌더러 · 셰이더는 플립북과 같은 길을 쓴다(1 × 1 이면 그 한 장을 그대로 읽는다).
//
//  텍스처 규칙(Flipbook 과 같다):
//    · 색 = 바탕색(빛을 굽지 않음, sRGB) + 알파. 노멀 = 사각형 기준(X 오른쪽 · Y 위 · Z 카메라 쪽, 선형) + 알파 — 없어도 된다.
//    · 사각형이 돌면 노멀도 같이 돈다(Particles.hlsl PSParticle 이 회전만큼 돌린 축으로 월드에 옮긴다).
//    · 게임이 해 · 하늘빛을 비춘다(2026-11-23 사용자 «게임 빛을 주는 방향으로 가야지»).
// ============================================================

namespace swc {

	class GRenderer;
	struct TextureData;

	class ParticleTexture
	{
	public:
		// 리소스 매니저가 읽은 RAM 이미지를 받아 GPU 텍스처를 만든다. normal 은 nullptr 이어도 된다.
		//  ★ 메인 스레드 · 프레임 밖(로드 묶음의 완료 콜백 같은 곳)에서 부른다 — GRenderer::CreateTexture 와 같은 조건.
		//  ★ 받은 RAM 이미지는 건드리지 않는다. 다 만든 뒤 부르는 쪽이 ResourceManager::Release 로 버린다.
		//  실패하면 false(LastError) — 만든 것이 있으면 놓고 빈 상태로 돌아간다.
		bool Create(GRenderer& renderer, const TextureData& color, const TextureData* normal);

		// GPU 텍스처를 놓는다(프레임 밖). 이 텍스처를 가리키는 층은 먼저 ParticleSystem::ClearImages(층) 로 끊는다.
		void Release(GRenderer& renderer);

		bool Valid() const { return color != kInvalidTexture; }
		bool HasNormal() const { return normal != kInvalidTexture; }

		// 렌더러에 넘길 표 한 줄 — 1 × 1 칸 · 1 프레임(플립북 표와 같은 칸을 쓴다)
		ParticleFlipbook Gpu() const;

		const std::wstring& LastError() const { return lastError; }

	private:
		TextureHandle color = kInvalidTexture;
		TextureHandle normal = kInvalidTexture;
		std::wstring  lastError;
	};

} // namespace swc
