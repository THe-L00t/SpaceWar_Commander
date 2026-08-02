#include "AccelStructure.h"
#include "DXCommon.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

	// 가속 구조용 버퍼는 DEFAULT 힙 + ALLOW_UNORDERED_ACCESS 가 필수다.
	bool CreateAsBuffer(ID3D12Device5* device, UINT64 bytes,
		D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
	{
		if (bytes == 0) return false;

		D3D12_HEAP_PROPERTIES heap = swc::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC desc = swc::BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			state, nullptr, IID_PPV_ARGS(&out)));
	}
}

namespace swc {

	bool AccelStructure::Initialize(ID3D12Device5* device, uint32_t maxInst)
	{
		maxInstances = maxInst;

		// 인스턴스 디스크립터 — UPLOAD 힙에 영속 매핑.
		// 매 프레임 GPU 완료를 기다리는 현재 구조라 링 버퍼가 필요 없다.
		D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RESOURCE_DESC desc = BufferDesc(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * maxInstances);
		if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&instanceBuffer))))
			return false;

		D3D12_RANGE noRead = { 0, 0 };
		if (FAILED(instanceBuffer->Map(0, &noRead, reinterpret_cast<void**>(&instanceData))))
			return false;

		// TLAS 는 최대 인스턴스 기준으로 미리 잡아두고 매 프레임 재빌드한다.
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = maxInstances;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

		if (!CreateAsBuffer(device, info.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlasResult))
			return false;
		if (!CreateAsBuffer(device, info.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tlasScratch))
			return false;

		return true;
	}

	uint32_t AccelStructure::AddMesh(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmd,
		const D3D12_RAYTRACING_GEOMETRY_DESC& geometry)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &geometry;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

		Blas b;
		if (!CreateAsBuffer(device, info.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, b.result))
			return kInvalidBlas;
		if (!CreateAsBuffer(device, info.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b.scratch))
			return kInvalidBlas;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = b.result->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = b.scratch->GetGPUVirtualAddress();
		cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

		D3D12_RESOURCE_BARRIER barrier = UavBarrier(b.result.Get());
		cmd->ResourceBarrier(1, &barrier);

		blas.push_back(std::move(b));
		return static_cast<uint32_t>(blas.size() - 1);
	}

	void AccelStructure::AddInstance(uint32_t blasIndex, const XMFLOAT4X4& world)
	{
		if (instanceCount >= maxInstances || blasIndex >= blas.size() || !instanceData)
			return;

		D3D12_RAYTRACING_INSTANCE_DESC& d = instanceData[instanceCount];

		// DirectXMath 는 행벡터·행우선, D3D12 인스턴스는 열벡터 3x4 → 전치해서 넣는다.
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 4; ++c)
				d.Transform[r][c] = world.m[c][r];

		d.InstanceID = instanceCount;
		d.InstanceMask = 0xFF;
		d.InstanceContributionToHitGroupIndex = 0;
		d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
		d.AccelerationStructure = blas[blasIndex].result->GetGPUVirtualAddress();

		++instanceCount;
	}

	void AccelStructure::BuildTlas(ID3D12GraphicsCommandList4* cmd)
	{
		if (instanceCount == 0 || !tlasResult)
			return;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = instanceCount;
		inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
		cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

		D3D12_RESOURCE_BARRIER barrier = UavBarrier(tlasResult.Get());
		cmd->ResourceBarrier(1, &barrier);
	}

	D3D12_GPU_VIRTUAL_ADDRESS AccelStructure::TlasAddress() const
	{
		return tlasResult ? tlasResult->GetGPUVirtualAddress() : 0;
	}
}
