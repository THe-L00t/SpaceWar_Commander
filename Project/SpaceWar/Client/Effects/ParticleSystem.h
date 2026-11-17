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

	class ParticleSystem
	{
	public:
		static constexpr size_t kSpritePool = 4096;   // 노션 10.3 — 16명 · 기관총 · 위쪽 값 순간 연사 3,718 까지 들어간다
		static constexpr size_t kDebrisPool = 4096;   // 노션 10.3 — 메시 파편(P4). 아직 쓰지 않는다
		static constexpr double kSpawnRange = 120.0;  // 눈에서 이보다 먼 명중은 만들지 않는다(m)

		// 맞은 곳의 종류 — 층(섬광 · 불꽃 · 먼지 · 연기 · 피)을 고른다(노션 10.2).
		//  지금 Shared::ShotHit 은 Body · Collider · Terrain 만 알려 준다 → Collider = 금속 · 건물(불꽃), Terrain = 지면(먼지), Body = 생물(피).
		enum class Surface : uint8_t { Metal, Ground, Body };
		static bool SurfaceOf(Shared::ShotHitKind kind, Surface& out);

		// ── 명중 하나의 효과를 만든다 ──
		//  position = 맞은 자리, normal = 튀는 쪽(표면 법선 — 없으면 쏜 방향의 반대), eye = 지금 카메라 눈(거리 검사).
		//  내 발(Engine::UpdateFire)이 부른다. ★ 남의 발 — 서버의 «사격 결과» 패킷이 생기면 그 처리에서 같은 함수를 부르면 된다
		//  (2026-11-17 사용자 «패킷은 아직 두고, 다른 사람의 사격효과도 그릴 준비만 해줘»). Body 면 피만, 나머지는 섬광 · 불꽃/먼지 · 연기.
		void EmitImpact(Surface surface, const Vec3d& position, const Vec3d& normal, const Vec3d& eye);

		// ── 피를 서버 확인 뒤에 띄우기 (2026-11-17 사용자 «구현만 하고 아직 적용은 하지 마» — Engine kBloodWaitsForServer = false) ──
		//  RememberBodyHit = 내 발이 NPC 몸에 맞은 자리를 기억해 둔다(NpcHit 패킷에는 자리가 없다 — npcId 만).
		//  ConfirmNpcHits = 서버가 «네 발이 이 NPC 에 맞았다» 고 알린 번호마다, 기억한 자리 중 그 NPC 의 가장 오래된 것에 피를 띄운다.
		//  kPendingSeconds 안에 확인이 안 온 자리는 버린다(서버가 빗나갔다고 본 발).
		static constexpr float kPendingSeconds = 1.0f;
		void RememberBodyHit(uint32_t npcId, const Vec3d& position, const Vec3d& normal, const Vec3d& eye);
		void ConfirmNpcHits(const std::vector<uint32_t>& npcIds);

		// 매 프레임 — 나이 · 중력(행성 중심 쪽) · 공기저항 · 지면(닿으면 튐 · 멈춤 · 사라짐).
		void Update(float dt, const Planet& planet);

		// 렌더 추출 — out 을 비우고 채운다: 알파 묶음(먼 것부터)을 앞에, 가산 묶음을 뒤에. alphaCount = 알파 묶음 수.
		void Extract(const Vec3d& eye, std::vector<ParticleSprite>& out, uint32_t& alphaCount) const;

		void Clear();   // 맵을 바꿀 때
		size_t LiveSprites() const { return liveCount; }
		size_t RecycledSprites() const { return recycled; }   // 풀이 차서 오래된 것을 다시 쓴 누적 수(확인용)

	private:
		enum class Layer : uint8_t { Flash, Spark, Dust, Smoke, BloodDrop, BloodMist, Count };
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

		struct Sprite
		{
			Vec3d    position;
			Vec3d    velocity;
			float    age = 0.0f, life = 0.0f;
			float    size = 0.0f, growth = 1.0f;
			float    rotation = 0.0f, spin = 0.0f;
			float    gravityScale = 1.0f, drag = 0.0f;
			DirectX::XMFLOAT4 color{ };
			uint32_t flags = 0;
			GroundRule ground = GroundRule::Ignore;
			bool     alive = false;
		};

		struct PendingHit
		{
			uint32_t npcId;
			Vec3d    position, normal, eye;
			float    age;
		};

		void EmitLayer(Layer layer, const Vec3d& position, const Vec3d& normal);
		Sprite& Allocate();
		float Random01();
		float Range(float a, float b) { return a + (b - a) * Random01(); }

		std::vector<Sprite>   sprites;          // kSpritePool 칸(Clear 가 만든다 — 약 0.5 MB 라 힙에 둔다)
		std::vector<uint32_t> freeSlots;        // 빈 칸 번호(처음엔 전부)
		size_t   liveCount = 0;
		size_t   recycled = 0;
		uint32_t rng = 0x9E3779B9u;             // 화면 효과용 난수(xorshift32) — 판정과 무관
		std::vector<PendingHit> pending;
		mutable std::vector<std::pair<float, uint32_t>> sortScratch;   // Extract — (거리², 칸) 알파 묶음 정렬용
		bool     initialized = false;
	};

} // namespace swc
