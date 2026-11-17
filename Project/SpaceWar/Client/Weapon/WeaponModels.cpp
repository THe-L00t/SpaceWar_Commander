#include "Client/Weapon/WeaponModels.h"
#include <DirectXMath.h>

using namespace DirectX;

namespace swc {

	const wchar_t* WeaponModels::Asset(Shared::WeaponType type, WeaponView view)
	{
		const Shared::WeaponSpec& spec = Shared::SpecOf(type);
		return view == WeaponView::FirstPerson ? spec.firstPersonAsset : spec.thirdPersonAsset;
	}

	bool WeaponModels::Initialize(Shared::WeaponType type, WeaponView view, const ModelData& data, GRenderer& renderer)
	{
		lastError.clear();
		if (!Valid(type, view))
		{
			lastError = L"총 종류 · 시점 번호가 범위 밖입니다.";
			return false;
		}
		const size_t t = size_t(type);
		const size_t v = size_t(view);
		if (ready[t][v])
		{
			lastError = L"이미 올린 총 모델입니다.";
			return false;
		}

		// 표시 보정 없음 — glb 가 실제 크기(미터) · 원점 = 손잡이로 나온다(노션 「총기 리소스 파이프라인」 3절 ⑤).
		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());
		if (!models[t][v].Initialize(data, renderer, identity))
		{
			lastError = models[t][v].LastError();
			return false;
		}
		ready[t][v] = true;
		return true;
	}

	bool WeaponModels::Ready(Shared::WeaponType type, WeaponView view) const
	{
		return Valid(type, view) && ready[size_t(type)][size_t(view)];
	}

	size_t WeaponModels::ReadyCount() const
	{
		size_t count = 0;
		for (size_t t = 0; t < Shared::kWeaponTypeCount; ++t)
			for (size_t v = 0; v < kWeaponViewCount; ++v)
				if (ready[t][v]) ++count;
		return count;
	}

	NodeHandle WeaponModels::Instantiate(Shared::WeaponType type, WeaponView view, LegacyScene& scene,
		NodeHandle parent) const
	{
		if (!Ready(type, view)) return kInvalidNode;
		return models[size_t(type)][size_t(view)].Instantiate(scene, parent);
	}

} // namespace swc
