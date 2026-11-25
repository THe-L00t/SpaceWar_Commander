#include "ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include "Client/Planet.h"
#include "Client/Effects/Flipbook.h"
#include "Client/Effects/ParticleTexture.h"

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
		{  1,  1, 0.075f, 0.125f, 0.30f, 0.45f, 1.6f,  0.0f,  0.0f, 0.00f,  0.00f, 0.0f, { 6.0f, 4.5f, 2.5f, 1.0f },     kParticleAdditive,               GroundRule::Ignore },  // 섬광 — 2026-11-23 안 씀(EmitImpact)
		{ 10, 20, 0.30f,  0.60f,  0.03f, 0.06f, 0.5f,  6.0f, 12.0f, 1.10f,  1.00f, 1.5f, { 5.0f, 2.2f, 0.6f, 1.0f },     kParticleAdditive,               GroundRule::Bounce },  // 불꽃(금속 · 건물) — 2026-11-23 안 씀(EmitImpact)
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

	void ParticleSystem::EmitLayer(Layer layer, const Vec3d& position, const Vec3d& normal, const Vec3d& up)
	{
		const LayerSpec& spec = kLayers[size_t(layer)];
		// 원뿔 안 방향을 만들 축 — normal 에 수직인 두 단위 벡터
		const Vec3d axisA = ProjectOntoPlane(std::fabs(normal.y) < 0.9 ? Vec3d{ 0.0, 1.0, 0.0 } : Vec3d{ 1.0, 0.0, 0.0 }, normal);
		const Vec3d axisB = Cross(normal, axisA);

		// ── 2026-11-25 고정 판(FlipbookFacing Front · Side)의 축 (사용자 «맞은 지점 기준으로 정면 측면 둘다 출력») ──
		//  뿜는 쪽 d = normal 을 수평으로(행성 위에 수직), 위 = 그 자리의 행성 위.
		//  게임 시점 렌더(Resource_Work/HitParticles/_scripts/blood_fluid_v2_ingame.py 162 ~ 165 줄)는 «−사격 × 0.6 + 면 노멀 × 0.4» 를 수평으로 —
		//  몸에 맞으면 normal = 쏜 방향의 반대(Engine::UpdateFire)라 같은 방향이 된다.
		//  왼손 좌표(Camera.cpp XMMatrixLookAtLH — 오른쪽 = 위 × 앞 = 눈 쪽 × 위):
		//    정면 = (X d × 위, Y 위, Z d) — 쏜 사람(앞 = −d)에게 칸이 뒤집히지 않고 보인다.
		//    측면 = (X d, Y 위, Z 위 × d) — 칸 오른쪽(뿜는 쪽)이 d.
		const Vec3d upN = Normalize(up);
		const Vec3d spray = ProjectOntoPlane(normal, upN);   // 정규화됨 · normal 이 위와 나란하면 아무 수평 방향
		auto toFloat3 = [](const Vec3d& v) { return XMFLOAT3(float(v.x), float(v.y), float(v.z)); };

		const std::vector<LayerImage>& images = layerImages[size_t(layer)];
		const uint32_t choices = layerChoices[size_t(layer)];

		const uint32_t span = uint32_t(spec.countMax) - uint32_t(spec.countMin) + 1u;
		const uint32_t count = uint32_t(spec.countMin) + (std::min)(uint32_t(Random01() * float(span)), span - 1u);
		for (uint32_t n = 0; n < count; ++n)
		{
			const float theta = spec.cone * std::sqrt(Random01());   // 원뿔 안 고르게
			const float phi = kTwoPi * Random01();
			const Vec3d side = axisA * std::cos(phi) + axisB * std::sin(phi);
			const Vec3d dir = normal * std::cos(theta) + side * std::sin(theta);
			const float speed = Range(spec.speedMin, spec.speedMax);
			const float life = Range(spec.lifeMin, spec.lifeMax);
			const float size = Range(spec.sizeMin, spec.sizeMax);
			const float rotation = kTwoPi * Random01();
			const float spin = Range(-2.0f, 2.0f);

			// 2026-11-24 — 그 층에 붙은 그림 중 하나를 같은 확률로 고른다(사용자 «먼지 전부, 연기 전부»). 없으면 둥근 모양.
			//  2026-11-25 — 묶음(AddFlipbook group)은 한 표. 골라지면 묶음의 그림이 모두 같은 자리 · 같은 값으로 나온다(피 = 판마다 정면 + 측면).
			//  플립북: 색 = Settings().tint, 돌리지 않는다(구운 «아래» 를 화면 아래로) — 2026-11-23. 크기 · 수명 · 머묾 · 보는 쪽 = Settings() — 2026-11-25.
			//  낱장 텍스처: 색은 tint, 회전 · 스핀은 위 무작위 값 그대로(사용자 «회전이 가능하게») — 2026-11-24.
			const uint32_t pick = choices > 0 ? (std::min)(uint32_t(Random01() * float(choices)), choices - 1u) : 0u;
			auto spawn = [&](size_t imageIndex)   // imageIndex = images 번호, images.size() = 그림 없음(둥근 모양)
			{
				Sprite& s = Allocate();
				s.position = position + normal * kSurfaceLift;
				s.velocity = dir * speed;
				s.age = 0.0f;
				s.life = life;
				s.size = size;
				s.growth = spec.growth;
				s.rotation = rotation;
				s.spin = spin;
				s.gravityScale = spec.gravityScale;
				s.drag = spec.drag;
				s.color = spec.color;
				s.flags = spec.flags;
				s.ground = spec.ground;
				s.layer = layer;
				s.image = kNoImage;
				if (imageIndex < images.size())
				{
					const LayerImage& image = images[imageIndex];
					s.image = uint16_t(imageIndex);
					if (image.flipbook && image.flipbook->Valid())
					{
						const FlipbookSettings& fs = image.flipbook->Settings();
						s.color = fs.tint;
						s.rotation = 0.0f;
						s.spin = 0.0f;
						if (fs.size > 0.0f) { s.size = fs.size; s.growth = 1.0f; }   // 칸이 실제 크기 — 커지지 않는다
						if (fs.life > 0.0f) s.life = fs.life;
						if (fs.stayAtHit)   // 움직임은 칸에 구워져 있다 — 맞은 자리(기준점 = pivot)에 머문다
						{
							s.velocity = Vec3d{ 0.0, 0.0, 0.0 };
							s.gravityScale = 0.0f;
							s.drag = 0.0f;
							s.ground = GroundRule::Ignore;
						}
						if (fs.facing != FlipbookFacing::Camera)
						{
							const bool front = fs.facing == FlipbookFacing::Front;
							const Vec3d x = front ? Cross(spray, upN) : spray;
							const Vec3d z = front ? spray : Cross(upN, spray);
							s.axisX = toFloat3(x);
							s.axisY = toFloat3(upN);
							s.axisZ = toFloat3(z);
							s.flags |= kParticleFixedAxes;
							s.position = s.position + z * double(fs.standOff);   // 깊이 페이드에 지워지지 않게 보는 쪽으로 띄운다
						}
					}
					else if (image.texture && image.texture->Valid())
						s.color = image.tint;
					else
						s.image = kNoImage;   // 놓인 그림 — 둥근 모양 · 층 색 그대로
				}
				s.alive = true;
			};
			if (choices == 0)
				spawn(images.size());
			else
				for (size_t k = 0; k < images.size(); ++k)
					if (images[k].choice == pick) spawn(k);
		}
	}

	void ParticleSystem::EmitImpact(Surface surface, const Vec3d& position, const Vec3d& normal, const Vec3d& up, const Vec3d& eye)
	{
		if (Length(position - eye) > kSpawnRange) return;   // 120 m 밖 명중은 만들지 않는다
		const Vec3d n = Normalize(normal);
		if (surface == Surface::Body)
		{
			EmitLayer(Layer::BloodDrop, position, n, up);
			EmitLayer(Layer::BloodMist, position, n, up);
			return;
		}
		// 2026-11-23 사용자 «섬광 불꽃은 빼» — 섬광(지형 · 충돌체 전부)과 불꽃(충돌체)을 내지 않는다.
		//  근거가 UE4 위키 튜토리얼 · 판매 에셋 설명 · 11-13 추천뿐이고, 조사한 게임(노션 「사격감 레퍼런스 조사」)에는 없었다.
		//  충돌체는 재질을 몰라 전부 Metal 로 묶여 나무 · 바위에도 불꽃이 튀었다. kLayers 의 섬광 · 불꽃 줄은 남겨 두되 쓰지 않는다.
		//  지금: 지형 = 먼지 + 연기, 충돌체 = 연기만.
		if (surface == Surface::Ground)
			EmitLayer(Layer::Dust, position, n, up);
		EmitLayer(Layer::Smoke, position, n, up);
	}

	void ParticleSystem::RememberBodyHit(uint32_t npcId, const Vec3d& position, const Vec3d& normal, const Vec3d& up, const Vec3d& eye)
	{
		if (pending.size() >= kMaxPending)
			pending.erase(pending.begin());
		pending.push_back({ npcId, position, normal, up, eye, 0.0f });
	}

	void ParticleSystem::ConfirmNpcHits(const std::vector<uint32_t>& npcIds)
	{
		for (const uint32_t id : npcIds)
		{
			// 그 NPC 를 맞힌 자리 중 가장 오래된 것 — 서버는 받은 순서대로 판정해 보낸다.
			const auto it = std::find_if(pending.begin(), pending.end(),
				[id](const PendingHit& hit) { return hit.npcId == id; });
			if (it == pending.end()) continue;   // 기억한 자리가 없다(이미 버림 · 다른 맵)
			EmitImpact(Surface::Body, it->position, it->normal, it->up, it->eye);
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

	void ParticleSystem::AddFlipbook(Layer layer, const Flipbook* flipbook, uint16_t group)
	{
		if (layer >= Layer::Count || !flipbook) return;
		std::vector<LayerImage>& images = layerImages[size_t(layer)];
		if (images.size() >= kNoImage) return;   // Sprite::image(16비트)에 못 담는 몫
		// 2026-11-25 — 같은 층 · 같은 group(0 아님)이 이미 있으면 그 표에 들어간다(한꺼번에 나옴). 아니면 새 표.
		uint16_t& choices = layerChoices[size_t(layer)];
		uint16_t choice = choices;
		if (group != 0)
			for (const LayerImage& image : images)
				if (image.group == group) { choice = image.choice; break; }
		if (choice == choices) ++choices;
		images.push_back(LayerImage{ flipbook, nullptr, { 1.0f, 1.0f, 1.0f, 1.0f }, group, choice });
	}

	void ParticleSystem::AddTexture(Layer layer, const ParticleTexture* texture, const XMFLOAT4& tint)
	{
		if (layer >= Layer::Count || !texture) return;
		std::vector<LayerImage>& images = layerImages[size_t(layer)];
		if (images.size() >= kNoImage) return;
		const uint16_t choice = layerChoices[size_t(layer)]++;   // 낱장은 늘 혼자 한 표
		images.push_back(LayerImage{ nullptr, texture, tint, 0, choice });
	}

	void ParticleSystem::ClearImages(Layer layer)
	{
		if (layer >= Layer::Count) return;
		layerImages[size_t(layer)].clear();   // 이미 고른 파티클은 Extract 에서 번호가 범위 밖 → 둥근 모양
		layerChoices[size_t(layer)] = 0;
	}

	void ParticleSystem::Extract(const Vec3d& eye, std::vector<ParticleSprite>& out, uint32_t& alphaCount,
		std::vector<ParticleFlipbook>& flipbooks) const
	{
		out.clear();
		alphaCount = 0;
		flipbooks.clear();
		if (liveCount == 0) return;
		out.reserve(liveCount);

		// ── 이번 프레임의 텍스처 표 (2026-11-23 플립북, 2026-11-24 낱장 텍스처 · 층마다 여러 장) ──
		//  모든 층의 그림을 차례로 한 줄씩, 같은 그림은 한 줄만. imageSlotScratch[layerFirst[층] + 그림 번호] = 표 번호 + 1(0 = 없음 — 둥근 모양).
		//  kMaxParticleFlipbooks 를 넘는 몫은 둥근 모양으로 그린다. 낱장 텍스처는 1 × 1 칸 · 1 프레임 한 줄(렌더러 · 셰이더는 구분하지 않는다).
		std::array<uint32_t, size_t(Layer::Count)> layerFirst{};
		imageSlotScratch.clear();
		slotKeyScratch.clear();
		for (size_t l = 0; l < size_t(Layer::Count); ++l)
		{
			layerFirst[l] = uint32_t(imageSlotScratch.size());
			for (const LayerImage& image : layerImages[l])
			{
				const void* key = nullptr;   // 같은 그림인지 가리는 주소(플립북 또는 낱장 텍스처)
				ParticleFlipbook row;
				if (image.flipbook && image.flipbook->Valid())
				{
					key = image.flipbook;
					row = image.flipbook->Gpu();
				}
				else if (image.texture && image.texture->Valid())
				{
					key = image.texture;
					row = image.texture->Gpu();
				}
				uint32_t slot = 0;
				if (key)
				{
					for (size_t k = 0; k < slotKeyScratch.size() && slot == 0; ++k)
						if (slotKeyScratch[k] == key) slot = uint32_t(k) + 1u;
					if (slot == 0 && flipbooks.size() < kMaxParticleFlipbooks)
					{
						flipbooks.push_back(row);
						slotKeyScratch.push_back(key);
						slot = uint32_t(flipbooks.size());
					}
				}
				imageSlotScratch.push_back(slot);
			}
		}

		auto toSprite = [this, &layerFirst](const Sprite& s)
		{
			const float t = (std::min)(s.age / s.life, 1.0f);   // 0 → 1
			// 플립북 — 수명 동안 한 바퀴(0 → 마지막 프레임). 낱장 텍스처 — 프레임 0 그대로(회전은 p.rotation). 크기 · 옅어짐은 아래와 같다.
			//  고른 그림이 떨어졌으면(ClearImages — 번호가 범위 밖) 둥근 모양.
			const std::vector<LayerImage>& images = layerImages[size_t(s.layer)];
			uint32_t slot = 0;
			const Flipbook* flipbook = nullptr;
			if (s.image != kNoImage && s.image < images.size())
			{
				slot = imageSlotScratch[layerFirst[size_t(s.layer)] + s.image];
				if (slot != 0 && images[s.image].flipbook && images[s.image].flipbook->Valid())
					flipbook = images[s.image].flipbook;
			}
			// 연기 · 먼지는 처음 10 % 동안 짙어지고 끝으로 갈수록 옅어진다. 나머지는 처음부터 옅어진다.
			//  2026-11-25 — 플립북 Settings().layerFade = false 면 안 곱한다(피 시트는 끝맺음이 텍스처 알파에 있다).
			const float fadeIn = (s.flags & kParticleLit) ? (std::min)(t * 10.0f, 1.0f) : 1.0f;
			const float fade = (flipbook && !flipbook->Settings().layerFade) ? 1.0f : fadeIn * (1.0f - t);
			ParticleSprite p;
			p.position = XMFLOAT3(float(s.position.x), float(s.position.y), float(s.position.z));
			p.size = s.size * (1.0f + (s.growth - 1.0f) * t);
			p.color = XMFLOAT4(s.color.x, s.color.y, s.color.z, s.color.w * fade);
			p.rotation = s.rotation;
			p.flags = s.flags;
			if (s.flags & kParticleFixedAxes)   // 2026-11-25 고정 판 축(EmitLayer)
			{
				p.axisX = s.axisX;
				p.axisY = s.axisY;
				p.axisZ = s.axisZ;
			}
			if (slot != 0)
			{
				p.flipbook = slot;
				p.frame = flipbook ? flipbook->FrameAt(t) : 0.0f;
			}
			if (flipbook)   // 2026-11-25 — 기준점(pivot → 사각형 가운데) · 레이 몫
			{
				const FlipbookSettings& fs = flipbook->Settings();
				p.offsetX = 0.5f - fs.pivot.x;   // pivot u 가 왼쪽일수록 가운데는 오른쪽
				p.offsetY = fs.pivot.y - 0.5f;   // pivot v 는 위 = 0 — 위쪽일수록 가운데는 아래
				p.rayCoverage = fs.rayCoverage;
			}
			return p;
		};
		// 정렬 기준 = 사각형 가운데(2026-11-25 — 같은 맞은 점의 정면 · 측면 판이 순서를 바꿔 가며 깜빡이지 않게).
		//  카메라를 보는 사각형은 여기서 카메라 축을 몰라 기준점 그대로.
		auto sortCenter = [](const ParticleSprite& p)
		{
			XMFLOAT3 c = p.position;
			if (p.flags & kParticleFixedAxes)
			{
				c.x += (p.axisX.x * p.offsetX + p.axisY.x * p.offsetY) * p.size;
				c.y += (p.axisX.y * p.offsetX + p.axisY.y * p.offsetY) * p.size;
				c.z += (p.axisX.z * p.offsetX + p.axisY.z * p.offsetY) * p.size;
			}
			return Vec3d{ double(c.x), double(c.y), double(c.z) };
		};

		// 알파 묶음은 먼 것부터(뒤에서 앞으로) — 겹칠 때 순서가 맞아야 한다. 가산 묶음은 순서와 무관하다.
		sortScratch.clear();
		spriteScratch.clear();
		for (uint32_t i = 0; i < uint32_t(sprites.size()); ++i)
		{
			const Sprite& s = sprites[i];
			if (!s.alive || (s.flags & kParticleAdditive)) continue;
			spriteScratch.push_back(toSprite(s));
			const Vec3d d = sortCenter(spriteScratch.back()) - eye;
			sortScratch.emplace_back(float(Dot(d, d)), uint32_t(spriteScratch.size() - 1));
		}
		std::sort(sortScratch.begin(), sortScratch.end(),
			[](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b)
			{ return a.first != b.first ? a.first > b.first : a.second < b.second; });   // 같은 거리면 칸 순서(프레임마다 같게)
		for (const auto& entry : sortScratch)
			out.push_back(spriteScratch[entry.second]);
		alphaCount = uint32_t(out.size());

		for (const Sprite& s : sprites)
			if (s.alive && (s.flags & kParticleAdditive))
				out.push_back(toSprite(s));
	}

} // namespace swc
