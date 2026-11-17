#include "ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include "Client/Planet.h"

using namespace DirectX;

namespace swc {

	namespace {
		constexpr float kTwoPi = 6.28318530718f;
		constexpr size_t kMaxPending = 256;        // 서버 확인을 기다리는 피 자리 상한(넘으면 오래된 것부터 버린다)
		constexpr double kSurfaceLift = 0.02;      // 맞은 면에서 띄워 만든다(m) — 면 속에서 태어나 바로 땅에 닿지 않게
	}

	// ── 층별 표 — 노션 「피격 파티클 정리」 10.2 (2026-11-17 사용자 «해당 조사를 따라 정리해주고») ──
	//  개수 · 수명 = 10.2 그대로. 크기 · 속도 · 원뿔 · 중력 · 저항 · 색은 자료에 없어 고른 시작값(속도만 Fab «불꽃 초속 10 m»).
	//  색은 선형 RGB — 해 정면의 흰 표면 휘도가 1 이다(GRenderer kSunIlluminanceTop). 1 을 넘는 섬광 · 불꽃은 블룸으로 번진다.
	const ParticleSystem::LayerSpec ParticleSystem::kLayers[size_t(Layer::Count)] =
	{
		//  개수      수명(s)         크기(m)      배율   속도(m/s)    원뿔   중력   저항   색(RGB · a)                         표시                              땅
		{  1,  1, 0.075f, 0.125f, 0.30f, 0.45f, 1.6f,  0.0f,  0.0f, 0.00f,  0.00f, 0.0f, { 6.0f, 4.5f, 2.5f, 1.0f },     kParticleAdditive,               GroundRule::Ignore },  // 섬광
		{ 10, 20, 0.30f,  0.60f,  0.03f, 0.06f, 0.5f,  6.0f, 12.0f, 1.10f,  1.00f, 1.5f, { 5.0f, 2.2f, 0.6f, 1.0f },     kParticleAdditive,               GroundRule::Bounce },  // 불꽃(금속 · 건물)
		{ 10, 20, 0.30f,  0.60f,  0.06f, 0.15f, 2.5f,  2.0f,  6.0f, 0.90f,  0.60f, 3.0f, { 0.32f, 0.29f, 0.25f, 0.55f }, kParticleLit,                    GroundRule::Stop   },  // 먼지(돌 · 흙 · 지면)
		{  2,  3, 2.00f,  3.00f,  0.30f, 0.50f, 3.0f,  0.3f,  1.0f, 0.60f, -0.03f, 1.2f, { 0.45f, 0.44f, 0.42f, 0.28f }, kParticleLit,                    GroundRule::Ignore },  // 연기
		{ 10, 20, 0.80f,  1.20f,  0.01f, 0.04f, 1.0f,  2.0f,  6.0f, 0.70f,  1.00f, 0.8f, { 0.30f, 0.012f, 0.012f, 0.9f }, kParticleLit,                   GroundRule::Die    },  // 피 방울(땅에 닿으면 사라짐)
		{  1,  2, 0.40f,  0.60f,  0.20f, 0.40f, 2.0f,  0.5f,  1.5f, 0.50f,  0.00f, 3.0f, { 0.35f, 0.02f, 0.02f, 0.35f }, kParticleLit,                    GroundRule::Ignore },  // 피 안개
	};

	bool ParticleSystem::SurfaceOf(Shared::ShotHitKind kind, Surface& out)
	{
		switch (kind)
		{
		case Shared::ShotHitKind::Body:     out = Surface::Body;   return true;
		case Shared::ShotHitKind::Collider: out = Surface::Metal;  return true;
		case Shared::ShotHitKind::Terrain:  out = Surface::Ground; return true;
		default: return false;   // None — 아무것도 안 맞았다
		}
	}

	void ParticleSystem::Clear()
	{
		sprites.assign(kSpritePool, Sprite{});
		freeSlots.clear();
		freeSlots.reserve(kSpritePool);
		for (size_t i = kSpritePool; i-- > 0; )
			freeSlots.push_back(uint32_t(i));   // 뒤에서 꺼내므로 0 번부터 쓴다
		liveCount = 0;
		pending.clear();
		initialized = true;
	}

	float ParticleSystem::Random01()
	{
		// xorshift32 — 화면 효과용(판정과 무관). 위 24비트를 [0, 1) 로.
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return float(rng >> 8) * (1.0f / 16777216.0f);
	}

	ParticleSystem::Sprite& ParticleSystem::Allocate()
	{
		if (!initialized) Clear();
		if (!freeSlots.empty())
		{
			Sprite& s = sprites[freeSlots.back()];
			freeSlots.pop_back();
			++liveCount;
			return s;
		}
		// 풀이 찼다 — 가장 오래된 것(나이가 가장 큰 것)을 다시 쓴다(노션 10.3).
		size_t oldest = 0;
		for (size_t i = 1; i < sprites.size(); ++i)
			if (sprites[i].age > sprites[oldest].age) oldest = i;
		++recycled;
		return sprites[oldest];
	}

	void ParticleSystem::EmitLayer(Layer layer, const Vec3d& position, const Vec3d& normal)
	{
		const LayerSpec& spec = kLayers[size_t(layer)];
		// 원뿔 안 방향을 만들 축 — normal 에 수직인 두 단위 벡터
		const Vec3d axisA = ProjectOntoPlane(std::fabs(normal.y) < 0.9 ? Vec3d{ 0.0, 1.0, 0.0 } : Vec3d{ 1.0, 0.0, 0.0 }, normal);
		const Vec3d axisB = Cross(normal, axisA);

		const uint32_t span = uint32_t(spec.countMax) - uint32_t(spec.countMin) + 1u;
		const uint32_t count = uint32_t(spec.countMin) + (std::min)(uint32_t(Random01() * float(span)), span - 1u);
		for (uint32_t n = 0; n < count; ++n)
		{
			Sprite& s = Allocate();
			const float theta = spec.cone * std::sqrt(Random01());   // 원뿔 안 고르게
			const float phi = kTwoPi * Random01();
			const Vec3d side = axisA * std::cos(phi) + axisB * std::sin(phi);
			const Vec3d dir = normal * std::cos(theta) + side * std::sin(theta);
			s.position = position + normal * kSurfaceLift;
			s.velocity = dir * Range(spec.speedMin, spec.speedMax);
			s.age = 0.0f;
			s.life = Range(spec.lifeMin, spec.lifeMax);
			s.size = Range(spec.sizeMin, spec.sizeMax);
			s.growth = spec.growth;
			s.rotation = kTwoPi * Random01();
			s.spin = Range(-2.0f, 2.0f);
			s.gravityScale = spec.gravityScale;
			s.drag = spec.drag;
			s.color = spec.color;
			s.flags = spec.flags;
			s.ground = spec.ground;
			s.alive = true;
		}
	}

	void ParticleSystem::EmitImpact(Surface surface, const Vec3d& position, const Vec3d& normal, const Vec3d& eye)
	{
		if (Length(position - eye) > kSpawnRange) return;   // 120 m 밖 명중은 만들지 않는다
		const Vec3d n = Normalize(normal);
		if (surface == Surface::Body)
		{
			EmitLayer(Layer::BloodDrop, position, n);
			EmitLayer(Layer::BloodMist, position, n);
			return;
		}
		EmitLayer(Layer::Flash, position, n);
		EmitLayer(surface == Surface::Metal ? Layer::Spark : Layer::Dust, position, n);
		EmitLayer(Layer::Smoke, position, n);
	}

	void ParticleSystem::RememberBodyHit(uint32_t npcId, const Vec3d& position, const Vec3d& normal, const Vec3d& eye)
	{
		if (pending.size() >= kMaxPending)
			pending.erase(pending.begin());
		pending.push_back({ npcId, position, normal, eye, 0.0f });
	}

	void ParticleSystem::ConfirmNpcHits(const std::vector<uint32_t>& npcIds)
	{
		for (const uint32_t id : npcIds)
		{
			// 그 NPC 를 맞힌 자리 중 가장 오래된 것 — 서버는 받은 순서대로 판정해 보낸다.
			const auto it = std::find_if(pending.begin(), pending.end(),
				[id](const PendingHit& hit) { return hit.npcId == id; });
			if (it == pending.end()) continue;   // 기억한 자리가 없다(이미 버림 · 다른 맵)
			EmitImpact(Surface::Body, it->position, it->normal, it->eye);
			pending.erase(it);
		}
	}

	void ParticleSystem::Update(float dt, const Planet& planet)
	{
		if (!initialized) Clear();
		if (dt <= 0.0f) return;

		// 서버 확인을 기다리던 피 자리 — kPendingSeconds 가 지나면 버린다(서버가 빗나갔다고 본 발).
		for (PendingHit& hit : pending) hit.age += dt;
		pending.erase(std::remove_if(pending.begin(), pending.end(),
			[](const PendingHit& hit) { return hit.age > kPendingSeconds; }), pending.end());

		for (uint32_t i = 0; i < uint32_t(sprites.size()); ++i)
		{
			Sprite& s = sprites[i];
			if (!s.alive) continue;
			s.age += dt;
			if (s.age >= s.life)
			{
				s.alive = false;
				freeSlots.push_back(i);
				--liveCount;
				continue;
			}

			// 중력은 행성 중심 쪽(Planet::Up 의 반대), 공기저항은 지수 감쇠.
			const Vec3d up = planet.Up(s.position);
			s.velocity = s.velocity - up * (planet.gravity * double(s.gravityScale) * double(dt));
			s.velocity = s.velocity * double(std::exp(-s.drag * dt));
			s.position = s.position + s.velocity * double(dt);
			s.rotation += s.spin * dt;

			if (s.ground == GroundRule::Ignore) continue;
			// 지면 — 지금 맵의 높이(Planet::SurfaceHeight). 구면은 방향으로, 평면은 위치로 묻는다.
			const Vec3d probe = Shared::kUsePlanarMap ? s.position : Normalize(s.position - planet.center);
			const double groundHeight = planet.SurfaceHeight(probe);
			if (planet.Altitude(s.position) > groundHeight) continue;

			switch (s.ground)
			{
			case GroundRule::Die:
				s.alive = false;
				freeSlots.push_back(i);
				--liveCount;
				break;
			case GroundRule::Stop:   // 땅에 붙어 남은 수명 동안 옅어진다
				s.position = planet.PositionAt(probe, groundHeight);
				s.velocity = Vec3d{ 0.0, 0.0, 0.0 };
				s.gravityScale = 0.0f;
				s.ground = GroundRule::Ignore;
				break;
			case GroundRule::Bounce:   // 한 번 튀고(위쪽 속도 30 % · 옆 60 %), 다음에 닿으면 사라진다
			{
				const double vn = Dot(s.velocity, up);
				const Vec3d tangent = s.velocity - up * vn;
				s.position = planet.PositionAt(probe, groundHeight);
				s.velocity = tangent * 0.6 + up * (vn < 0.0 ? -vn * 0.3 : vn);
				s.ground = GroundRule::Die;
				break;
			}
			default:
				break;
			}
		}
	}

	void ParticleSystem::Extract(const Vec3d& eye, std::vector<ParticleSprite>& out, uint32_t& alphaCount) const
	{
		out.clear();
		alphaCount = 0;
		if (liveCount == 0) return;
		out.reserve(liveCount);

		auto toSprite = [](const Sprite& s)
		{
			const float t = (std::min)(s.age / s.life, 1.0f);   // 0 → 1
			// 연기 · 먼지는 처음 10 % 동안 짙어지고 끝으로 갈수록 옅어진다. 나머지는 처음부터 옅어진다.
			const float fadeIn = (s.flags & kParticleLit) ? (std::min)(t * 10.0f, 1.0f) : 1.0f;
			const float fade = fadeIn * (1.0f - t);
			ParticleSprite p;
			p.position = XMFLOAT3(float(s.position.x), float(s.position.y), float(s.position.z));
			p.size = s.size * (1.0f + (s.growth - 1.0f) * t);
			p.color = XMFLOAT4(s.color.x, s.color.y, s.color.z, s.color.w * fade);
			p.rotation = s.rotation;
			p.flags = s.flags;
			return p;
		};

		// 알파 묶음은 먼 것부터(뒤에서 앞으로) — 겹칠 때 순서가 맞아야 한다. 가산 묶음은 순서와 무관하다.
		sortScratch.clear();
		for (uint32_t i = 0; i < uint32_t(sprites.size()); ++i)
		{
			const Sprite& s = sprites[i];
			if (!s.alive || (s.flags & kParticleAdditive)) continue;
			const Vec3d d = s.position - eye;
			sortScratch.emplace_back(float(Dot(d, d)), i);
		}
		std::sort(sortScratch.begin(), sortScratch.end(),
			[](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) { return a.first > b.first; });
		for (const auto& entry : sortScratch)
			out.push_back(toSprite(sprites[entry.second]));
		alphaCount = uint32_t(out.size());

		for (const Sprite& s : sprites)
			if (s.alive && (s.flags & kParticleAdditive))
				out.push_back(toSprite(s));
	}

} // namespace swc
