#include "Weapon.h"
#include "../Physics/CollisionWorld.h"   // CollisionBox (2026-11-15 총 충돌 상자)

namespace Shared {

	namespace {

		// ── 총 충돌 상자 (2026-11-15) — WeaponSpec::colliderBoxes ──
		//  사용자: «지금 1인칭과 3인칭의 메쉬 크기 규격은 거의 동일하지? 1인칭 메쉬 기준으로 각 총기를 최소크기와 최소 개수로
		//         총을 포함할 수 있는 육면체 그룹을 만들어서 해당 그룹을 충돌체로 사용하자. 총에 부딪힐때 해당 처리때문에 프레임이 10까지 떨어져»
		//  만든 법(스크립트: 졸작/Resource_Work/Weapons/충돌상자_1115/스크립트/box_fit.py · box_export.py, 그림 · 목록.md 같은 폴더):
		//   ① 1인칭 glb 의 삼각형 전부(권총 133만 · 라이플 76만 · 기관총 92만)를 경계 상자 하나로 덮는다.
		//   ② «삼각형 중심을 한 축으로 정렬해 앞 · 뒤로 나눴을 때 두 상자 부피 합이 가장 작은 자리» 로, 부피가 가장 많이 주는 상자를 하나씩 나눈다.
		//      삼각형은 통째로 한 상자에 든다 → 상자들이 메시를 빠짐없이 덮는다.
		//   ③ 상자 수 = 하나 더 나눠도 부피 합이 5 % 미만 줄어드는 곳에서 멈춘 수(권총 3 · 라이플 5 · 기관총 6 — 상자 1개의 69 · 57 · 44 %).
		//   ④ 0.1 mm 단위로 바깥쪽 반올림. 검사: 1인칭 정점 · 삼각형 전부가 상자 안(box_export.py).
		//  축 = 총 glb 축(+X 옆 · +Y 위 · +Z 총구, 원점 손잡이) — 1인칭 · 3인칭이 같은 원점 · 크기라(차이 0.7 mm 이하) 둘 다 이 상자를 쓴다.
		//  옆 축(X) 둘레로 기울인 상자도 재 봤으나 부피가 0.1 % 이하로만 줄어 축에 맞춘 상자로 둔다.
		constexpr CollisionBox kPistolBoxes[] =
		{
			// 최소 (x, y, z)                         최대 (x, y, z)                    — m
			{ { -0.0149f, -0.0731f, -0.0410f }, { +0.0148f, +0.0608f, +0.0297f } },   // 손잡이 · 뒤      30 × 134 × 71 mm
			{ { -0.0157f, -0.0257f, +0.0242f }, { +0.0156f, +0.0588f, +0.1012f } },   // 가운데 · 방아쇠  31 × 84 × 77 mm
			{ { -0.0163f, -0.0016f, +0.0969f }, { +0.0163f, +0.0640f, +0.1591f } },   // 총구 쪽          33 × 66 × 62 mm
		};
		constexpr CollisionBox kLaserRifleBoxes[] =
		{
			{ { -0.0307f, -0.0654f, -0.2873f }, { +0.0305f, +0.1146f, -0.0804f } },   // 개머리          61 × 180 × 207 mm
			{ { -0.0204f, -0.1339f, -0.0801f }, { +0.0204f, +0.0199f, +0.0815f } },   // 손잡이          41 × 154 × 162 mm
			{ { -0.0376f, +0.0153f, -0.0812f }, { +0.0376f, +0.1696f, +0.0815f } },   // 위 조준경        75 × 154 × 163 mm
			{ { -0.0320f, -0.1267f, +0.0683f }, { +0.0319f, +0.1220f, +0.2453f } },   // 탄창 · 가운데 몸통 64 × 249 × 177 mm
			{ { -0.0319f, -0.0082f, +0.2284f }, { +0.0318f, +0.1374f, +0.5628f } },   // 앞 몸통 · 총열   64 × 146 × 334 mm
		};
		constexpr CollisionBox kMachineGunBoxes[] =
		{
			{ { -0.0215f, -0.1315f, -0.0778f }, { +0.0220f, -0.0630f, +0.0180f } },   // 손잡이 아래      44 × 68 × 96 mm
			{ { -0.0402f, -0.0706f, -0.2536f }, { +0.0407f, +0.1691f, +0.1378f } },   // 개머리 · 뒤 몸통  81 × 240 × 391 mm
			{ { -0.0913f, -0.1381f, +0.1288f }, { +0.0913f, +0.0516f, +0.3405f } },   // 탄통            183 × 190 × 212 mm
			{ { -0.0504f, +0.0480f, +0.1268f }, { +0.0502f, +0.1810f, +0.3431f } },   // 위 손잡이        101 × 133 × 216 mm
			{ { -0.0599f, -0.0576f, +0.3366f }, { +0.0597f, +0.1642f, +0.6596f } },   // 앞 몸통          120 × 222 × 323 mm
			{ { -0.0267f, +0.0391f, +0.6420f }, { +0.0266f, +0.0928f, +0.7965f } },   // 총열            53 × 54 × 154 mm
		};
		template <size_t N> constexpr uint32_t BoxCount(const CollisionBox (&)[N]) { return static_cast<uint32_t>(N); }

		// ── 총 종류 표 — 조절은 여기서 (2026-11-14 제안값, 2026-11-15 부터 판정에 쓴다 — Weapon.h 머리 주석) ──
		//  근거 범위(노션 「사격감 레퍼런스 조사」 0절 · 2절): 권총 분당 240 ~ 450발 · 기관총 545 ~ 936발 · 탄창 50 ~ 150,
		//  Laser Rifle 은 현실 게임에 짝이 없어 반자동 DMR(분당 240 ~ 390발)을 기준으로 잡았다(기획 확인 대기).
		//  권총의 발 간격 · 피해는 예전 서버 값(GameLogic.h kFireInterval 0.2 s · kShotDamage 25)과 같다. 사거리는 아래 «사거리» 줄.
		//  눕힌 높이 = 3인칭 glb 의 COL_<이름> X 반폭(2026-11-14 파일에서 잼).
		//  예광 = 세 총 모두 매 발(2026-11-15 사용자 «예광탄 우선 매 발 나가게 해줘» — 기관총 3발마다 → 매 발. 조사의 CS2 기관총도 매 발).
		//  모드 = 라이플 · 기관총 Auto(2026-11-15 사용자 «눌려있다면 해당 속도에 맞게 자동 발사되게 하자» — 라이플 Single → Auto).
		//    누르고 있으면 그 총의 간격마다 한 발씩 나간다. Laser Rifle 의 «연속 빔» 여부는 여전히 기획 확인 대기 — 지금은 한 발씩 되풀이.
		//    권총 = Single(같은 날 사용자 «권총은 자동 발사 없애주고, 클릭 시에만 발사. 그리고 연속 클릭이 들어오면 0.2초마다 게이트를 열어서
		//    딜레이를 줄 수 있게해줘» — 잠깐 Auto 였다가 되돌림). 클릭마다 한 발, 간격(0.2 s) 안에 누른 클릭은 버린다
		//    (같은 날 사용자 «0.2 게이트 닫힌 사이에 들어온 클릭 신호는 버려줘» — Client Engine::UpdateFire).
		//  간격 = 사용자 지정(2026-11-15 «라이플은 초당 9발 기관총 초당 11발로 수정해줘») — 라이플 0.25 s → 1/9 s(분당 540) ·
		//    기관총 0.08 s → 1/11 s(분당 660). 권총은 제안값 0.20 s(초당 5발) 그대로.
		//  사거리 = 세 총 모두 130 m(2026-11-16 사용자 «사거리 300이 의미 없을거 같아서 전부 130정도로 수정해줘» — 300 → 130).
		//    행성 반지름 650 m 라 땅에 닿는 겨눈 점은 지평선(우클릭 조준 약 55 m · 평소 약 67 m)까지이고,
		//    지평선 위로 솟은 몸(머리)도 약 100 ~ 115 m 까지만 보인다. 서버 판정(ResolveFire) · 클라 겨눈 점(AimDirection)이 이 값을 쓴다.
		//  ★ 줄 순서 = WeaponType 순서.
		constexpr WeaponSpec kSpecs[kWeaponTypeCount] =
		{
		//  총구 = 3인칭 glb 의 Muzzle 노드 translation(2026-11-15 파일에서 읽음 — 1인칭과 같은 자리).
			// 이름          1인칭 glb                                      3인칭 glb                                      모드              탄               간격    피해   사거리  탄창  예비탄          장전   예광  눕힌 높이  총구 (x, y, z)                충돌 상자
			{ "Pistol",     L"model\\weapon\\Pistol\\Pistol_FP.glb",         L"model\\weapon\\Pistol\\Pistol_TP.glb",         FireMode::Single, AmmoKind::Bullet, 0.20f, 25.0f, 130.0f,  12, kInfiniteAmmo, 1.5f, 1, 0.0162f, { 0.0f, 0.0349f, 0.1590f }, kPistolBoxes,     BoxCount(kPistolBoxes) },
			{ "LaserRifle", L"model\\weapon\\LaserRifle\\LaserRifle_FP.glb", L"model\\weapon\\LaserRifle\\LaserRifle_TP.glb", FireMode::Auto,   AmmoKind::Laser,  1.0f / 9.0f, 50.0f, 130.0f,  10,  40,           2.5f, 1, 0.0374f, { 0.0f, 0.0676f, 0.5627f }, kLaserRifleBoxes, BoxCount(kLaserRifleBoxes) },
			{ "MachineGun", L"model\\weapon\\MachineGun\\MachineGun_FP.glb", L"model\\weapon\\MachineGun\\MachineGun_TP.glb", FireMode::Auto,   AmmoKind::Bullet, 1.0f / 11.0f, 15.0f, 130.0f, 100, 200,           4.5f, 1, 0.0909f, { 0.0f, 0.0660f, 0.7964f }, kMachineGunBoxes, BoxCount(kMachineGunBoxes) },
		};

		constexpr WeaponSpec kNoSpec = { "", L"", L"", FireMode::Single, AmmoKind::Bullet, 0.0f, 0.0f, 0.0f, 0, 0, 0.0f, 1, 0.0f, { 0.0f, 0.0f, 0.0f }, nullptr, 0 };

	} // namespace

	const WeaponSpec& SpecOf(WeaponType type)
	{
		return IsWeaponType(type) ? kSpecs[size_t(type)] : kNoSpec;
	}

	Weapon::Weapon(WeaponType weaponType)
		: type(weaponType)
		, magazine(SpecOf(weaponType).magazineSize)
		, reserve(SpecOf(weaponType).reserveAmmo)
	{
	}

} // namespace Shared
