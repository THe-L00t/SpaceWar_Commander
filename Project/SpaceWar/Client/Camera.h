#pragma once
#include <DirectXMath.h>

// 아주 기본적인 3인칭 카메라: 타겟(플레이어) 뒤·위에서 타겟을 바라본다.
namespace swc {
	class Camera
	{
	public:
		void SetAspect(float a) { aspect = a; }

		void FollowTarget(const DirectX::XMFLOAT3& target)
		{
			using namespace DirectX;
			const XMVECTOR t = XMLoadFloat3(&target);
			const XMVECTOR eye = XMVectorAdd(t, XMVectorSet(0.0f, 8.0f, -15.0f, 0.0f));
			const XMVECTOR at = XMVectorAdd(t, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
			const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

			const XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
			const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 500.0f);
			XMStoreFloat4x4(&viewProj, view * proj);
		}

		const DirectX::XMFLOAT4X4& ViewProj() const { return viewProj; }

	private:
		float aspect = 16.0f / 9.0f;
		DirectX::XMFLOAT4X4 viewProj{};
	};
}
