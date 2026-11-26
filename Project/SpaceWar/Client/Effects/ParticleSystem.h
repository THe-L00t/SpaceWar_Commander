#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <DirectXMath.h>
#include "Client/InstanceData.h"
#include "Client/Vec3d.h"
#include "Shared/GameLogic/ShotTrace.h"

// ============================================================
//  Client/Effects/ParticleSystem.h — 피격 파티클 (2026-11-17)
//
//  사용자(2026-11-17): «파티클 전용 파이프라인», «각 파티클 갯수나 수명은 해당 조사를 따라 정리해주고, 풀 크기도 그걸로 정해줘»,
//  «CPU로하는게 기본아니야?», «다른 사람의 사격효과도 그릴 준비만 해줘», 피 위치 «구현만 하고 아직 적용은 하지 마».
//  층별 개수 · 수명 · 풀 크기 = 노션 「피격 파티클 정리」 10절(kLayers 표 하나에 모았다).
//
//  ★ 클라 전용 — 판정과 무관하다(명세 16절 Game Timer «클라이언트 전용 요소(Particle · Visual Effect)»). 서버는 모른다.
//  ★ CPU 에서 움직인다 — 맞은 자리(Shared::TraceShot) · 지면 높이(Planet::SurfaceHeight)가 CPU 에 있고, 풀이 4,096 개라 충분하다.
//    결과는 프레임마다 FramePacket::particles 로 넘긴다(명세 7절 Render World Particle[]). 렌더러는 그 배열만 본다.
//  ★ 풀 — 사각형 kSpritePool 개 고정. 꽉 차면 가장 오래된 것(나이가 가장 큰 것)을 다시 쓴다.
//  ★ 120 m(kSpawnRange — 오브젝트 단계 설계의 R1 후보, 사용자 «TLAS 중간 범위까지는 다 보이게») 밖에서 맞은 것은 만들지 않는다.
//  ★ 메시 파편(바닥에 4 ~ 5 s 남는 조각)은 4단계(P4) — 풀 크기 kDebrisPool 만 정해 두었다.
// ============================================================

namespace swc {

	struct Planet;
	class Flipbook;
	class ParticleTexture;

	class ParticleSystem
	{
	public:
		// 층 — 노션 「피격 파티클 정리」 10.2 의 한 줄(kLayers 의 줄 순서).
		//  2026-11-23 — 층마다 플립북을 붙이려고(지금 AddFlipbook · AddTexture) 밖에 보이게 옮겼다.
		enum class Layer : uint8_t { Flash, Spark, Dust, Smoke, BloodDrop, BloodMist, Count };

		static constexpr size_t kSpritePool = 4096;   // 노션 10.3 — 16명 · 기관총 · 위쪽 값 순간 연사 3,718 까지 들어간다
		static constexpr size_t kDebrisPool = 4096;   // 노션 10.3 — 메시 파편(P4). 아직 쓰지 않는다
		static constexpr double kSpawnRange = 120.0;  // 눈에서 이보다 먼 명중은 만들지 않는다(m)

		// 맞은 곳의 종류 — 층(섬광 · 불꽃 · 먼지 · 연기 · 피)을 고른다(노션 10.2).
		//  지금 Shared::ShotHit 은 Body · Collider · Terrain 만 알려 준다 → Collider = 금속 · 건물(불꽃), Terrain = 지면(먼지), Body = 생물(피).
		enum class Surface : uint8_t { Metal, Ground, Body };
		static bool SurfaceOf(Shared::ShotHitKind kind, Surface& out);

		// ── 명중 하나의 효과를 만든다 ──
		//  position = 맞은 자리, normal = 튀는 쪽(표면 법선 — 없으면 쏜 방향의 반대), eye = 지금 카메라 눈(거리 검사).
		//  up = 맞은 자리의 행성 위(Planet::Up) — 2026-11-25 고정 판(FlipbookFacing Front · Side)의 «위» 와 «수평» 을 정한다.
		//  내 발(Engine::UpdateFire)이 부른다. ★ 남의 발 — 서버의 «사격 결과» 패킷이 생기면 그 처리에서 같은 함수를 부르면 된다
		//  (2026-11-17 사용자 «패킷은 아직 두고, 다른 사람의 사격효과도 그릴 준비만 해줘»). Body 면 피만, Ground 는 먼지 · 연기, Metal(충돌체)은 연기만
	//  (2026-11-23 사용자 «섬광 불꽃은 빼» — 그 전엔 섬광 · 불꽃/먼지 · 연기).
		void EmitImpact(Surface surface, const Vec3d& position, const Vec3d& normal, const Vec3d& up, const Vec3d& eye);

		// ── 피를 서버 확인 뒤에 띄우기 (2026-11-17 사용자 «구현만 하고 아직 적용은 하지 마» — Engine kBloodWaitsForServer = false) ──
		//  RememberBodyHit = 내 발이 NPC 몸에 맞은 자리를 기억해 둔다(NpcHit 패킷에는 자리가 없다 — npcId 만).
		//  ConfirmNpcHits = 서버가 «네 발이 이 NPC 에 맞았다» 고 알린 번호마다, 기억한 자리 중 그 NPC 의 가장 오래된 것에 피를 띄운다.
		//  kPendingSeconds 안에 확인이 안 온 자리는 버린다(서버가 빗나갔다고 본 발).
		static constexpr float kPendingSeconds = 1.0f;
		void RememberBodyHit(uint32_t npcId, const Vec3d& position, const Vec3d& normal, const Vec3d& up, const Vec3d& eye);
		void ConfirmNpcHits(const std::vector<uint32_t>& npcIds);

		// 매 프레임 — 나이 · 중력(행성 중심 쪽) · 공기저항 · 지면(닿으면 튐 · 멈춤 · 사라짐).
		void Update(float dt, const Planet& planet);

		// ── 층에 그림 붙이기 (2026-11-23 플립북 · 11-24 낱장 텍스처 · 11-24 여러 장) ──
		//  2026-11-23 사용자 «리소스 매니저에서 읽어 넘겨주면 플립북으로 다룰 수 있도록», 11-24 «일반 텍스처용 클래스 하나 만들어줘. 회전이 가능하게»,
		//  11-24 «먼지 전부, 연기 전부» → 층 하나에 그림을 여러 장 붙인다. 파티클마다 태어날 때 그 층의 그림 중 하나를 고르게(같은 확률) 고른다.
		//  그림이 없는 층은 둥근 모양. 플립북과 낱장 텍스처를 한 층에 섞어 붙여도 된다.
		//  ★ 텍스처를 읽지 않는다 — ResourceManager 가 읽고 Flipbook · ParticleTexture 가 GPU 로 올린 것을 가리키기만 한다(주인은 부르는 쪽).
		//  tint = 텍스처 색에 곱할 색 · a = 불투명도(층 표의 색 대신 — 텍스처에 색이 들어 있다).
		//    2026-11-25 — 플립북은 tint 를 Flipbook::Settings() 에서 읽는다(크기 · 수명 · 기준점 · 보는 쪽도 거기 — Flipbook.h FlipbookSettings).
		//  플립북을 고른 파티클은 돌리지 않는다 — 플립북 안에 구운 «아래»(떨어지는 방향)가 화면 아래로 남게.
		//  낱장 텍스처를 고른 파티클은 돈다 — 태어날 때 무작위 각도 + 수명 동안 스핀(둥근 모양 때와 같은 값, EmitLayer).
		//  group(2026-11-25 사용자 «정면 측면 둘다 출력») — 0 이 아니면 같은 층 · 같은 번호의 플립북을 한 묶음으로 본다:
		//    고를 때 묶음이 한 표(같은 확률)이고, 골라지면 묶음의 플립북이 같은 자리에 한꺼번에 나온다(피 = 판마다 정면 + 측면).
		void AddFlipbook(Layer layer, const Flipbook* flipbook, uint16_t group = 0);
		void AddTexture(Layer layer, const ParticleTexture* texture, const DirectX::XMFLOAT4& tint = { 1.0f, 1.0f, 1.0f, 1.0f });
		// 그 층의 그림을 모두 뗀다(둥근 모양으로 돌아감). 이미 그림을 고른 파티클도 다음 Extract 부터 둥근 모양이다.
		//  그림(Flipbook · ParticleTexture)을 놓기 전에 먼저 부른다.
		void ClearImages(Layer layer);
		size_t ImageCount(Layer layer) const { return layer < Layer::Count ? layerImages[size_t(layer)].size() : 0; }

		// 렌더 추출 — out 을 비우고 채운다: 알파 묶음(먼 것부터)을 앞에, 가산 묶음을 뒤에. alphaCount = 알파 묶음 수.
		//  flipbooks = 이번 프레임에 쓰는 텍스처 표(ParticleSprite::flipbook − 1 이 번호, kMaxParticleFlipbooks 까지 — 넘는 몫은 둥근 모양).
		//  플립북과 낱장 텍스처(1 × 1 칸 한 줄)가 같은 표를 나눠 쓴다. 모든 층에 붙은 그림이 한 줄씩(같은 그림은 한 줄만).
		void Extract(const Vec3d& eye, std::vector<ParticleSprite>& out, uint32_t& alphaCount,
			std::vector<ParticleFlipbook>& flipbooks) const;

		void Clear();   // 맵을 바꿀 때
		size_t LiveSprites() const { return liveCount; }
		size_t RecycledSprites() const { return recycled; }   // 풀이 차서 오래된 것을 다시 쓴 누적 수(확인용)

	private:
		enum class GroundRule : uint8_t { Ignore, Die, Stop, Bounce };   // 땅에 닿으면

		struct LayerSpec   // 노션 10.2 의 한 줄 — 범위는 파티클마다 무작위로 고른다(UE4 위키 Uniform Min · Max)
		{
			uint8_t    countMin, countMax;
			float      lifeMin, lifeMax;      // s
			float      sizeMin, sizeMax;      // 처음 크기(m)
			float      growth;                // 수명 끝의 크기 배율
			float      speedMin, speedMax;    // 처음 속도(m/s) — 튀는 쪽(normal) 둘레 원뿔 안
			float      cone;                  // 원뿔 반각(라디안)
			float      gravityScale;          // 1 = 행성 중력, 음수 = 떠오름
			float      drag;                  // 초당 속도 감쇠(1/s)
			DirectX::XMFLOAT4 color;          // 선형 RGB · a
			uint32_t   flags;                 // ParticleFlags
			GroundRule ground;
		};
		static const LayerSpec kLayers[size_t(Layer::Count)];

		static constexpr uint16_t kNoImage = 0xFFFF;   // Sprite::image — 그림 없음(둥근 모양)

		struct Sprite
		{
			Vec3d    position;
			Vec3d    velocity;
			float    age = 0.0f, life = 0.0f;
			float    size = 0.0f, growth = 1.0f;
			float    rotation = 0.0f, spin = 0.0f;
			float    gravityScale = 1.0f, drag = 0.0f;
			DirectX::XMFLOAT4 color{ };
			DirectX::XMFLOAT3 axisX{ }, axisY{ }, axisZ{ };   // 2026-11-25 — 고정 판 축(flags 에 kParticleFixedAxes 일 때)
			uint32_t flags = 0;
			GroundRule ground = GroundRule::Ignore;
			Layer    layer = Layer::Flash;   // 2026-11-23 — 붙은 플립북 · 낱장 텍스처를 찾는 데 쓴다
			uint16_t image = kNoImage;       // 2026-11-24 — 태어날 때 고른 그림(layerImages[layer] 의 번호, kNoImage = 둥근 모양)
			// 2026-11-26 [0b] 빛 고정 열쇠 — 이 칸에 태어난 차례(Allocate 가 하나씩 올린다, kParticleKeyMask 에서 돌아감).
			//  Extract 가 칸 번호와 함께 flags 위 비트에 싣는다(InstanceData.h kParticleLightKeyShift). Clear 도 지우지 않는다 —
			//  GPU 의 칸 기록(ParticleLight.hlsl)이 맵을 바꿔도 남아 있어, 0 부터 다시 세면 앞 파티클의 값을 이어 쓸 수 있다.
			uint16_t lightKey = 0;
			bool     alive = false;
		};
		static_assert(kSpritePool <= size_t(kParticleKeyMask) + 1, "풀 칸 번호는 flags 의 12비트(kParticleSlotShift)에 들어가야 한다");

		// 그림 한 장(가리키기만) · 텍스처에 곱할 색. flipbook · texture 중 하나만 채운다(2026-11-24 texture 추가).
		struct LayerImage
		{
			const Flipbook*        flipbook = nullptr;
			const ParticleTexture* texture = nullptr;
			DirectX::XMFLOAT4      tint{ 1.0f, 1.0f, 1.0f, 1.0f };   // 낱장 텍스처만(플립북은 Settings().tint)
			uint16_t               group = 0;    // 2026-11-25 — AddFlipbook group(0 = 혼자)
			uint16_t               choice = 0;   // 2026-11-25 — 고르는 표 번호(같은 묶음 = 같은 번호, 0 ~ layerChoices − 1)
		};
		// 층마다 붙은 그림 목록(2026-11-24 «먼지 전부, 연기 전부» — 한 장 → 여러 장). 로드 때만 늘어난다.
		std::array<std::vector<LayerImage>, size_t(Layer::Count)> layerImages{};
		std::array<uint16_t, size_t(Layer::Count)> layerChoices{};   // 2026-11-25 — 층마다 고를 표 수(묶음 하나 = 한 표)
		mutable std::vector<uint32_t>    imageSlotScratch;   // Extract — 층별 그림의 표 번호 + 1(0 = 없음), layerImages 를 이어 붙인 순서
		mutable std::vector<const void*> slotKeyScratch;     // Extract — 표 줄마다 그 그림의 주소(같은 그림을 한 줄로)

		struct PendingHit
		{
			uint32_t npcId;
			Vec3d    position, normal, up, eye;
			float    age;
		};

		void EmitLayer(Layer layer, const Vec3d& position, const Vec3d& normal, const Vec3d& up);
		Sprite& Allocate();
		float Random01();
		float Range(float a, float b) { return a + (b - a) * Random01(); }

		std::vector<Sprite>   sprites;          // kSpritePool 칸(Clear 가 만든다 — 약 0.5 MB 라 힙에 둔다)
		std::vector<uint32_t> freeSlots;        // 빈 칸 번호(처음엔 전부)
		size_t   liveCount = 0;
		size_t   recycled = 0;
		uint32_t rng = 0x9E3779B9u;             // 화면 효과용 난수(xorshift32) — 판정과 무관
		std::vector<PendingHit> pending;
		mutable std::vector<std::pair<float, uint32_t>> sortScratch;   // Extract — (거리², spriteScratch 번호) 알파 묶음 정렬용
		mutable std::vector<ParticleSprite> spriteScratch;             // Extract — 알파 묶음을 먼저 펴 두는 곳(2026-11-25 정렬 기준 = 사각형 가운데)
		bool     initialized = false;
	};

} // namespace swc
