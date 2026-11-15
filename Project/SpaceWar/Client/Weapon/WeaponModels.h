#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "Client/Model.h"
#include "Shared/GameObject/Weapon.h"

// ============================================================
//  Client/Weapon/WeaponModels.h — 총 모델 (2026-11-14)
//
//  총 종류(Shared::WeaponType)마다 1인칭 · 3인칭 두 벌을 GPU 에 한 번 올리고, 쓸 곳마다 Scene 노드만 붙인다
//  (캐릭터 모델과 같은 방식 — Model.h). 파일은 Shared::SpecOf(type) 의 firstPersonAsset · thirdPersonAsset.
//  명세 21절 «작업 중 추가된 클래스»(노션, 2026-11-14). 명세 1절에 Model 자리가 없는 것과 같은 이유로 클라에만 있다.
//
//  ★ 어느 쪽을 그리나(2026-11-14 사용자)
//    · 자기가 든 총         → 1인칭 (클라이언트 자신이 든 총은 모두 1인칭)
//    · 다른 플레이어가 든 총 → 3인칭
//    · 바닥에 떨어진 총     → 가까우면 1인칭 · 멀면 3인칭 (두 파일의 원점(손잡이) · 크기가 같아 바꿔도 위치가 안 튄다)
//  ★ 3인칭 파일의 충돌체 노드 COL_<이름> 은 읽을 때 메시가 떨어진다(GltfReader hideAuthoringHelpers) — 화면에 안 나온다.
//  ★ 메인 스레드 소유. 로드 묶음의 끝(메인)에서 Initialize 하고 바로 CPU 사본을 버린다(Engine::BuildGameLoad).
// ============================================================

namespace swc {

	class GRenderer;
	class LegacyScene;

	enum class WeaponView : uint8_t
	{
		FirstPerson = 0,
		ThirdPerson,
		Count,
	};
	inline constexpr size_t kWeaponViewCount = size_t(WeaponView::Count);

	class WeaponModels
	{
	public:
		// exe 옆 assets\ 기준 파일 경로(Shared::WeaponSpec 의 값).
		static const wchar_t* Asset(Shared::WeaponType type, WeaponView view);

		// 읽은 모델로 GPU 메시 · 재질을 만든다. 표시 보정 없이 glb 의 미터 · 원점(손잡이) 그대로 둔다.
		bool Initialize(Shared::WeaponType type, WeaponView view, const ModelData& data, GRenderer& renderer);

		bool Ready(Shared::WeaponType type, WeaponView view) const;
		size_t ReadyCount() const;

		// 그 모델을 Scene 에 붙인다. 반환값 = 이동 루트(총 원점 = 손잡이). 아직 안 올렸으면 kInvalidNode.
		NodeHandle Instantiate(Shared::WeaponType type, WeaponView view, LegacyScene& scene,
			NodeHandle parent = kInvalidNode) const;

		const std::wstring& LastError() const { return lastError; }

	private:
		static bool Valid(Shared::WeaponType type, WeaponView view)
		{
			return Shared::IsWeaponType(type) && size_t(view) < kWeaponViewCount;
		}

		Model        models[Shared::kWeaponTypeCount][kWeaponViewCount];
		bool         ready[Shared::kWeaponTypeCount][kWeaponViewCount] = {};
		std::wstring lastError;
	};

} // namespace swc
