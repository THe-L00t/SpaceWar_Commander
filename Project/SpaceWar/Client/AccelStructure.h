#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>
#include <DirectXMath.h>

// BLAS(메쉬당 1개, 최초 1회) + TLAS(매 프레임 재빌드).
// 렌더러 내부 전용 헤더 — 게임 로직은 절대 include 하지 않는다.
namespace swc {
	class AccelStructure
	{
	public:
		bool Initialize(ID3D12Device5*, uint32_t maxInstances);

		// 메쉬 하나의 BLAS 를 빌드 (cmd 에 기록만 하고, 실행·대기는 호출자 책임)
		uint32_t AddMesh(ID3D12Device5*, ID3D12GraphicsCommandList4*,
			const D3D12_RAYTRACING_GEOMETRY_DESC&);

		void ResetInstances() { instanceCount = 0; }
		void AddInstance(uint32_t blasIndex, const DirectX::XMFLOAT4X4& world);
		void BuildTlas(ID3D12GraphicsCommandList4*);

		D3D12_GPU_VIRTUAL_ADDRESS TlasAddress() const;
		uint32_t InstanceCount() const { return instanceCount; }

		static constexpr uint32_t kInvalidBlas = 0xFFFFFFFFu;

	private:
		struct Blas
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> result;
			Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
		};

		std::vector<Blas> blas;
		Microsoft::WRL::ComPtr<ID3D12Resource> tlasResult;
		Microsoft::WRL::ComPtr<ID3D12Resource> tlasScratch;
		Microsoft::WRL::ComPtr<ID3D12Resource> instanceBuffer;

		D3D12_RAYTRACING_INSTANCE_DESC* instanceData = nullptr;   // UPLOAD 힙 영속 매핑
		uint32_t instanceCount = 0;
		uint32_t maxInstances = 0;
	};
}
