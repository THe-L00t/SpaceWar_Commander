#pragma once
#include <d3d12.h>

// 렌더러 내부 전용 헤더. 게임 로직은 절대 include 하지 않는다.
namespace swc {

	inline D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
	{
		D3D12_HEAP_PROPERTIES p = {};
		p.Type = type;
		p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		p.CreationNodeMask = 1;
		p.VisibleNodeMask = 1;
		return p;
	}

	inline D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
	{
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = bytes;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		d.Flags = flags;
		return d;
	}

	inline D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		b.UAV.pResource = resource;
		return b;
	}
}
