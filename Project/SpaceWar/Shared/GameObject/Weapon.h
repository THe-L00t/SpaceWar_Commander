#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================
//  Shared/GameObject/Weapon.h — 무기 (2026-11-14)
//
//  아키텍처 명세서 6.6.2 · 6.7.2 — Player · OtherPlayer 가 Weapon 을 든다.
//  명세에 Weapon 자체의 절이 없어 21절 «작업 중 추가된 클래스» 에 정리했다(노션, 2026-11-14).
//  근거: 노션 「무기 판정 방식 — 히트스캔 vs 실제 투사체」 3장(총알은 객체가 아니다 —
//        연사 간격 · 탄 · 반동 · 사거리는 Weapon 에 붙는다) ·
//        회의록 2026-11-03 ToDo(Laser Rifle · 권총 · 기관총, 장탄수 · 장전 키, 땅바닥의 총으로 무기 교체).
//
//  ★ 두 부분 — 총 종류 표(WeaponSpec, 종류마다 한 줄 · 바뀌지 않는다)와 무기 한 자루의 상태(Weapon).
//    새 총 = Weapon.cpp 의 kSpecs 에 한 줄 + WeaponType 에 하나 + assets/model/weapon/<이름>/ 폴더(1인칭 · 3인칭 glb).
//  ★ Weapon 은 Object 를 상속하지 않는다 — 손에 든 «상태» 다.
//    바닥에 떨어진 총(WeaponPickup, 예정 — 명세 21절)이 오브젝트가 되고 이 상태를 안에 든다.
//  ★ 한 사람이 한 번에 한 자루(2026-11-14 사용자 «일단은 총기 하나만 들고 있게»).
//    빈손(버리기 키를 누른 뒤)은 type == WeaponType::None 이다.
//  ★ 수치는 제안값이다(2026-11-14, 노션 「사격감 레퍼런스 조사」 범위에서 고름).
//    2026-11-15 부터 판정에 쓴다 — 사용자 «주우면 … 쏠 수 있도록 해줘»: 든 총의 발사 모드 · 발 간격(클라 · 서버) · 피해 · 사거리(서버).
//    빈손이면 쏘지 못한다.
//  ★ 2026-11-27 탄 · 장전 (사용자 «무기에 현재 탄수를 추가해서 장전과 UI 연결을 구현해줘») — 현재 탄수 = Weapon::magazine(탄창에 남은 탄).
//    한 발마다 하나 줄고, 0 이면 못 쏜다. 장전 = 장전 키 → reloadSeconds 뒤 탄창을 가득 채운다(장전 중에는 못 쏜다).
//    규칙은 아래 Weapon 함수 한 곳 — 서버(판정) · 오프라인 클라가 같이 부른다. 접속 중에는 서버가 원본이고 클라는 받은 값을 그린다(명세 18절 원칙 4).
//    ★ 예비탄은 없다(2026-11-27 사용자 «예비탄없애고 최대 장탄수만 나오게 해. 클래스에서도 예비탄 부분 빼. 몇 번이고 장전가능하게 할거야»)
//      — 장전은 몇 번이고 된다. HUD = «현재 장탄수 / 최대 장탄수»(magazine / WeaponSpec::magazineSize).
//      (그 전에는 팀원 HUD 명세 transfer = min(capacity − ammo, reserve) 로 예비탄에서 옮겼다 — 같은 날 뺐다.)
//    Laser Rifle 의 방식(한 발씩 빔 / 연속 빔)은 기획 확인 대기(2026-11-14 사용자 «기획 팀원에게 물어볼게») — 지금은 한 발씩.
//    라이플 · 기관총은 누르고 있으면 간격마다 쏜다(FireMode::Auto — 2026-11-15 사용자 «눌려있다면 해당 속도에 맞게 자동 발사되게 하자»).
//    권총은 클릭마다 한 발(FireMode::Single — 같은 날 사용자 «권총은 자동 발사 없애주고, 클릭 시에만 발사»).
//      간격(0.2 s 게이트) 안에 들어온 클릭은 버린다(같은 날 사용자 «0.2 게이트 닫힌 사이에 들어온 클릭 신호는 버려줘»).
//  ★ 그리기(클라, Client/Weapon/WeaponModels.h) — 자기가 든 총 = 1인칭 모델, 남이 든 총 = 3인칭,
//    바닥의 총 = 가까우면 1인칭 · 멀면 3인칭(2026-11-14 사용자).
// ============================================================

namespace Shared {

	struct CollisionBox;   // Shared/Physics/CollisionWorld.h — 총 충돌 상자(2026-11-15)

	// 총 종류 — 값이 kSpecs 의 줄 번호다. 패킷에 넣을 때도 이 값을 쓴다(★ 순서를 바꾸지 말고 Count 앞에 붙인다).
	enum class WeaponType : uint8_t
	{
		Pistol = 0,
		LaserRifle,
		MachineGun,
		Count,
		None = 0xFF,   // 빈손
	};
	inline constexpr size_t kWeaponTypeCount = size_t(WeaponType::Count);

	enum class FireMode : uint8_t
	{
		Single,   // 누를 때마다 한 발 — 간격 안의 클릭은 버린다(2026-11-15)
		Auto,     // 누르고 있으면 발 간격마다 한 발
	};

	enum class AmmoKind : uint8_t
	{
		Bullet,   // 총알 — 눈에 안 보이므로 맞은 자리에 피 · 불꽃(회의록 11-03)
		Laser,    // 레이저 빔 — 빔 자체가 보인다
	};

	// 총 종류 하나 — 바뀌지 않는 값
	struct WeaponSpec
	{
		const char*    name;               // 영문 — assets/model/weapon/<name>/ 폴더 이름과 같다
		const wchar_t* firstPersonAsset;   // exe 옆 assets\ 기준 — 1인칭(자기가 든 총 · 가까운 바닥 총)
		const wchar_t* thirdPersonAsset;   // 3인칭(남이 든 총 · 먼 바닥 총). 파일의 COL_<name>(삼각형 5만)은 게임 충돌에 쓰지 않는다 — colliderBoxes
		FireMode       fireMode;
		AmmoKind       ammoKind;
		float          fireInterval;       // 발 간격 (s)
		float          damage;             // 한 발 피해
		float          range;              // 사거리 (m)
		uint16_t       magazineSize;       // 탄창 = 최대 장탄수(장전하면 이만큼 찬다)
		float          reloadSeconds;      // 장전 시간 (s)
		uint8_t        laserBoltEvery;     // 레이저탄(보이는 빔) — N 발마다 하나(1 = 매 발, 0 = 없음). 2026-11-27 tracerEvery(예광탄) → 레이저탄
		float          restHeight;         // 옆으로 눕혔을 때 원점(손잡이)에서 바닥까지 (m) — 3인칭 COL_ 의 X 반폭(실측)
		float          muzzle[3];          // 총구 — 총 glb 의 Muzzle 노드 자리(m, 총 축). 발사 원점 · 레이저탄 시작 (2026-11-15)
		// 충돌 상자 묶음 (2026-11-15 사용자 «1인칭 메쉬 기준으로 각 총기를 최소크기와 최소 개수로 총을 포함할 수 있는 육면체 그룹») —
		//  총 glb 축(+X 옆 · +Y 위 · +Z 총구)에 맞춘 상자들. 1인칭 메시의 삼각형을 전부 덮는다. 값 · 만든 법은 Weapon.cpp kPistolBoxes 위 주석.
		const CollisionBox* colliderBoxes;
		uint32_t            colliderBoxCount;
	};

	// 종류의 표 한 줄. None · 범위 밖이면 모든 값이 0 인 빈 줄을 돌려준다.
	const WeaponSpec& SpecOf(WeaponType type);

	inline bool IsWeaponType(WeaponType type) { return size_t(type) < kWeaponTypeCount; }

	// 무기 한 자루의 상태 — 손에 든 총(Player · OtherPlayer)이나 바닥 총 안에 든다.
	//  ★ 원본은 서버가 든다(명세 18절 원칙 4). 클라는 받은 값으로 UI · 효과만 먼저 보여 준다.
	//  ★ 2026-11-27 바닥 총도 이 상태를 든다 — 떨군 총을 다시 주우면 남은 탄 그대로다(공중 진열에서 주우면 가득).
	class Weapon
	{
	public:
		Weapon() = default;                 // 빈손
		explicit Weapon(WeaponType type);   // 그 종류를 탄창 가득 채워 든다

		bool              Held() const { return IsWeaponType(type); }
		const WeaponSpec& Spec() const { return SpecOf(type); }

		// ── 탄 · 장전 (2026-11-27) ──
		bool  Reloading() const { return reloadLeft > 0.0f; }
		bool  CanFire() const;                 // 들고 있고 · 장전 중이 아니고 · 탄창에 탄이 있다
		bool  ConsumeShot();                   // CanFire 면 한 발 빼고 true
		bool  CanReload() const;               // 들고 있고 · 장전 중이 아니고 · 탄창이 덜 찼다(예비탄 없음 — 몇 번이고 된다)
		bool  StartReload();                   // CanReload 면 장전을 시작하고 true(장전 시간 0 이면 바로 채운다)
		bool  AdvanceReload(float seconds);    // 장전 중이면 시간을 줄이고, 다 되면 채운 뒤 true
		void  CancelReload() { reloadLeft = 0.0f; }   // 탄은 그대로(팀원 명세 «중간에 취소하면 원래 탄약 수치를 유지»)
		float ReloadProgress() const;          // 0 ~ 1 (장전 중이 아니면 0)

		WeaponType type = WeaponType::None;
		uint16_t   magazine = 0;             // 현재 탄수 — 탄창에 남은 탄
		float      reloadLeft = 0.0f;        // 장전 남은 시간 (s). 0 = 장전 중 아님 (2026-11-27)

	private:
		void FinishReload();                 // 탄창을 가득(magazineSize) 채운다
	};

} // namespace Shared
