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
#include <algorithm>
#include <array>

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

	// 인스턴스별 프러스텀 검사는 GPU(Cull.hlsl)로 옮겼다(2026-10-11, 멀티스레딩 명세 §2 원칙 5).
	//  CPU 는 평면 6개만 만들어 넘긴다.

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

	// Forward.hlsl의 MaterialCB와 16바이트 단위로 맞춘다.
	struct MaterialConstants
	{
		XMFLOAT4 baseColor;
		XMFLOAT3 emissive; float roughness;
		float metallic; uint32_t hasNormalMap; float pad[2];
	};
	static_assert(sizeof(MaterialConstants) == 12 * sizeof(uint32_t));

	// Cull.hlsl 의 CullCB 와 같은 순서. 루트 상수 26개.
	struct CullConstants
	{
		XMFLOAT4 planes[6];      // xyz = 법선, w = d (정규화)
		uint32_t entryCount;
		uint32_t cullEnabled;    // 0 이면 전부 보이는 것으로 둔다(프러스텀을 못 만들었을 때)
	};
	static_assert(sizeof(CullConstants) == 26 * sizeof(uint32_t));

	// UI.hlsl 의 SpriteCB 와 순서가 같아야 한다. 루트 상수 16개(64바이트).
	struct SpriteConstants
	{
		XMFLOAT2 center;
		XMFLOAT2 halfSize;
		XMFLOAT2 uvMin;
		XMFLOAT2 uvMax;
		XMFLOAT4 tint;
		float    rotation;
		XMFLOAT2 invViewport;
		float    pad0;
	};
	static_assert(sizeof(SpriteConstants) == 16 * sizeof(uint32_t), "SpriteCB 는 루트 상수 16개");

	struct GRenderer::Impl
	{
		static const UINT FrameCount = 2;
		static const UINT MaxInstances = 4096;
		static const UINT FrameCBSize = 256;   // CBV 는 256바이트 정렬
		static const UINT MaxMaterials = 4096;
		// UI SRV 힙 크기. 텍스처 핸들 i 의 UI 용 SRV = 슬롯 i (CreateTexture 가 만든다).
		// 넘는 텍스처는 UI 로 그릴 수 없다(재질용 SRV 는 따로 materialHeap 에 있다).
		static const UINT MaxUiTextures = 4096;
		// 패스 하나에 그릴 수 있는 인스턴스·묶음 상한. 패스 자원은 (프레임 슬롯 × 패스)마다 하나다.
		//  인스턴스 128 B × 8192 = 1 MiB. TLAS 상한(MaxInstances 4096)보다 넉넉하다.
		static const UINT MaxDrawInstances = 8192;
		static const UINT MaxBatches = 4096;
		static const UINT PassCount = 2;        // 0 = 배경(하늘 행성), 1 = 메인
		static const UINT CullGroupSize = 64;   // Cull.hlsl 의 numthreads 와 같아야 한다

		struct MeshGpu
		{
			// 정점과 인덱스는 하나의 DEFAULT 버퍼를 나눠 써서 힙 정렬 낭비를 줄인다.
			ComPtr<ID3D12Resource> buffer;
			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			D3D12_INDEX_BUFFER_VIEW ibv = {};
			UINT indexCount = 0;
			XMFLOAT3 boundsCenter{};
			XMFLOAT3 boundsExtents{};
			uint32_t blasIndex = AccelStructure::kInvalidBlas;
		};

		struct TextureGpu
		{
			ComPtr<ID3D12Resource> resource;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		};

		struct MaterialGpu
		{
			bool doubleSided = false;
			MaterialConstants constants{};
			D3D12_GPU_DESCRIPTOR_HANDLE textures{};
		};

		ComPtr<ID3D12Device5>              device;
		ComPtr<ID3D12CommandQueue>         commandQueue;
		ComPtr<IDXGISwapChain3>            swapChain;
		ComPtr<ID3D12DescriptorHeap>       rtvHeap;
		ComPtr<ID3D12DescriptorHeap>       dsvHeap;
		ComPtr<ID3D12DescriptorHeap>       materialHeap;
		ComPtr<ID3D12Resource>             renderTargets[FrameCount];
		ComPtr<ID3D12Resource>             depthStencil;
		ComPtr<ID3D12CommandAllocator>     commandAllocator;
		ComPtr<ID3D12GraphicsCommandList4> commandList;
		ComPtr<ID3D12RootSignature>        rootSig;
		ComPtr<ID3D12PipelineState>        pso;
		ComPtr<ID3D12PipelineState>        doubleSidedPso;
		ComPtr<ID3D12PipelineState>        mirroredPso;
		ComPtr<ID3D12PipelineState>        mirroredDoubleSidedPso;
		ComPtr<ID3D12Resource>             frameCB;
		uint8_t*                           frameCBData = nullptr;

		// ── 인스턴싱 + GPU 컬링 (2026-10-11, 수정방향 3.2 · 멀티스레딩 명세 §2 원칙 5) ──
		//  ① 패킷의 InstanceData(world·prevWorld)를 인스턴스 버퍼에 그대로 올린다(패킷 순서 = 인스턴스 번호).
		//  ② 그리기 묶음 = 같은 (PSO, 재질, 메시). 패킷의 구성 버전이 바뀔 때만 다시 만든다(정렬도 그때만).
		//  ③ 컴퓨트(Cull.hlsl)가 인스턴스마다 프러스텀 검사 → 묶음별 보이는 인스턴스 번호 목록 + 간접 인자.
		//  ④ 묶음마다 ExecuteIndirect 한 번. 그리기에는 «그 묶음 목록의 시작 번호» 하나만 루트 상수로 넘긴다.
		//  CPU 는 컬링도 정렬도 매 프레임 하지 않는다.
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
			uint32_t pso = 0;                  // 0 기본 · 1 양면 · 2 반전 · 3 반전 양면
			MaterialHandle material = 0;
			MeshHandle mesh = kInvalidMesh;
			uint32_t first = 0;                // 보이는 인스턴스 목록에서 이 묶음 구간의 시작
			uint32_t count = 0;                // 이 묶음에 들 수 있는 인스턴스 수(컬링 전)
		};

		// (프레임 슬롯 × 패스)마다 하나. CPU 가 쓰는 버퍼는 슬롯별이라 비행 중 프레임에서도 덮어쓰지 않는다(3.1).
		struct PassGpu
		{
			ComPtr<ID3D12Resource> instances;     // UPLOAD  InstanceGpu × MaxDrawInstances — 매 프레임
			ComPtr<ID3D12Resource> entries;       // UPLOAD  CullEntryGpu × MaxDrawInstances — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> bounds;        // UPLOAD  BatchBoundsGpu × MaxBatches — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> argsTemplate;  // UPLOAD  인스턴스 수 0 인 간접 인자 — 구성이 바뀔 때만
			ComPtr<ID3D12Resource> args;          // DEFAULT UAV  D3D12_DRAW_INDEXED_ARGUMENTS × MaxBatches
			ComPtr<ID3D12Resource> visible;       // DEFAULT UAV  uint × MaxDrawInstances
			ComPtr<ID3D12Resource> readback;      // READBACK     args 사본 — 창 제목 통계용
			uint8_t* instanceData = nullptr;
			uint8_t* entryData = nullptr;
			uint8_t* boundsData = nullptr;
			uint8_t* templateData = nullptr;

			uint64_t layoutVersion = (std::numeric_limits<uint64_t>::max)();
			size_t   layoutInstances = 0;
			std::vector<Batch> batches;
			UINT     entryCount = 0;
			size_t   dropped = 0;                 // 상한을 넘어 묶음에 못 넣은 인스턴스 수
			bool     statesInitialized = false;   // 첫 사용 전에는 생성 직후 상태다
			bool     usedThisFrame = false;
		};
		PassGpu passes[FrameCount][PassCount];
		std::vector<uint32_t> batchOrder;          // 묶음 재구성 때만 쓰는 작업 배열

		ComPtr<ID3D12RootSignature>        cullRootSig;
		ComPtr<ID3D12PipelineState>        cullPso;
		ComPtr<ID3D12CommandSignature>     drawSignature;   // DRAW_INDEXED 하나

		std::vector<MeshGpu> meshes;
		std::vector<TextureGpu> textures;
		std::vector<MaterialGpu> materials;
		std::array<TextureHandle, kMaterialTextureCount> fallbackTextures{};
		UINT materialDescriptorSize = 0;
		bool frameOpen = false;

		// ── UI (로딩 화면 등) ──
		//  재질 SRV(materialHeap)는 재질마다 5칸씩 묶여 있어 텍스처 하나를 따로 가리킬 수 없다.
		//  그래서 UI 는 자기 SRV 힙을 쓴다. 텍스처 i 의 UI SRV = 힙 슬롯 i.
		ComPtr<ID3D12DescriptorHeap>       srvHeap;
		UINT                               srvDescriptorSize = 0;
		ComPtr<ID3D12RootSignature>        uiRootSig;
		ComPtr<ID3D12PipelineState>        uiPso;

		ShaderCompiler shaderCompiler;
		AccelStructure accel;

		bool rtSupported = false;
		RayTracingParams rtParams;
		// 정규화 필수 — GGX 의 H = normalize(L+V) 계산에 들어간다
		XMFLOAT3 sunDir{ 0.3563f, -0.8144f, 0.4581f };
		uint32_t debugMode = 0;
		uint32_t frameCounter = 0;
		std::wstring status;
		RenderStats stats;

		UINT rtvDescriptorSize = 0;
		UINT frameIndex = 0;

		ComPtr<ID3D12Fence> fence;
		UINT64 fenceValue = 0;
		HANDLE fenceEvent = nullptr;
		// 명령 제출 후 펜스 대기가 실패해도 GPU가 참조하는 리소스는 유지한다.
		ComPtr<ID3D12Resource> pendingUpload;
		ComPtr<ID3D12Resource> pendingDestination;

		uint32_t width = 0;
		uint32_t height = 0;

		bool WaitForGpu()
		{
			const UINT64 target = ++fenceValue;
			if (FAILED(commandQueue->Signal(fence.Get(), target)))
			{
				status = L"GPU 펜스 신호 전송 실패.";
				return false;
			}
			UINT64 completed = fence->GetCompletedValue();
			if (completed == (std::numeric_limits<UINT64>::max)())
			{
				status = L"GPU 디바이스가 제거되었습니다.";
				return false;
			}
			while (completed < target)
			{
				if (FAILED(fence->SetEventOnCompletion(target, fenceEvent)) ||
					WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0)
				{
					status = L"GPU 펜스 완료 대기 실패.";
					return false;
				}
				completed = fence->GetCompletedValue();
				if (completed == (std::numeric_limits<UINT64>::max)())
				{
					status = L"GPU 디바이스가 제거되었습니다.";
					return false;
				}
			}
			pendingUpload.Reset();
			pendingDestination.Reset();
			frameIndex = swapChain->GetCurrentBackBufferIndex();
			return true;
		}

		// 초기화 중 커맨드를 한 번 기록·실행하고 완료까지 기다린다.
		bool FlushCommands(const ComPtr<ID3D12Resource>& upload, const ComPtr<ID3D12Resource>& destination)
		{
			if (FAILED(commandList->Close()))
			{
				status = L"리소스 업로드 명령 기록 종료 실패.";
				return false;
			}
			pendingUpload = upload;
			pendingDestination = destination;
			ID3D12CommandList* lists[] = { commandList.Get() };
			commandQueue->ExecuteCommandLists(1, lists);
			return WaitForGpu();
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
		// kEnableRaytracing 이 false 면 장치가 지원해도 RT 경로를 전부 끈다.
		// 이 한 줄이 BLAS·TLAS 빌드, 셰이더의 RayQuery, TLAS 루트 SRV 를 모두 막는다.
		impl->rtSupported = pick.raytracing && kEnableRaytracing;

		impl->status = pick.name;
		if (impl->rtSupported)
			impl->status += L"  [DXR Tier 1.1]";
		else if (pick.raytracing)
			impl->status += L"  [DXR 지원 — 임시 비활성, 래스터만]";
		else
			impl->status += L"  [RT 미지원 — 래스터만]";

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
			D3D12_RESOURCE_DESC desc = BufferDesc(Impl::FrameCBSize * 2);
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->frameCB))))
				return false;
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->frameCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->frameCBData))))
				return false;
		}

		// 인스턴싱·GPU 컬링 패스 자원 — (프레임 슬롯 × 패스)마다
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
			for (auto& slot : impl->passes)
			{
				for (Impl::PassGpu& pass : slot)
				{
					D3D12_HEAP_PROPERTIES readbackHeap = HeapProps(D3D12_HEAP_TYPE_READBACK);
					D3D12_RESOURCE_DESC readbackDesc = BufferDesc(argsBytes);
					if (!createUpload(UINT64(sizeof(Impl::InstanceGpu)) * Impl::MaxDrawInstances, pass.instances, pass.instanceData) ||
						!createUpload(UINT64(sizeof(Impl::CullEntryGpu)) * Impl::MaxDrawInstances, pass.entries, pass.entryData) ||
						!createUpload(UINT64(sizeof(Impl::BatchBoundsGpu)) * Impl::MaxBatches, pass.bounds, pass.boundsData) ||
						!createUpload(argsBytes, pass.argsTemplate, pass.templateData) ||
						!createDefault(argsBytes, D3D12_RESOURCE_STATE_COPY_DEST, pass.args) ||
						!createDefault(UINT64(sizeof(uint32_t)) * Impl::MaxDrawInstances, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pass.visible) ||
						FAILED(impl->device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
							D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pass.readback))))
					{
						impl->status = L"인스턴스·컬링 버퍼 생성 실패.";
						return false;
					}
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

		// RT 초기화 결과를 확정한 다음 루트 시그니처와 셰이더 경로를 선택한다.
		if (impl->rtSupported && !impl->accel.Initialize(impl->device.Get(), Impl::MaxInstances))
		{
			impl->rtSupported = false;
			impl->status += L"  (가속 구조 초기화 실패 — RT 비활성)";
		}

		// Root signature
		//   b1 = 묶음의 보이는 목록 시작 번호(루트 상수 1개, 수정방향 3.2), b0 = 프레임, b2 = 재질,
		//   t1~t5 = 텍스처, t6 = 인스턴스, t7 = 보이는 인스턴스 번호 목록, t0 = TLAS.
		//   RT 포함 1 + 2 + 12 + 1 + 2 + 2 + 2 = 22 DWORD.
		//   (인스턴싱 전에는 b1 루트 상수 32개로 행렬을 그리기마다 넘겼다.)
		{
			D3D12_ROOT_PARAMETER params[7] = {};

			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 1;
			params[0].Constants.RegisterSpace = 0;
			params[0].Constants.Num32BitValues = 1;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[1].Descriptor.ShaderRegister = 0;
			params[1].Descriptor.RegisterSpace = 0;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[2].Constants.ShaderRegister = 2;
			params[2].Constants.Num32BitValues = 12;
			params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_DESCRIPTOR_RANGE textureRange = {};
			textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			textureRange.NumDescriptors = static_cast<UINT>(kMaterialTextureCount);
			textureRange.BaseShaderRegister = 1;
			textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
			params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[3].DescriptorTable.NumDescriptorRanges = 1;
			params[3].DescriptorTable.pDescriptorRanges = &textureRange;
			params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[4].Descriptor.ShaderRegister = 6;
			params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[5].Descriptor.ShaderRegister = 7;
			params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			// TLAS 는 마지막에 둔다 — RT 를 못 쓰면 파라미터 수만 줄여 뺀다.
			params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[6].Descriptor.ShaderRegister = 0;
			params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler = {};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.MaxAnisotropy = 1;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = impl->rtSupported ? 7u : 6u;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 1;
			rsDesc.pStaticSamplers = &sampler;
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
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, normal)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, color)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, uv)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, tangent)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->rootSig.Get();
			psoDesc.InputLayout = { layout, 5 };
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
			psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->doubleSidedPso))))
			{
				impl->status = L"양면 재질 PSO 생성 실패.";
				return false;
			}
			// 반전 배율은 와인딩만 뒤집는다. SV_IsFrontFace도 실제 앞면을 가리켜야 한다.
			psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->mirroredDoubleSidedPso))))
			{
				impl->status = L"반전 양면 재질 PSO 생성 실패.";
				return false;
			}
			psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->mirroredPso))))
			{
				impl->status = L"반전 메시 PSO 생성 실패.";
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

		// ── UI 파이프라인 (로딩 화면 등) ──
		//   b0 = 루트 상수 16개(SpriteCB) / t0 = 텍스처(디스크립터 테이블) / s0 = 정적 샘플러
		{
			D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
			srvHeapDesc.NumDescriptors = Impl::MaxUiTextures;
			srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(impl->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&impl->srvHeap))))
			{
				impl->status = L"UI 디스크립터 힙 생성 실패.";
				return false;
			}
			impl->srvDescriptorSize = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_DESCRIPTOR_RANGE srvRange = {};
			srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			srvRange.NumDescriptors = 1;
			srvRange.BaseShaderRegister = 0;
			srvRange.RegisterSpace = 0;
			srvRange.OffsetInDescriptorsFromTableStart = 0;

			D3D12_ROOT_PARAMETER params[2] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 0;
			params[0].Constants.RegisterSpace = 0;
			params[0].Constants.Num32BitValues = sizeof(SpriteConstants) / sizeof(uint32_t);
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable.NumDescriptorRanges = 1;
			params[1].DescriptorTable.pDescriptorRanges = &srvRange;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			// 밉까지 선형 보간 — 스피너를 원본보다 훨씬 작게 그려도 반짝이지 않게.
			D3D12_STATIC_SAMPLER_DESC sampler = {};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.RegisterSpace = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = 2;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 1;
			rsDesc.pStaticSamplers = &sampler;
			rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->uiRootSig))))
			{
				impl->status = L"UI 루트 시그니처 생성 실패.";
				return false;
			}

			const std::wstring path = ShaderPath(L"UI.hlsl");
			std::string log;
			std::vector<uint8_t> vs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", L"vs_6_0", nullptr, 0, log);
			if (vs.empty())
			{
				OutputDebugStringA(("[UI VS] " + log + "\n").c_str());
				impl->status = L"UI 버텍스 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			std::vector<uint8_t> ps = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", L"ps_6_0", nullptr, 0, log);
			if (ps.empty())
			{
				OutputDebugStringA(("[UI PS] " + log + "\n").c_str());
				impl->status = L"UI 픽셀 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->uiRootSig.Get();
			psoDesc.VS = { vs.data(), vs.size() };
			psoDesc.PS = { ps.data(), ps.size() };
			psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			psoDesc.RasterizerState.DepthClipEnable = TRUE;

			// premultiplied 알파 합성 — UI 이미지는 올리기 전에 알파를 미리 곱해 둔다(LoadingScene.cpp)
			D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_ONE;
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			// 깊이는 쓰지도 읽지도 않는다. 바인딩된 DSV 와 형식만 맞춘다.
			psoDesc.DepthStencilState.DepthEnable = FALSE;
			psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			psoDesc.SampleDesc.Count = 1;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->uiPso))))
			{
				impl->status = L"UI PSO 생성 실패.";
				return false;
			}
		}

		if (FAILED(impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence))))
			return false;
		impl->fenceValue = 0;
		impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (impl->fenceEvent == nullptr)
			return false;

		D3D12_DESCRIPTOR_HEAP_DESC materialHeapDesc = {};
		materialHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		materialHeapDesc.NumDescriptors = Impl::MaxMaterials * static_cast<UINT>(kMaterialTextureCount);
		materialHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(impl->device->CreateDescriptorHeap(&materialHeapDesc, IID_PPV_ARGS(&impl->materialHeap))))
		{
			impl->status = L"재질 텍스처 디스크립터 힙 생성 실패.";
			return false;
		}
		impl->materialDescriptorSize = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		TextureData white;
		white.width = white.height = 1;
		white.pixels = { 255, 255, 255, 255 };
		white.srgb = true;
		const TextureHandle whiteColor = CreateTexture(white);
		if (whiteColor == kInvalidTexture) return false;
		white.srgb = false;
		const TextureHandle whiteValue = CreateTexture(white);
		if (whiteValue == kInvalidTexture) return false;
		white.pixels = { 128, 128, 255, 255 };
		const TextureHandle flatNormal = CreateTexture(white);
		if (flatNormal == kInvalidTexture) return false;
		impl->fallbackTextures = { whiteColor, flatNormal, whiteValue, whiteValue, whiteColor };

		// 기존 Scene의 material 0은 정점 색만 쓰는 지형/더미 재질이다.
		MaterialData defaultMaterial;
		defaultMaterial.roughness = 0.10f;
		std::array<TextureHandle, kMaterialTextureCount> noTextures;
		noTextures.fill(kInvalidTexture);
		if (CreateMaterial(defaultMaterial, noTextures) == kInvalidMaterial) return false;

		return true;
	}

	MeshHandle GRenderer::CreateMesh(const Vertex* verts, size_t vcount, const uint32_t* indices, size_t icount)
	{
		if (!impl->device || !impl->fenceEvent || impl->frameOpen || !verts || !indices || vcount == 0 || icount == 0 ||
			icount % 3 != 0 || vcount > UINT_MAX / sizeof(Vertex) || icount > UINT_MAX / sizeof(uint32_t) ||
			impl->meshes.size() >= kInvalidMesh)
		{
			impl->status = L"메시 생성 인자 또는 호출 시점이 올바르지 않습니다.";
			return kInvalidMesh;
		}
		for (size_t i = 0; i < icount; ++i)
		{
			if (indices[i] >= vcount)
			{
				impl->status = L"메시 인덱스가 정점 범위를 벗어났습니다.";
				return kInvalidMesh;
			}
		}
		// CPU 정점을 해제하기 전에 경계만 남긴다. 프레임마다 정점을 읽지 않는다.
		//  Vertex 는 Shared::Vertex(위치 = Shared::Vec3)다 — XMFLOAT3 로 받지 않는다.
		XMFLOAT3 boundsMin{ verts[0].position.x, verts[0].position.y, verts[0].position.z };
		XMFLOAT3 boundsMax = boundsMin;
		for (size_t i = 0; i < vcount; ++i)
		{
			const Shared::Vec3& point = verts[i].position;
			if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
			{
				impl->status = L"메시 정점 좌표가 유효하지 않습니다.";
				return kInvalidMesh;
			}
			boundsMin.x = (std::min)(boundsMin.x, point.x); boundsMax.x = (std::max)(boundsMax.x, point.x);
			boundsMin.y = (std::min)(boundsMin.y, point.y); boundsMax.y = (std::max)(boundsMax.y, point.y);
			boundsMin.z = (std::min)(boundsMin.z, point.z); boundsMax.z = (std::max)(boundsMax.z, point.z);
		}
		Impl::MeshGpu m;
		m.boundsCenter = { boundsMin.x * 0.5f + boundsMax.x * 0.5f,
			boundsMin.y * 0.5f + boundsMax.y * 0.5f, boundsMin.z * 0.5f + boundsMax.z * 0.5f };
		m.boundsExtents = { boundsMax.x * 0.5f - boundsMin.x * 0.5f,
			boundsMax.y * 0.5f - boundsMin.y * 0.5f, boundsMax.z * 0.5f - boundsMin.z * 0.5f };
		const UINT vbSize = UINT(vcount * sizeof(Vertex));
		const UINT ibSize = UINT(icount * sizeof(uint32_t));
		const UINT64 indexOffset = (UINT64(vbSize) + sizeof(uint32_t) - 1) & ~(UINT64(sizeof(uint32_t)) - 1);
		const UINT64 bufferBytes = indexOffset + ibSize;
		if (bufferBytes > (std::numeric_limits<size_t>::max)())
		{
			impl->status = L"메시 버퍼 크기가 주소 범위를 벗어났습니다.";
			return kInvalidMesh;
		}
		D3D12_RESOURCE_DESC desc = BufferDesc(bufferBytes);
		D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.buffer))))
		{
			impl->status = L"GPU 메시 버퍼 생성 실패.";
			return kInvalidMesh;
		}
		m.vbv.BufferLocation = m.buffer->GetGPUVirtualAddress();
		m.vbv.StrideInBytes = sizeof(Vertex);
		m.vbv.SizeInBytes = vbSize;
		m.ibv.BufferLocation = m.buffer->GetGPUVirtualAddress() + indexOffset;
		m.ibv.Format = DXGI_FORMAT_R32_UINT;
		m.ibv.SizeInBytes = ibSize;
		m.indexCount = UINT(icount);

		// CPU 사본은 업로드 중에만 필요하며, 정적 메시를 UPLOAD 힙에 계속 두지 않는다.
		ComPtr<ID3D12Resource> upload;
		D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		if (FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
		{
			impl->status = L"메시 업로드 버퍼 생성 실패.";
			return kInvalidMesh;
		}
		uint8_t* mapped = nullptr;
		D3D12_RANGE noRead = { 0, 0 };
		if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
		{
			impl->status = L"메시 업로드 버퍼 매핑 실패.";
			return kInvalidMesh;
		}
		memcpy(mapped, verts, vbSize);
		memcpy(mapped + static_cast<size_t>(indexOffset), indices, ibSize);
		upload->Unmap(0, nullptr);

		if (!impl->WaitForGpu()) return kInvalidMesh;
		if (FAILED(impl->commandAllocator->Reset()) ||
			FAILED(impl->commandList->Reset(impl->commandAllocator.Get(), nullptr)))
		{
			impl->status = L"메시 업로드 명령 초기화 실패.";
			return kInvalidMesh;
		}
		impl->commandList->CopyBufferRegion(m.buffer.Get(), 0, upload.Get(), 0, bufferBytes);
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = m.buffer.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
		if (impl->rtSupported)
			barrier.Transition.StateAfter |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		impl->commandList->ResourceBarrier(1, &barrier);

		// BLAS 는 메쉬가 만들어질 때 한 번만 빌드한다 (정적 지오메트리).
		if (impl->rtSupported)
		{
			D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
			geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geo.Triangles.VertexBuffer.StartAddress = m.vbv.BufferLocation + offsetof(Vertex, position);
			geo.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
			geo.Triangles.VertexCount = UINT(vcount);
			geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geo.Triangles.IndexBuffer = m.ibv.BufferLocation;
			geo.Triangles.IndexCount = UINT(icount);
			geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

			m.blasIndex = impl->accel.AddMesh(impl->device.Get(), impl->commandList.Get(), geo);
			if (m.blasIndex == AccelStructure::kInvalidBlas)
			{
				impl->commandList->Close();
				impl->status = L"메시 가속 구조 생성 실패.";
				return kInvalidMesh;
			}
		}
		// 정점/인덱스 복사와 선택적 BLAS 빌드가 모두 끝난 뒤 임시 upload를 해제한다.
		if (!impl->FlushCommands(upload, m.buffer)) return kInvalidMesh;

		// ── BLAS 압축 + scratch 해제 (2026-10-08) ──
		//  위 Flush 로 GPU 가 빌드를 끝냈다 → 압축 크기를 읽을 수 있다.
		if (m.blasIndex != AccelStructure::kInvalidBlas)
		{
			impl->commandAllocator->Reset();
			impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);

			impl->accel.CompactMesh(impl->device.Get(), impl->commandList.Get(), m.blasIndex);

			// 압축을 안 하더라도 커맨드 리스트를 닫아야 다음 호출이 Reset 할 수 있다.
			if (!impl->FlushCommands(nullptr, nullptr)) return kInvalidMesh;

			// 여기서 원본·scratch 가 실제로 해제된다(압축 복사가 GPU 에서 끝난 뒤).
			impl->accel.FinishCompaction(m.blasIndex);
		}

		impl->meshes.push_back(std::move(m));
		return MeshHandle(impl->meshes.size() - 1);
	}

	TextureHandle GRenderer::CreateTexture(const TextureData& data)
	{
		const uint64_t rowBytes = uint64_t(data.width) * 4;
		const uint64_t imageBytes = rowBytes * data.height;
		if (!impl->device || !impl->fenceEvent || impl->frameOpen || data.width == 0 || data.height == 0 ||
			data.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || data.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			imageBytes != data.pixels.size() || impl->textures.size() >= kInvalidTexture)
		{
			impl->status = L"RGBA8 텍스처 데이터 또는 호출 시점이 올바르지 않습니다.";
			return kInvalidTexture;
		}

		Impl::TextureGpu texture;
		texture.format = data.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = data.width;
		desc.Height = data.height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = texture.format;
		desc.SampleDesc.Count = 1;
		D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource))))
		{
			impl->status = L"GPU 텍스처 생성 실패.";
			return kInvalidTexture;
		}

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		UINT rows = 0;
		UINT64 rowSize = 0, uploadBytes = 0;
		impl->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &uploadBytes);
		if (uploadBytes == 0 || uploadBytes > (std::numeric_limits<size_t>::max)() ||
			rows != data.height || rowSize != rowBytes)
		{
			impl->status = L"텍스처 업로드 배치 계산 실패.";
			return kInvalidTexture;
		}
		ComPtr<ID3D12Resource> upload;
		D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RESOURCE_DESC uploadDesc = BufferDesc(uploadBytes);
		if (FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
		{
			impl->status = L"텍스처 업로드 버퍼 생성 실패.";
			return kInvalidTexture;
		}
		uint8_t* mapped = nullptr;
		D3D12_RANGE noRead = { 0, 0 };
		if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
		{
			impl->status = L"텍스처 업로드 버퍼 매핑 실패.";
			return kInvalidTexture;
		}
		// GPU 행 간격은 256바이트 정렬이므로 원본 RGBA8 행을 따로 복사한다.
		for (UINT row = 0; row < rows; ++row)
			memcpy(mapped + static_cast<size_t>(footprint.Offset) + size_t(row) * footprint.Footprint.RowPitch,
				data.pixels.data() + size_t(row) * size_t(rowBytes), size_t(rowBytes));
		upload->Unmap(0, nullptr);

		if (!impl->WaitForGpu()) return kInvalidTexture;
		if (FAILED(impl->commandAllocator->Reset()) ||
			FAILED(impl->commandList->Reset(impl->commandAllocator.Get(), nullptr)))
		{
			impl->status = L"텍스처 업로드 명령 초기화 실패.";
			return kInvalidTexture;
		}
		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = upload.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION destination = {};
		destination.pResource = texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		impl->commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.resource.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		impl->commandList->ResourceBarrier(1, &barrier);
		// 복사가 끝날 때까지 upload를 유지한다. 디스크립터는 CreateMaterial에서 만든다.
		if (!impl->FlushCommands(upload, texture.resource)) return kInvalidTexture;

		// UI 로도 그릴 수 있게 UI SRV 힙의 «핸들 번호» 슬롯에 SRV 를 하나 더 만든다(DrawSprites).
		const UINT slot = UINT(impl->textures.size());
		if (impl->srvHeap && slot < Impl::MaxUiTextures)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.Format = texture.format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = impl->srvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += SIZE_T(slot) * impl->srvDescriptorSize;
			impl->device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
		}

		impl->textures.push_back(std::move(texture));
		return static_cast<TextureHandle>(impl->textures.size() - 1);
	}

	MaterialHandle GRenderer::CreateMaterial(const MaterialData& data,
		const std::array<TextureHandle, kMaterialTextureCount>& textures)
	{
		if (!impl->materialHeap || impl->frameOpen || impl->materials.size() >= Impl::MaxMaterials ||
			!std::isfinite(data.baseColor.x) || !std::isfinite(data.baseColor.y) ||
			!std::isfinite(data.baseColor.z) || !std::isfinite(data.baseColor.w) ||
			!std::isfinite(data.emissive.x) || !std::isfinite(data.emissive.y) || !std::isfinite(data.emissive.z) ||
			!std::isfinite(data.roughness) || !std::isfinite(data.metallic))
		{
			impl->status = L"재질 데이터, 등록 개수 또는 호출 시점이 올바르지 않습니다.";
			return kInvalidMaterial;
		}
		std::array<TextureHandle, kMaterialTextureCount> resolved;
		for (size_t slot = 0; slot < kMaterialTextureCount; ++slot)
		{
			resolved[slot] = textures[slot] == kInvalidTexture ? impl->fallbackTextures[slot] : textures[slot];
			if (resolved[slot] >= impl->textures.size())
			{
				impl->status = L"재질이 등록되지 않은 GPU 텍스처를 참조합니다.";
				return kInvalidMaterial;
			}
		}

		Impl::MaterialGpu material;
		material.doubleSided = data.doubleSided;
		material.constants.baseColor = data.baseColor;
		material.constants.emissive = data.emissive;
		material.constants.roughness = data.roughness;
		material.constants.metallic = data.metallic;
		material.constants.hasNormalMap = textures[static_cast<size_t>(TextureSlot::Normal)] != kInvalidTexture ? 1u : 0u;
		const size_t descriptorOffset = impl->materials.size() * kMaterialTextureCount * impl->materialDescriptorSize;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = impl->materialHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += descriptorOffset;
		material.textures = impl->materialHeap->GetGPUDescriptorHandleForHeapStart();
		material.textures.ptr += descriptorOffset;
		for (size_t slot = 0; slot < kMaterialTextureCount; ++slot)
		{
			const Impl::TextureGpu& texture = impl->textures[resolved[slot]];
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.Format = texture.format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			impl->device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
			cpu.ptr += impl->materialDescriptorSize;
		}
		impl->materials.push_back(material);
		return static_cast<MaterialHandle>(impl->materials.size() - 1);
	}

	void GRenderer::BeginFrame()
	{
		impl->frameOpen = true;
		impl->stats = RenderStats{};
		for (Impl::PassGpu& pass : impl->passes[impl->frameIndex])
			pass.usedThisFrame = false;
		++impl->frameCounter;   // 배경/메인 패스가 같은 프레임의 룰렛 패턴을 사용한다.
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
		impl->stats.submitted += items.size();
		if (impl->materials.empty()) return;
		const bool rtActive = !view.background && impl->rtSupported && impl->rtParams.enabled;
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
		fc.frameIndex = impl->frameCounter;
		// 한 커맨드 리스트에 두 패스를 기록하므로 서로 다른 256바이트 CBV를 사용한다.
		// 같은 CPU 메모리를 덮어쓰면 GPU가 배경에서도 마지막 메인 행렬을 읽는다.
		const UINT frameOffset = view.background ? 0 : Impl::FrameCBSize;
		memcpy(impl->frameCBData + frameOffset, &fc, sizeof(fc));

		Impl::PassGpu& pass = impl->passes[impl->frameIndex][view.background ? 0 : 1];
		pass.usedThisFrame = true;

		// ── ① 그리기 묶음 — 패킷의 구성 버전이 바뀔 때만 다시 만든다 ──
		//  정적 맵은 로드 직후 한 번만 정렬한다. 매 프레임 CPU 정렬은 하지 않는다(멀티스레딩 명세 §2 원칙 5).
		//  묶음 = 같은 (PSO, 재질, 메시). PSO 는 재질의 양면 여부와 인스턴스의 반전 여부로 정해진다.
		if (pass.layoutVersion != packet.layoutVersion || pass.layoutInstances != items.size())
		{
			pass.layoutVersion = packet.layoutVersion;
			pass.layoutInstances = items.size();
			pass.batches.clear();
			pass.entryCount = 0;
			const size_t usable = (std::min)(items.size(), size_t(Impl::MaxDrawInstances));
			pass.dropped = items.size() - usable;

			auto materialOf = [&](const InstanceData& it) -> MaterialHandle
			{
				return it.material < impl->materials.size() ? it.material : 0;
			};
			auto psoOf = [&](const InstanceData& it) -> uint32_t
			{
				return (impl->materials[materialOf(it)].doubleSided ? 1u : 0u) |
					((it.flags & kInstanceMirrored) ? 2u : 0u);
			};

			std::vector<uint32_t>& order = impl->batchOrder;
			order.clear();
			for (size_t i = 0; i < usable; ++i)
			{
				const InstanceData& it = items[i];
				if (it.mesh >= impl->meshes.size() || (it.flags & kInstanceDegenerate)) continue;
				order.push_back(uint32_t(i));
			}
			// stable_sort — 같은 묶음 안에서는 Extract 순서를 지킨다.
			std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
				{
					const InstanceData& x = items[a];
					const InstanceData& y = items[b];
					const uint32_t psoX = psoOf(x), psoY = psoOf(y);
					if (psoX != psoY) return psoX < psoY;
					const MaterialHandle materialX = materialOf(x), materialY = materialOf(y);
					if (materialX != materialY) return materialX < materialY;
					return x.mesh < y.mesh;
				});

			for (size_t k = 0; k < order.size(); ++k)
			{
				const InstanceData& it = items[order[k]];
				const uint32_t pso = psoOf(it);
				const MaterialHandle material = materialOf(it);
				if (pass.batches.empty() || pass.batches.back().pso != pso ||
					pass.batches.back().material != material || pass.batches.back().mesh != it.mesh)
				{
					if (pass.batches.size() >= Impl::MaxBatches)
					{
						pass.dropped += order.size() - k;
						break;
					}
					Impl::Batch batch;
					batch.pso = pso;
					batch.material = material;
					batch.mesh = it.mesh;
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
		impl->stats.dropped += pass.dropped;

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
					Transition(pass.args.Get(),
						D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE,
						D3D12_RESOURCE_STATE_COPY_DEST),
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
				Transition(pass.args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE),
				Transition(pass.visible.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
			commandList->ResourceBarrier(2, toRead);
			pass.statesInitialized = true;
			// 창 제목 통계용 — EndFrame 에서 GPU 완료를 기다린 뒤 읽는다.
			commandList->CopyBufferRegion(pass.readback.Get(), 0, pass.args.Get(), 0, argsBytes);
		}

		// ── 래스터 패스 ──
		commandList->SetGraphicsRootSignature(impl->rootSig.Get());
		commandList->SetPipelineState(impl->pso.Get());
		ID3D12DescriptorHeap* heaps[] = { impl->materialHeap.Get() };
		commandList->SetDescriptorHeaps(1, heaps);
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->SetGraphicsRootConstantBufferView(1, impl->frameCB->GetGPUVirtualAddress() + frameOffset);
		commandList->SetGraphicsRootShaderResourceView(4, pass.instances->GetGPUVirtualAddress());
		commandList->SetGraphicsRootShaderResourceView(5, pass.visible->GetGPUVirtualAddress());
		if (impl->rtSupported)
		{
			// DXR의 null acceleration structure는 GPU 주소 0이다. 루트 슬롯을 미설정으로 두지 않는다.
			const D3D12_GPU_VIRTUAL_ADDRESS tlas = rtActive && impl->accel.InstanceCount() > 0
				? impl->accel.TlasAddress() : 0;
			commandList->SetGraphicsRootShaderResourceView(6, tlas);
		}

		// ── ④ 묶음마다 ExecuteIndirect 한 번 — 인스턴스 수는 컬링 셰이더가 채웠다 ──
		ID3D12PipelineState* const psoByKey[4] = {
			impl->pso.Get(), impl->doubleSidedPso.Get(), impl->mirroredPso.Get(), impl->mirroredDoubleSidedPso.Get() };
		ID3D12PipelineState* currentPso = impl->pso.Get();
		MaterialHandle currentMaterial = kInvalidMaterial;
		for (UINT b = 0; b < batchCount; ++b)
		{
			const Impl::Batch& batch = pass.batches[b];
			ID3D12PipelineState* selectedPso = psoByKey[batch.pso];
			if (currentPso != selectedPso)
			{
				commandList->SetPipelineState(selectedPso);
				currentPso = selectedPso;
			}
			if (currentMaterial != batch.material)
			{
				const Impl::MaterialGpu& material = impl->materials[batch.material];
				commandList->SetGraphicsRoot32BitConstants(2, 12, &material.constants, 0);
				commandList->SetGraphicsRootDescriptorTable(3, material.textures);
				currentMaterial = batch.material;
			}
			const Impl::MeshGpu& m = impl->meshes[batch.mesh];
			commandList->IASetVertexBuffers(0, 1, &m.vbv);
			commandList->IASetIndexBuffer(&m.ibv);
			// 그리기에 넘기는 것은 «이 묶음의 보이는 목록 시작 번호» 하나다(수정방향 3.2).
			commandList->SetGraphicsRoot32BitConstant(0, batch.first, 0);
			commandList->ExecuteIndirect(impl->drawSignature.Get(), 1, pass.args.Get(),
				UINT64(b) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
			++impl->stats.drawCalls;
		}
		if (view.background)
		{
			// 먼 행성 내부의 깊이 비교는 유지하고 이후 맵/캐릭터가 항상 앞에 놓이게 한다.
			// 배경 패스의 넓은 near/far 범위를 메인 맵의 깊이 범위와 섞지 않는다.
			const D3D12_CPU_DESCRIPTOR_HANDLE dsv = impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
			impl->commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		}
	}

	void GRenderer::DrawSprites(const std::vector<UISprite>& sprites)
	{
		if (sprites.empty() || !impl->uiPso) return;

		impl->commandList->SetGraphicsRootSignature(impl->uiRootSig.Get());
		impl->commandList->SetPipelineState(impl->uiPso.Get());
		ID3D12DescriptorHeap* heaps[] = { impl->srvHeap.Get() };
		impl->commandList->SetDescriptorHeaps(1, heaps);
		impl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		const XMFLOAT2 invViewport(1.0f / float(impl->width), 1.0f / float(impl->height));
		const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = impl->srvHeap->GetGPUDescriptorHandleForHeapStart();

		for (const UISprite& s : sprites)
		{
			if (s.texture >= impl->textures.size() || s.texture >= Impl::MaxUiTextures) continue;

			SpriteConstants c = {};
			c.center = s.center;
			c.halfSize = s.halfSize;
			c.uvMin = s.uvMin;
			c.uvMax = s.uvMax;
			c.tint = s.tint;
			c.rotation = s.rotation;
			c.invViewport = invViewport;
			impl->commandList->SetGraphicsRoot32BitConstants(0, sizeof(c) / sizeof(uint32_t), &c, 0);

			D3D12_GPU_DESCRIPTOR_HANDLE gpu = heapStart;
			gpu.ptr += UINT64(s.texture) * impl->srvDescriptorSize;
			impl->commandList->SetGraphicsRootDescriptorTable(1, gpu);

			impl->commandList->DrawInstanced(4, 1, 0, 0);
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

		// WaitForGpu 가 frameIndex 를 다음 백버퍼로 바꾸므로 이번 프레임 슬롯을 먼저 잡아 둔다.
		const UINT slot = impl->frameIndex;
		impl->swapChain->Present(1, 0);
		const bool finished = impl->WaitForGpu();
		impl->frameOpen = false;

		// ── 통계 — GPU 컬링 결과(묶음별 인스턴스 수)를 읽는다. GPU 를 기다린 뒤라 이번 프레임 값이다 ──
		if (!finished) return;
		for (Impl::PassGpu& pass : impl->passes[slot])
		{
			if (!pass.usedThisFrame || pass.batches.empty() || !pass.statesInitialized) continue;
			const size_t bytes = pass.batches.size() * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
			D3D12_RANGE readRange = { 0, bytes };
			void* mapped = nullptr;
			if (FAILED(pass.readback->Map(0, &readRange, &mapped))) continue;
			const D3D12_DRAW_INDEXED_ARGUMENTS* args = static_cast<const D3D12_DRAW_INDEXED_ARGUMENTS*>(mapped);
			size_t visible = 0;
			for (size_t b = 0; b < pass.batches.size(); ++b)
			{
				visible += args[b].InstanceCount;
				impl->stats.triangles += uint64_t(args[b].IndexCountPerInstance / 3) * args[b].InstanceCount;
			}
			D3D12_RANGE noWrite = { 0, 0 };
			pass.readback->Unmap(0, &noWrite);
			impl->stats.instances += visible;
			impl->stats.culled += pass.entryCount > visible ? pass.entryCount - visible : 0;
		}
	}

	bool GRenderer::SupportsRaytracing() const { return impl->rtSupported; }
	void GRenderer::SetRayTracingParams(const RayTracingParams& p) { impl->rtParams = p; }
	const RayTracingParams& GRenderer::GetRayTracingParams() const { return impl->rtParams; }
	void GRenderer::SetSunDirection(const XMFLOAT3& d) { impl->sunDir = d; }
	void GRenderer::SetDebugMode(uint32_t m) { impl->debugMode = m; }
	uint32_t GRenderer::DebugMode() const { return impl->debugMode; }
	const RenderStats& GRenderer::Stats() const { return impl->stats; }
	const std::wstring& GRenderer::StatusText() const { return impl->status; }
	uint32_t GRenderer::Width() const { return impl->width; }
	uint32_t GRenderer::Height() const { return impl->height; }

	// 가속 구조 계측 — Impl 안의 AccelStructure 를 그대로 묻는다(DX 타입은 밖으로 안 나간다).
	uint64_t GRenderer::BlasResultBytes()     const { return impl->accel.BlasResultBytes(); }
	uint64_t GRenderer::BlasScratchBytes()    const { return impl->accel.BlasScratchBytes(); }
	uint64_t GRenderer::BlasCompactionSaved() const { return impl->accel.BlasCompactionSaved(); }
	size_t   GRenderer::BlasCount()           const { return impl->accel.BlasCount(); }
	uint32_t GRenderer::TlasInstanceCount()   const { return impl->accel.InstanceCount(); }
	uint32_t GRenderer::TlasMaxInstances()    const { return impl->accel.MaxInstances(); }
	uint32_t GRenderer::TlasDroppedInstances()const { return impl->accel.DroppedInstances(); }
}
