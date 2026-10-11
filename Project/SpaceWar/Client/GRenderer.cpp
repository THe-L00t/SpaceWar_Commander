#include "GRenderer.h"
#include "DXCommon.h"
#include "ShaderCompiler.h"
#include "AccelStructure.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <cstring>
#include <cstddef>
#include <cmath>
#include <limits>
#include <array>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

	// 셰이더는 빌드 후 exe 옆 Shaders\ 로 복사된다. 작업 디렉터리와 무관하게 찾는다.
	std::wstring ShaderPath(const wchar_t* name)
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		std::wstring p(exePath);
		const size_t slash = p.find_last_of(L"\\/");
		p = (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
		return p + L"Shaders\\" + name;
	}

	// ── GPU 컬링용 프러스텀 (integrate-client2 의 client2 2f15728 판정을 옮김) ──
	//  CPU 는 평면 6개만 만들어 넘기고, 인스턴스별 검사는 Cull.hlsl 이 한다.
	struct FrustumPlane
	{
		double x = 0.0, y = 0.0, z = 0.0, d = 0.0;
	};

	struct Frustum
	{
		std::array<FrustumPlane, 6> planes{};
		bool valid = false;
	};

	Frustum MakeFrustum(const XMFLOAT4X4& m)
	{
		// 행벡터 view * projection, D3D 클립 범위: -w <= x,y <= w, 0 <= z <= w.
		// 행이 아니라 열을 조합한다. 카메라별로 한 프레임에 한 번만 계산한다.
		Frustum frustum;
		frustum.planes = {{
			{ double(m._14) + m._11, double(m._24) + m._21, double(m._34) + m._31, double(m._44) + m._41 },
			{ double(m._14) - m._11, double(m._24) - m._21, double(m._34) - m._31, double(m._44) - m._41 },
			{ double(m._14) + m._12, double(m._24) + m._22, double(m._34) + m._32, double(m._44) + m._42 },
			{ double(m._14) - m._12, double(m._24) - m._22, double(m._34) - m._32, double(m._44) - m._42 },
			{ m._13, m._23, m._33, m._43 },
			{ double(m._14) - m._13, double(m._24) - m._23, double(m._34) - m._33, double(m._44) - m._43 }
		}};
		for (FrustumPlane& plane : frustum.planes)
		{
			const double length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
			if (!std::isfinite(length) || length <= 0.0 || !std::isfinite(plane.d)) return frustum;
			plane.x /= length; plane.y /= length; plane.z /= length; plane.d /= length;
		}
		frustum.valid = true;
		return frustum;
	}

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		return barrier;
	}

	struct AdapterPick
	{
		ComPtr<IDXGIAdapter1> adapter;
		bool  raytracing = false;
		std::wstring name;
	};

	bool ProbeAdapter(IDXGIAdapter1* adapter, bool& outRaytracing)
	{
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			return false;

		ComPtr<ID3D12Device5> probe;
		if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe))))
			return false;

		// 인라인 RayQuery 는 DXR Tier 1.1 + SM 6.5 가 필요하다.
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
		const bool tierOk =
			SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
			options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;

		D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_5 };
		const bool smOk =
			SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) &&
			shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

		outRaytracing = tierOk && smOk;
		return true;
	}

	// 하이브리드 그래픽 노트북에서 내장 GPU 가 잡히면 DXR 이 없다.
	// 반드시 명시적으로 고른다 — 고성능 우선, DXR 되는 어댑터를 최우선.
	AdapterPick PickAdapter(IDXGIFactory4* factory)
	{
		AdapterPick pick;
		AdapterPick fallback;

		auto consider = [&](ComPtr<IDXGIAdapter1>& candidate) -> bool
		{
			bool rt = false;
			if (!ProbeAdapter(candidate.Get(), rt))
				return false;

			DXGI_ADAPTER_DESC1 desc = {};
			candidate->GetDesc1(&desc);

			if (rt)
			{
				pick.adapter = candidate;
				pick.raytracing = true;
				pick.name = desc.Description;
				return true;
			}
			if (!fallback.adapter)
			{
				fallback.adapter = candidate;
				fallback.name = desc.Description;
			}
			return false;
		};

		ComPtr<IDXGIFactory6> factory6;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6))))
		{
			ComPtr<IDXGIAdapter1> candidate;
			for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i,
				DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
				IID_PPV_ARGS(candidate.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				if (consider(candidate))
					return pick;
			}
		}

		ComPtr<IDXGIAdapter1> candidate;
		for (UINT i = 0; factory->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			if (consider(candidate))
				return pick;
		}

		return fallback;
	}
}

namespace swc {

	// 셰이더의 FrameCB 와 레이아웃이 반드시 일치해야 한다.
	struct FrameConstants
	{
		XMFLOAT4X4 viewProj;
		XMFLOAT3   eyePos;    float pad0;
		XMFLOAT3   sunDir;    float pad1;
		uint32_t   rtEnabled;
		float      rouletteKnee;
		float      fresnelBoost;
		uint32_t   debugMode;
		uint32_t   frameIndex;
		float      pad2[3];
	};

	// Cull.hlsl 의 CullCB 와 같은 순서. 루트 상수 26개.
	struct CullConstants
	{
		XMFLOAT4 planes[6];      // xyz = 법선, w = d (정규화)
		uint32_t entryCount;
		uint32_t cullEnabled;    // 0 이면 전부 보이는 것으로 둔다(프러스텀을 못 만들었을 때)
	};
	static_assert(sizeof(CullConstants) == 26 * sizeof(uint32_t));

	struct GRenderer::Impl
	{
		static const UINT FrameCount = 2;
		static const UINT MaxInstances = 4096;
		static const UINT FrameCBSize = 256;   // CBV 는 256바이트 정렬
		// 한 프레임에 그릴 수 있는 인스턴스·묶음 상한. 인스턴스 128 B × 8192 = 1 MiB / 프레임 슬롯.
		static const UINT MaxDrawInstances = 8192;
		static const UINT MaxBatches = 4096;
		static const UINT CullGroupSize = 64;   // Cull.hlsl 의 numthreads 와 같아야 한다

		struct MeshGpu
		{
			ComPtr<ID3D12Resource> vertexBuffer;
			ComPtr<ID3D12Resource> indexBuffer;
			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			D3D12_INDEX_BUFFER_VIEW ibv = {};
			UINT indexCount = 0;
			XMFLOAT3 boundsCenter{};    // 메시 로컬 AABB — GPU 컬링용
			XMFLOAT3 boundsExtents{};
			uint32_t blasIndex = AccelStructure::kInvalidBlas;
		};

		ComPtr<ID3D12Device5>              device;
		ComPtr<ID3D12CommandQueue>         commandQueue;
		ComPtr<IDXGISwapChain3>            swapChain;
		ComPtr<ID3D12DescriptorHeap>       rtvHeap;
		ComPtr<ID3D12DescriptorHeap>       dsvHeap;
		ComPtr<ID3D12Resource>             renderTargets[FrameCount];
		ComPtr<ID3D12Resource>             depthStencil;
		ComPtr<ID3D12CommandAllocator>     commandAllocator;
		ComPtr<ID3D12GraphicsCommandList4> commandList;
		ComPtr<ID3D12RootSignature>        rootSig;
		ComPtr<ID3D12PipelineState>        pso;
		ComPtr<ID3D12Resource>             frameCB;
		uint8_t*                           frameCBData = nullptr;

		// ── 인스턴싱 + GPU 컬링 (2026-10-11, integrate-client2 5b17e73 이식) ──
		//  수정방향 3.2 · 멀티스레딩 명세 §2 원칙 5.
		//  ① 패킷의 InstanceData(world·prevWorld)를 인스턴스 버퍼에 그대로 올린다(패킷 순서 = 인스턴스 번호).
		//  ② 그리기 묶음 = 같은 메시(이 브랜치는 PSO·재질이 하나다). 구성 버전이 바뀔 때만 다시 만든다.
		//  ③ 컴퓨트(Cull.hlsl)가 인스턴스마다 프러스텀 검사 → 묶음별 보이는 인스턴스 번호 목록 + 간접 인자.
		//  ④ 묶음마다 ExecuteIndirect 한 번. 그리기에는 «그 묶음 목록의 시작 번호» 하나만 루트 상수로 넘긴다.
		struct InstanceGpu                 // Forward.hlsl · Cull.hlsl 의 InstanceGpu (row_major)
		{
			XMFLOAT4X4 world;
			XMFLOAT4X4 prevWorld;
		};
		struct BatchBoundsGpu              // Cull.hlsl 의 BatchBounds — 메시 로컬 AABB + 목록 시작 번호
		{
			XMFLOAT3 center;  uint32_t first;
			XMFLOAT3 extents; uint32_t pad;
		};
		struct CullEntryGpu                // Cull.hlsl 의 gEntries (uint2)
		{
			uint32_t instance;
			uint32_t batch;
		};
		static_assert(sizeof(InstanceGpu) == 128 && sizeof(BatchBoundsGpu) == 32 && sizeof(CullEntryGpu) == 8);

		struct Batch
		{
			MeshHandle mesh = kInvalidMesh;
			uint32_t first = 0;                // 보이는 인스턴스 목록에서 이 묶음 구간의 시작
			uint32_t count = 0;                // 이 묶음에 들 수 있는 인스턴스 수(컬링 전)
		};

		// 프레임 슬롯마다 하나. CPU 가 쓰는 버퍼는 슬롯별이라 비행 중 프레임에서도 덮어쓰지 않는다(3.1).
		struct PassGpu
		{
			ComPtr<ID3D12Resource> instances;     // UPLOAD  InstanceGpu × MaxDrawInstances — 매 프레임
			ComPtr<ID3D12Resource> entries;       // UPLOAD  CullEntryGpu × MaxDrawInstances — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> bounds;        // UPLOAD  BatchBoundsGpu × MaxBatches — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> argsTemplate;  // UPLOAD  인스턴스 수 0 인 간접 인자 — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> args;          // DEFAULT UAV  D3D12_DRAW_INDEXED_ARGUMENTS × MaxBatches
			ComPtr<ID3D12Resource> visible;       // DEFAULT UAV  uint × MaxDrawInstances
			uint8_t* instanceData = nullptr;
			uint8_t* entryData = nullptr;
			uint8_t* boundsData = nullptr;
			uint8_t* templateData = nullptr;

			uint64_t layoutVersion = (std::numeric_limits<uint64_t>::max)();
			size_t   layoutInstances = 0;
			std::vector<Batch> batches;
			UINT     entryCount = 0;
			bool     statesInitialized = false;   // 첫 사용 전에는 생성 직후 상태다
		};
		PassGpu passes[FrameCount];
		std::vector<uint32_t> batchOrder;          // 묶음 재구성 때만 쓰는 작업 배열

		ComPtr<ID3D12RootSignature>        cullRootSig;
		ComPtr<ID3D12PipelineState>        cullPso;
		ComPtr<ID3D12CommandSignature>     drawSignature;   // DRAW_INDEXED 하나

		std::vector<MeshGpu> meshes;

		ShaderCompiler shaderCompiler;
		AccelStructure accel;

		bool rtSupported = false;
		RayTracingParams rtParams;
		// 정규화 필수 — GGX 의 H = normalize(L+V) 계산에 들어간다
		XMFLOAT3 sunDir{ 0.3563f, -0.8144f, 0.4581f };
		uint32_t debugMode = 0;
		uint32_t frameCounter = 0;
		std::wstring status;

		UINT rtvDescriptorSize = 0;
		UINT frameIndex = 0;

		ComPtr<ID3D12Fence> fence;
		UINT64 fenceValue = 0;
		HANDLE fenceEvent = nullptr;

		uint32_t width = 0;
		uint32_t height = 0;

		void WaitForGpu()
		{
			const UINT64 target = ++fenceValue;
			commandQueue->Signal(fence.Get(), target);
			if (fence->GetCompletedValue() < target)
			{
				fence->SetEventOnCompletion(target, fenceEvent);
				WaitForSingleObject(fenceEvent, INFINITE);
			}
			frameIndex = swapChain->GetCurrentBackBufferIndex();
		}

		// 초기화 중 커맨드를 한 번 기록·실행하고 완료까지 기다린다.
		void FlushCommands()
		{
			commandList->Close();
			ID3D12CommandList* lists[] = { commandList.Get() };
			commandQueue->ExecuteCommandLists(1, lists);
			WaitForGpu();
		}
	};

	GRenderer::GRenderer()
		: impl(std::make_unique<Impl>())
	{
	}

	GRenderer::~GRenderer()
	{
		if (impl->device && impl->swapChain && impl->commandQueue && impl->fence && impl->fenceEvent)
			impl->WaitForGpu();
		if (impl->fenceEvent)
			CloseHandle(impl->fenceEvent);
	}

	bool GRenderer::Initialize(HWND hwnd, uint32_t width, uint32_t height)
	{
		impl->width = width;
		impl->height = height;

		UINT factoryFlags = 0;
#if defined(_DEBUG)
		{
			ComPtr<ID3D12Debug> debug;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
			{
				debug->EnableDebugLayer();
				factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			}
		}
#endif

		ComPtr<IDXGIFactory4> factory;
		if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
			return false;

		AdapterPick pick = PickAdapter(factory.Get());
		if (!pick.adapter)
		{
			impl->status = L"D3D12 어댑터를 찾지 못했습니다.";
			return false;
		}
		if (FAILED(D3D12CreateDevice(pick.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&impl->device))))
		{
			impl->status = L"D3D12 디바이스 생성 실패.";
			return false;
		}
		impl->rtSupported = pick.raytracing;
		impl->status = pick.name + (pick.raytracing ? L"  [DXR Tier 1.1]" : L"  [RT 미지원 — 래스터만]");

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(impl->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&impl->commandQueue))))
			return false;

		DXGI_SWAP_CHAIN_DESC1 scDesc = {};
		scDesc.BufferCount = Impl::FrameCount;
		scDesc.Width = width;
		scDesc.Height = height;
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scDesc.SampleDesc.Count = 1;

		ComPtr<IDXGISwapChain1> swapChain1;
		if (FAILED(factory->CreateSwapChainForHwnd(impl->commandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1)))
			return false;
		factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
		swapChain1.As(&impl->swapChain);
		impl->frameIndex = impl->swapChain->GetCurrentBackBufferIndex();

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = Impl::FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		if (FAILED(impl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&impl->rtvHeap))))
			return false;
		impl->rtvDescriptorSize = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (UINT i = 0; i < Impl::FrameCount; ++i)
		{
			if (FAILED(impl->swapChain->GetBuffer(i, IID_PPV_ARGS(&impl->renderTargets[i]))))
				return false;
			impl->device->CreateRenderTargetView(impl->renderTargets[i].Get(), nullptr, rtvHandle);
			rtvHandle.ptr += impl->rtvDescriptorSize;
		}

		// Depth buffer + DSV
		{
			D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
			dsvHeapDesc.NumDescriptors = 1;
			dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			if (FAILED(impl->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&impl->dsvHeap))))
				return false;

			D3D12_RESOURCE_DESC depthDesc = {};
			depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			depthDesc.Width = width;
			depthDesc.Height = height;
			depthDesc.DepthOrArraySize = 1;
			depthDesc.MipLevels = 1;
			depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clearVal = {};
			clearVal.Format = DXGI_FORMAT_D32_FLOAT;
			clearVal.DepthStencil.Depth = 1.0f;

			D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&impl->depthStencil))))
				return false;
			impl->device->CreateDepthStencilView(impl->depthStencil.Get(), nullptr,
				impl->dsvHeap->GetCPUDescriptorHandleForHeapStart());
		}

		if (FAILED(impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl->commandAllocator))))
			return false;
		if (FAILED(impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->commandAllocator.Get(), nullptr, IID_PPV_ARGS(&impl->commandList))))
			return false;
		impl->commandList->Close();

		// 프레임 상수 버퍼 (UPLOAD, 영속 매핑)
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC desc = BufferDesc(Impl::FrameCBSize);
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->frameCB))))
				return false;
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->frameCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->frameCBData))))
				return false;
		}

		// 인스턴싱·GPU 컬링 자원 — 프레임 슬롯마다
		{
			auto createUpload = [&](UINT64 bytes, ComPtr<ID3D12Resource>& out, uint8_t*& mapped) -> bool
			{
				D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
				D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
				if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out))))
					return false;
				D3D12_RANGE noRead = { 0, 0 };
				return SUCCEEDED(out->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
			};
			auto createDefault = [&](UINT64 bytes, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) -> bool
			{
				D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
				D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				return SUCCEEDED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					state, nullptr, IID_PPV_ARGS(&out)));
			};
			const UINT64 argsBytes = UINT64(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)) * Impl::MaxBatches;
			for (Impl::PassGpu& pass : impl->passes)
			{
				if (!createUpload(UINT64(sizeof(Impl::InstanceGpu)) * Impl::MaxDrawInstances, pass.instances, pass.instanceData) ||
					!createUpload(UINT64(sizeof(Impl::CullEntryGpu)) * Impl::MaxDrawInstances, pass.entries, pass.entryData) ||
					!createUpload(UINT64(sizeof(Impl::BatchBoundsGpu)) * Impl::MaxBatches, pass.bounds, pass.boundsData) ||
					!createUpload(argsBytes, pass.argsTemplate, pass.templateData) ||
					!createDefault(argsBytes, D3D12_RESOURCE_STATE_COPY_DEST, pass.args) ||
					!createDefault(UINT64(sizeof(uint32_t)) * Impl::MaxDrawInstances, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pass.visible))
				{
					impl->status = L"인스턴스·컬링 버퍼 생성 실패.";
					return false;
				}
			}

			// ExecuteIndirect — 인자는 DRAW_INDEXED 하나뿐이라 루트 시그니처가 필요 없다.
			D3D12_INDIRECT_ARGUMENT_DESC argument = {};
			argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
			D3D12_COMMAND_SIGNATURE_DESC signature = {};
			signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
			signature.NumArgumentDescs = 1;
			signature.pArgumentDescs = &argument;
			if (FAILED(impl->device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&impl->drawSignature))))
			{
				impl->status = L"간접 그리기 시그니처 생성 실패.";
				return false;
			}
		}

		// Root signature
		//   b1 = 묶음의 보이는 목록 시작 번호(루트 상수 1개, 수정방향 3.2) / b0 = 프레임 CBV /
		//   t6 = 인스턴스 / t7 = 보이는 인스턴스 번호 목록 / t0 = TLAS (루트 SRV, 마지막)
		//   TLAS 를 루트 SRV 로 바인딩하면 셰이더 가시 디스크립터 힙이 필요 없다.
		//   (인스턴싱 전에는 b1 루트 상수 16개로 월드 행렬을 그리기마다 넘겼다.)
		{
			D3D12_ROOT_PARAMETER params[5] = {};

			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 1;
			params[0].Constants.RegisterSpace = 0;
			params[0].Constants.Num32BitValues = 1;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[1].Descriptor.ShaderRegister = 0;
			params[1].Descriptor.RegisterSpace = 0;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[2].Descriptor.ShaderRegister = 6;
			params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[3].Descriptor.ShaderRegister = 7;
			params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[4].Descriptor.ShaderRegister = 0;
			params[4].Descriptor.RegisterSpace = 0;
			params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = impl->rtSupported ? 5u : 4u;
			rsDesc.pParameters = params;
			rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
				return false;
			if (FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->rootSig))))
				return false;
		}

		// 셰이더 (DXC 런타임 컴파일) + PSO
		{
			if (!impl->shaderCompiler.Available())
			{
				impl->status = L"dxcompiler.dll 을 찾지 못했습니다 (exe 옆에 있어야 합니다).";
				return false;
			}

			const std::wstring path = ShaderPath(L"Forward.hlsl");
			const wchar_t* defineRt = impl->rtSupported ? L"RT_SUPPORTED=1" : L"RT_SUPPORTED=0";
			const wchar_t* vsTarget = impl->rtSupported ? L"vs_6_5" : L"vs_6_0";
			const wchar_t* psTarget = impl->rtSupported ? L"ps_6_5" : L"ps_6_0";

			std::string log;
			std::vector<uint8_t> vs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", vsTarget, &defineRt, 1, log);
			if (vs.empty())
			{
				OutputDebugStringA(("[VS] " + log + "\n").c_str());
				impl->status = L"버텍스 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			std::vector<uint8_t> ps = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", psTarget, &defineRt, 1, log);
			if (ps.empty())
			{
				OutputDebugStringA(("[PS] " + log + "\n").c_str());
				impl->status = L"픽셀 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}

			D3D12_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->rootSig.Get();
			psoDesc.InputLayout = { layout, 3 };
			psoDesc.VS = { vs.data(), vs.size() };
			psoDesc.PS = { ps.data(), ps.size() };
			psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			psoDesc.RasterizerState.DepthClipEnable = TRUE;
			psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			psoDesc.DepthStencilState.DepthEnable = TRUE;
			psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			psoDesc.SampleDesc.Count = 1;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->pso))))
			{
				impl->status = L"PSO 생성 실패.";
				return false;
			}
		}

		// ── GPU 컬링 파이프라인 (Cull.hlsl) ──
		//   b0 = CullCB(루트 상수 26개), t0 = 인스턴스, t1 = 컬링 항목, t2 = 묶음 경계,
		//   u0 = 간접 인자(raw), u1 = 보이는 인스턴스 번호 목록. 전부 루트 디스크립터라 힙이 필요 없다.
		{
			D3D12_ROOT_PARAMETER params[6] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 0;
			params[0].Constants.Num32BitValues = sizeof(CullConstants) / sizeof(uint32_t);
			for (UINT i = 0; i < 3; ++i)
			{
				params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[1 + i].Descriptor.ShaderRegister = i;
			}
			for (UINT i = 0; i < 2; ++i)
			{
				params[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
				params[4 + i].Descriptor.ShaderRegister = i;
			}
			for (D3D12_ROOT_PARAMETER& param : params)
				param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = 6;
			rsDesc.pParameters = params;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->cullRootSig))))
			{
				impl->status = L"컬링 루트 시그니처 생성 실패.";
				return false;
			}

			const std::wstring path = ShaderPath(L"Cull.hlsl");
			std::string log;
			std::vector<uint8_t> cs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"CSMain", L"cs_6_0", nullptr, 0, log);
			if (cs.empty())
			{
				OutputDebugStringA(("[Cull CS] " + log + "\n").c_str());
				impl->status = L"컬링 컴퓨트 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->cullRootSig.Get();
			psoDesc.CS = { cs.data(), cs.size() };
			if (FAILED(impl->device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&impl->cullPso))))
			{
				impl->status = L"컬링 PSO 생성 실패.";
				return false;
			}
		}

		if (FAILED(impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence))))
			return false;
		impl->fenceValue = 0;
		impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (impl->fenceEvent == nullptr)
			return false;

		if (impl->rtSupported && !impl->accel.Initialize(impl->device.Get(), Impl::MaxInstances))
		{
			impl->rtSupported = false;
			impl->status += L"  (가속 구조 초기화 실패 — RT 비활성)";
		}

		return true;
	}

	MeshHandle GRenderer::CreateMesh(const Vertex* verts, size_t vcount, const uint32_t* indices, size_t icount)
	{
		Impl::MeshGpu m;
		// GPU 컬링용 로컬 AABB — 정점을 버리기 전에 한 번만 구한다.
		if (vcount > 0)
		{
			XMFLOAT3 lo = verts[0].position, hi = verts[0].position;
			for (size_t i = 1; i < vcount; ++i)
			{
				const XMFLOAT3& p = verts[i].position;
				lo.x = (std::min)(lo.x, p.x); hi.x = (std::max)(hi.x, p.x);
				lo.y = (std::min)(lo.y, p.y); hi.y = (std::max)(hi.y, p.y);
				lo.z = (std::min)(lo.z, p.z); hi.z = (std::max)(hi.z, p.z);
			}
			m.boundsCenter = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
			m.boundsExtents = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f };
		}
		D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RANGE noRead = { 0, 0 };

		const UINT vbSize = UINT(vcount * sizeof(Vertex));
		D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbSize);
		impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.vertexBuffer));
		void* vp = nullptr;
		m.vertexBuffer->Map(0, &noRead, &vp);
		memcpy(vp, verts, vbSize);
		m.vertexBuffer->Unmap(0, nullptr);
		m.vbv.BufferLocation = m.vertexBuffer->GetGPUVirtualAddress();
		m.vbv.StrideInBytes = sizeof(Vertex);
		m.vbv.SizeInBytes = vbSize;

		const UINT ibSize = UINT(icount * sizeof(uint32_t));
		D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibSize);
		impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.indexBuffer));
		void* ip = nullptr;
		m.indexBuffer->Map(0, &noRead, &ip);
		memcpy(ip, indices, ibSize);
		m.indexBuffer->Unmap(0, nullptr);
		m.ibv.BufferLocation = m.indexBuffer->GetGPUVirtualAddress();
		m.ibv.Format = DXGI_FORMAT_R32_UINT;
		m.ibv.SizeInBytes = ibSize;

		m.indexCount = UINT(icount);

		// BLAS 는 메쉬가 만들어질 때 한 번만 빌드한다 (정적 지오메트리).
		if (impl->rtSupported)
		{
			D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
			geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geo.Triangles.VertexBuffer.StartAddress = m.vertexBuffer->GetGPUVirtualAddress();
			geo.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
			geo.Triangles.VertexCount = UINT(vcount);
			geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geo.Triangles.IndexBuffer = m.indexBuffer->GetGPUVirtualAddress();
			geo.Triangles.IndexCount = UINT(icount);
			geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

			impl->commandAllocator->Reset();
			impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);
			m.blasIndex = impl->accel.AddMesh(impl->device.Get(), impl->commandList.Get(), geo);
			impl->FlushCommands();

			// ── BLAS 압축 + scratch 해제 (2026-10-08) ──
			//  위 Flush 로 GPU 가 빌드를 끝냈다 → 압축 크기를 읽을 수 있다.
			if (m.blasIndex != AccelStructure::kInvalidBlas)
			{
				impl->commandAllocator->Reset();
				impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);

				impl->accel.CompactMesh(impl->device.Get(), impl->commandList.Get(), m.blasIndex);

				// 압축을 안 하더라도 커맨드 리스트를 닫아야 다음 호출이 Reset 할 수 있다.
				impl->FlushCommands();

				// 여기서 원본·scratch 가 실제로 해제된다(압축 복사가 GPU 에서 끝난 뒤).
				impl->accel.FinishCompaction(m.blasIndex);
			}
		}

		impl->meshes.push_back(std::move(m));
		return MeshHandle(impl->meshes.size() - 1);
	}

	void GRenderer::BeginFrame()
	{
		impl->commandAllocator->Reset();
		impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = impl->renderTargets[impl->frameIndex].Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		impl->commandList->ResourceBarrier(1, &barrier);

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += SIZE_T(impl->frameIndex) * impl->rtvDescriptorSize;
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
		impl->commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

		const float clearColor[4] = { 0.05f, 0.08f, 0.14f, 1.0f };
		impl->commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
		impl->commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(impl->width), float(impl->height), 0.0f, 1.0f };
		D3D12_RECT scissor = { 0, 0, LONG(impl->width), LONG(impl->height) };
		impl->commandList->RSSetViewports(1, &vp);
		impl->commandList->RSSetScissorRects(1, &scissor);
	}

	void GRenderer::Render(const FramePacket& packet)
	{
		const RenderView& view = packet.view;
		const std::vector<InstanceData>& items = packet.instances;
		const bool rtActive = impl->rtSupported && impl->rtParams.enabled;
		ID3D12GraphicsCommandList4* const commandList = impl->commandList.Get();

		// ── TLAS 재빌드 (패킷 인스턴스 → TLAS 인스턴스) ──
		if (rtActive)
		{
			// 프레임 슬롯을 넘긴다 — 비행 중 프레임(M1)에서 인스턴스 버퍼 덮어쓰기를 막는다.
			impl->accel.ResetInstances(impl->frameIndex);
			for (const InstanceData& it : items)
			{
				if (it.mesh >= impl->meshes.size()) continue;
				const uint32_t blasIndex = impl->meshes[it.mesh].blasIndex;
				if (blasIndex == AccelStructure::kInvalidBlas) continue;
				impl->accel.AddInstance(blasIndex, it.world);
			}
			impl->accel.BuildTlas(commandList);
		}

		// ── 프레임 상수 ──
		FrameConstants fc = {};
		XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.viewProj)));
		fc.eyePos = view.eyePosition;
		fc.sunDir = impl->sunDir;
		fc.rtEnabled = (rtActive && impl->accel.InstanceCount() > 0) ? 1u : 0u;
		fc.rouletteKnee = impl->rtParams.rouletteKnee;
		fc.fresnelBoost = impl->rtParams.fresnelBoost;
		fc.debugMode = impl->debugMode;
		fc.frameIndex = impl->frameCounter++;   // 룰렛 디더링을 프레임마다 흔든다
		memcpy(impl->frameCBData, &fc, sizeof(fc));

		Impl::PassGpu& pass = impl->passes[impl->frameIndex];

		// ── ① 그리기 묶음 — 패킷의 구성 버전이 바뀔 때만 다시 만든다 ──
		//  매 프레임 CPU 정렬은 하지 않는다(멀티스레딩 명세 §2 원칙 5). 묶음 = 같은 메시.
		if (pass.layoutVersion != packet.layoutVersion || pass.layoutInstances != items.size())
		{
			pass.layoutVersion = packet.layoutVersion;
			pass.layoutInstances = items.size();
			pass.batches.clear();
			pass.entryCount = 0;
			// 상한을 넘는 인스턴스는 그리지 않는다(TLAS 상한 4096 보다 넉넉하다).
			const size_t usable = (std::min)(items.size(), size_t(Impl::MaxDrawInstances));

			std::vector<uint32_t>& order = impl->batchOrder;
			order.clear();
			for (size_t i = 0; i < usable; ++i)
			{
				const InstanceData& it = items[i];
				if (it.mesh >= impl->meshes.size() || (it.flags & kInstanceDegenerate)) continue;
				order.push_back(uint32_t(i));
			}
			// stable_sort — 같은 묶음 안에서는 Extract 순서를 지킨다.
			std::stable_sort(order.begin(), order.end(),
				[&](uint32_t a, uint32_t b) { return items[a].mesh < items[b].mesh; });

			for (size_t k = 0; k < order.size(); ++k)
			{
				const MeshHandle mesh = items[order[k]].mesh;
				if (pass.batches.empty() || pass.batches.back().mesh != mesh)
				{
					if (pass.batches.size() >= Impl::MaxBatches) break;
					Impl::Batch batch;
					batch.mesh = mesh;
					batch.first = pass.entryCount;
					pass.batches.push_back(batch);
				}
				++pass.batches.back().count;
				const Impl::CullEntryGpu entry{ order[k], uint32_t(pass.batches.size() - 1) };
				memcpy(pass.entryData + size_t(pass.entryCount) * sizeof(entry), &entry, sizeof(entry));
				++pass.entryCount;
			}

			for (size_t b = 0; b < pass.batches.size(); ++b)
			{
				const Impl::Batch& batch = pass.batches[b];
				const Impl::MeshGpu& m = impl->meshes[batch.mesh];
				const Impl::BatchBoundsGpu bounds{ m.boundsCenter, batch.first, m.boundsExtents, 0 };
				memcpy(pass.boundsData + b * sizeof(bounds), &bounds, sizeof(bounds));
				// 인스턴스 수는 0 으로 둔다 — 컬링 셰이더가 보이는 만큼 올린다.
				const D3D12_DRAW_INDEXED_ARGUMENTS args{ m.indexCount, 0, 0, 0, 0 };
				memcpy(pass.templateData + b * sizeof(args), &args, sizeof(args));
			}
		}

		// ── ② 인스턴스 행렬 업로드 — 패킷 순서 그대로(인스턴스 번호 = 패킷 번호) ──
		static_assert(offsetof(InstanceData, world) == 0 && offsetof(InstanceData, prevWorld) == sizeof(XMFLOAT4X4),
			"InstanceData 의 world·prevWorld 는 InstanceGpu 와 같은 배치여야 한다");
		const size_t uploadCount = (std::min)(items.size(), size_t(Impl::MaxDrawInstances));
		for (size_t i = 0; i < uploadCount; ++i)
			memcpy(pass.instanceData + i * sizeof(Impl::InstanceGpu), &items[i], sizeof(Impl::InstanceGpu));

		const UINT batchCount = UINT(pass.batches.size());
		const UINT argsBytes = batchCount * UINT(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
		if (batchCount > 0)
		{
			// ── ③ GPU 컬링 — 간접 인자를 0 으로 되돌리고, 보이는 인스턴스만 묶음 목록에 담는다 ──
			if (pass.statesInitialized)
			{
				const D3D12_RESOURCE_BARRIER toWrite[2] = {
					Transition(pass.args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST),
					Transition(pass.visible.Get(),
						D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
				commandList->ResourceBarrier(2, toWrite);
			}
			commandList->CopyBufferRegion(pass.args.Get(), 0, pass.argsTemplate.Get(), 0, argsBytes);
			const D3D12_RESOURCE_BARRIER argsToUav = Transition(pass.args.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList->ResourceBarrier(1, &argsToUav);

			CullConstants cull = {};
			const Frustum frustum = MakeFrustum(view.viewProj);
			for (size_t i = 0; i < frustum.planes.size(); ++i)
			{
				const FrustumPlane& plane = frustum.planes[i];
				cull.planes[i] = XMFLOAT4(float(plane.x), float(plane.y), float(plane.z), float(plane.d));
			}
			cull.entryCount = pass.entryCount;
			cull.cullEnabled = frustum.valid ? 1u : 0u;

			commandList->SetComputeRootSignature(impl->cullRootSig.Get());
			commandList->SetPipelineState(impl->cullPso.Get());
			commandList->SetComputeRoot32BitConstants(0, sizeof(cull) / sizeof(uint32_t), &cull, 0);
			commandList->SetComputeRootShaderResourceView(1, pass.instances->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(2, pass.entries->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(3, pass.bounds->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(4, pass.args->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(5, pass.visible->GetGPUVirtualAddress());
			commandList->Dispatch((pass.entryCount + Impl::CullGroupSize - 1) / Impl::CullGroupSize, 1, 1);

			const D3D12_RESOURCE_BARRIER toRead[2] = {
				Transition(pass.args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
				Transition(pass.visible.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
			commandList->ResourceBarrier(2, toRead);
			pass.statesInitialized = true;
		}

		// ── 래스터 패스 ──
		commandList->SetGraphicsRootSignature(impl->rootSig.Get());
		commandList->SetPipelineState(impl->pso.Get());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->SetGraphicsRootConstantBufferView(1, impl->frameCB->GetGPUVirtualAddress());
		commandList->SetGraphicsRootShaderResourceView(2, pass.instances->GetGPUVirtualAddress());
		commandList->SetGraphicsRootShaderResourceView(3, pass.visible->GetGPUVirtualAddress());
		if (impl->rtSupported && impl->accel.InstanceCount() > 0)
			commandList->SetGraphicsRootShaderResourceView(4, impl->accel.TlasAddress());

		// ── ④ 묶음마다 ExecuteIndirect 한 번 — 인스턴스 수는 컬링 셰이더가 채웠다 ──
		for (UINT b = 0; b < batchCount; ++b)
		{
			const Impl::Batch& batch = pass.batches[b];
			const Impl::MeshGpu& m = impl->meshes[batch.mesh];
			commandList->IASetVertexBuffers(0, 1, &m.vbv);
			commandList->IASetIndexBuffer(&m.ibv);
			// 그리기에 넘기는 것은 «이 묶음의 보이는 목록 시작 번호» 하나다(수정방향 3.2).
			commandList->SetGraphicsRoot32BitConstant(0, batch.first, 0);
			commandList->ExecuteIndirect(impl->drawSignature.Get(), 1, pass.args.Get(),
				UINT64(b) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
		}
	}

	void GRenderer::EndFrame()
	{
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = impl->renderTargets[impl->frameIndex].Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		impl->commandList->ResourceBarrier(1, &barrier);

		impl->commandList->Close();

		ID3D12CommandList* lists[] = { impl->commandList.Get() };
		impl->commandQueue->ExecuteCommandLists(1, lists);

		impl->swapChain->Present(1, 0);
		impl->WaitForGpu();
	}

	bool GRenderer::SupportsRaytracing() const { return impl->rtSupported; }
	void GRenderer::SetRayTracingParams(const RayTracingParams& p) { impl->rtParams = p; }
	const RayTracingParams& GRenderer::GetRayTracingParams() const { return impl->rtParams; }
	void GRenderer::SetSunDirection(const XMFLOAT3& d) { impl->sunDir = d; }
	void GRenderer::SetDebugMode(uint32_t m) { impl->debugMode = m; }
	uint32_t GRenderer::DebugMode() const { return impl->debugMode; }
	const std::wstring& GRenderer::StatusText() const { return impl->status; }

	// 가속 구조 계측 — Impl 안의 AccelStructure 를 그대로 묻는다(DX 타입은 밖으로 안 나간다).
	uint64_t GRenderer::BlasResultBytes()     const { return impl->accel.BlasResultBytes(); }
	uint64_t GRenderer::BlasScratchBytes()    const { return impl->accel.BlasScratchBytes(); }
	uint64_t GRenderer::BlasCompactionSaved() const { return impl->accel.BlasCompactionSaved(); }
	size_t   GRenderer::BlasCount()           const { return impl->accel.BlasCount(); }
	uint32_t GRenderer::TlasInstanceCount()   const { return impl->accel.InstanceCount(); }
	uint32_t GRenderer::TlasMaxInstances()    const { return impl->accel.MaxInstances(); }
	uint32_t GRenderer::TlasDroppedInstances()const { return impl->accel.DroppedInstances(); }
}
