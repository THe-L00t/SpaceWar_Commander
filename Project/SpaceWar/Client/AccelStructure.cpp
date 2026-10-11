#include "AccelStructure.h"
#include "DXCommon.h"

#include <cstring>
#include <utility>

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

		// 인스턴스 디스크립터 — UPLOAD 힙에 영속 매핑. 프레임 슬롯만큼 잡는다(kFrameSlots 주석 참조).
		D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RESOURCE_DESC desc = BufferDesc(
			sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * maxInstances * kFrameSlots);
		if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&instanceBuffer))))
			return false;

		D3D12_RANGE noRead = { 0, 0 };
		if (FAILED(instanceBuffer->Map(0, &noRead, reinterpret_cast<void**>(&instanceBase))))
			return false;
		instanceData = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(instanceBase);

		// TLAS 는 최대 인스턴스 기준으로 미리 잡아두고 매 프레임 재빌드한다.
		// ★ PREFER_FAST_BUILD 로 둔다 (2026-10-08)
		//   TLAS 는 매 프레임 처음부터 다시 빌드한다. FAST_TRACE 는 빌드가 비싸고,
		//   인스턴스 수십 개 규모에서 트래버설 이득은 미미하다 — 빌드 비용이 더 큰 쪽이다.
		//   (BLAS 는 한 번만 빌드하고 계속 추적하므로 FAST_TRACE 를 유지한다)
		//   ※ 프리빌드 질의와 실제 빌드의 플래그는 반드시 같아야 한다 — 크기가 달라진다.
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		inputs.NumDescs = maxInstances;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

		if (!CreateAsBuffer(device, info.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlasResult))
			return false;
		if (!CreateAsBuffer(device, info.ScratchDataSizeInBytes,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tlasScratch))
			return false;

		// ── 압축 크기 질의용 버퍼 (8바이트) ──────────────
		//  EmitRaytracingAccelerationStructurePostbuildInfo 는 UAV 에만 쓸 수 있어서
		//  DEFAULT 힙에 쓰고 READBACK 으로 복사해 CPU 가 읽는다.
		//  메쉬 하나를 만들 때마다 GPU 대기가 끼므로 하나를 돌려 쓴다.
		if (!CreateAsBuffer(device, sizeof(uint64_t),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, postbuildWrite))
			return false;

		D3D12_HEAP_PROPERTIES readback = HeapProps(D3D12_HEAP_TYPE_READBACK);
		D3D12_RESOURCE_DESC readDesc = BufferDesc(sizeof(uint64_t));
		if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &readDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&postbuildRead))))
			return false;

		return true;
	}

	uint32_t AccelStructure::AddMesh(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmd,
		const D3D12_RAYTRACING_GEOMETRY_DESC& geometry)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		// ★ ALLOW_COMPACTION 추가 — 이 플래그가 있어야 빌드 뒤 압축 크기를 질의하고
		//   COPY_MODE_COMPACT 로 옮겨 담을 수 있다. 정적 지오메트리는 보통 20~50% 줄어든다.
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &geometry;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

		Blas b;
		b.resultBytes = info.ResultDataMaxSizeInBytes;
		b.scratchBytes = info.ScratchDataSizeInBytes;
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

		// 압축 크기 질의를 같은 커맨드에 기록한다. CPU 는 GPU 대기 뒤에 읽는다(CompactMesh).
		if (postbuildWrite && postbuildRead)
		{
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query = {};
			query.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
			query.DestBuffer = postbuildWrite->GetGPUVirtualAddress();

			const D3D12_GPU_VIRTUAL_ADDRESS source = b.result->GetGPUVirtualAddress();
			cmd->EmitRaytracingAccelerationStructurePostbuildInfo(&query, 1, &source);

			D3D12_RESOURCE_BARRIER toCopy = {};
			toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toCopy.Transition.pResource = postbuildWrite.Get();
			toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			cmd->ResourceBarrier(1, &toCopy);

			cmd->CopyBufferRegion(postbuildRead.Get(), 0, postbuildWrite.Get(), 0, sizeof(uint64_t));

			// 다음 메쉬가 다시 UAV 로 쓰도록 상태를 되돌린다.
			std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
			cmd->ResourceBarrier(1, &toCopy);
		}

		blas.push_back(std::move(b));
		return static_cast<uint32_t>(blas.size() - 1);
	}

	bool AccelStructure::CompactMesh(ID3D12Device5* device, ID3D12GraphicsCommandList4* cmd,
		uint32_t blasIndex)
	{
		if (blasIndex >= blas.size() || !postbuildRead) return false;
		Blas& b = blas[blasIndex];
		if (!b.result || !b.scratch) return false;   // 이미 마무리된 BLAS

		// ★ AddMesh 의 커맨드가 GPU 에서 끝난 뒤여야 이 값이 유효하다(호출자가 기다린다).
		uint64_t compactedBytes = 0;
		void* mapped = nullptr;
		D3D12_RANGE range = { 0, sizeof(uint64_t) };
		if (FAILED(postbuildRead->Map(0, &range, &mapped)) || !mapped) return false;
		memcpy(&compactedBytes, mapped, sizeof(compactedBytes));
		D3D12_RANGE noWrite = { 0, 0 };
		postbuildRead->Unmap(0, &noWrite);

		// 이득이 없으면 그대로 둔다. 커밋 리소스는 64KB 단위라 그보다 작은 차이는 의미가 없다.
		if (compactedBytes == 0 || compactedBytes + (64ull * 1024ull) >= b.resultBytes)
			return false;

		if (!CreateAsBuffer(device, compactedBytes,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, b.compacted))
			return false;

		cmd->CopyRaytracingAccelerationStructure(
			b.compacted->GetGPUVirtualAddress(), b.result->GetGPUVirtualAddress(),
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);

		D3D12_RESOURCE_BARRIER barrier = UavBarrier(b.compacted.Get());
		cmd->ResourceBarrier(1, &barrier);

		compactionSaved += b.resultBytes - compactedBytes;
		b.resultBytes = compactedBytes;
		return true;
	}

	void AccelStructure::FinishCompaction(uint32_t blasIndex)
	{
		if (blasIndex >= blas.size()) return;
		Blas& b = blas[blasIndex];

		// 압축본이 있으면 그것을 결과로 삼고 원본을 버린다.
		// ★ AddInstance 가 매 프레임 result 의 주소를 새로 읽으므로 교체해도 안전하다.
		if (b.compacted)
			b.result = std::move(b.compacted);

		// ★ scratch 는 빌드 중에만 쓰인다. 들고 있으면 결과만큼의 메모리가 그냥 잠긴다.
		b.scratch.Reset();
		b.scratchBytes = 0;
	}

	uint64_t AccelStructure::BlasResultBytes() const
	{
		uint64_t total = 0;
		for (const Blas& b : blas) total += b.resultBytes;
		return total;
	}

	uint64_t AccelStructure::BlasScratchBytes() const
	{
		uint64_t total = 0;
		for (const Blas& b : blas) total += b.scratchBytes;
		return total;
	}

	void AccelStructure::ResetInstances(uint32_t slot)
	{
		// ★ 프레임마다 다른 슬롯에 쓴다. 지금은 프레임마다 GPU 를 기다리므로 효과가 없지만,
		//   비행 중 프레임(M1)이 들어오면 이 분리가 깜빡임을 막는다.
		frameSlot = kFrameSlots ? (slot % kFrameSlots) : 0;
		instanceData = instanceBase
			? reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(
				instanceBase + size_t(frameSlot) * maxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC))
			: nullptr;
		instanceCount = 0;
		droppedInstances = 0;
	}

	void AccelStructure::AddInstance(uint32_t blasIndex, const XMFLOAT4X4& world)
	{
		if (blasIndex >= blas.size() || !instanceData)
			return;
		if (instanceCount >= maxInstances)
		{
			// 조용히 버리면 «RT 에만 안 보이는 물체» 가 생긴다. 숫자로 남긴다.
			++droppedInstances;
			return;
		}

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
		// Initialize 의 프리빌드 질의와 같은 플래그여야 한다(크기가 달라진다).
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		inputs.NumDescs = instanceCount;
		// 이번 프레임 슬롯의 주소를 넘긴다.
		inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress() +
			UINT64(frameSlot) * maxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

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
