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

	struct ObjectConstants
	{
		XMFLOAT4X4 world;
		XMFLOAT4X4 normalWorld;
	};

	// Forward.hlsl의 MaterialCB와 16바이트 단위로 맞춘다.
	struct MaterialConstants
	{
		XMFLOAT4 baseColor;
		XMFLOAT3 emissive; float roughness;
		float metallic; uint32_t hasNormalMap; float pad[2];
	};
	static_assert(sizeof(ObjectConstants) == 32 * sizeof(uint32_t));
	static_assert(sizeof(MaterialConstants) == 12 * sizeof(uint32_t));

	struct GRenderer::Impl
	{
		static const UINT FrameCount = 2;
		static const UINT MaxInstances = 4096;
		static const UINT FrameCBSize = 256;   // CBV 는 256바이트 정렬
		static const UINT MaxMaterials = 4096;

		struct MeshGpu
		{
			ComPtr<ID3D12Resource> vertexBuffer;
			ComPtr<ID3D12Resource> indexBuffer;
			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			D3D12_INDEX_BUFFER_VIEW ibv = {};
			UINT indexCount = 0;
			uint32_t blasIndex = AccelStructure::kInvalidBlas;
		};

		struct TextureGpu
		{
			ComPtr<ID3D12Resource> resource;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		};

		struct MaterialGpu
		{
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
		ComPtr<ID3D12Resource>             frameCB;
		uint8_t*                           frameCBData = nullptr;

		std::vector<MeshGpu> meshes;
		std::vector<TextureGpu> textures;
		std::vector<MaterialGpu> materials;
		std::array<TextureHandle, kMaterialTextureCount> fallbackTextures{};
		UINT materialDescriptorSize = 0;
		bool frameOpen = false;

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

		bool WaitForGpu()
		{
			const UINT64 target = ++fenceValue;
			if (FAILED(commandQueue->Signal(fence.Get(), target)))
			{
				status = L"GPU 펜스 신호 전송 실패.";
				return false;
			}
			if (fence->GetCompletedValue() < target)
			{
				if (FAILED(fence->SetEventOnCompletion(target, fenceEvent)) ||
					WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0)
				{
					status = L"GPU 펜스 완료 대기 실패.";
					return false;
				}
			}
			frameIndex = swapChain->GetCurrentBackBufferIndex();
			return true;
		}

		// 초기화 중 커맨드를 한 번 기록·실행하고 완료까지 기다린다.
		bool FlushCommands()
		{
			if (FAILED(commandList->Close()))
			{
				status = L"리소스 업로드 명령 기록 종료 실패.";
				return false;
			}
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

		// RT 초기화 결과를 확정한 다음 루트 시그니처와 셰이더 경로를 선택한다.
		if (impl->rtSupported && !impl->accel.Initialize(impl->device.Get(), Impl::MaxInstances))
		{
			impl->rtSupported = false;
			impl->status += L"  (가속 구조 초기화 실패 — RT 비활성)";
		}

		// Root signature
		//   b1 = 월드/법선 행렬, b0 = 프레임, b2 = 재질, t1~t5 = 텍스처, t0 = TLAS.
		//   RT 포함 32 + 2 + 12 + 1 + 2 = 49 DWORD로 루트 상수 한도 안에 둔다.
		{
			D3D12_ROOT_PARAMETER params[5] = {};

			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 1;
			params[0].Constants.RegisterSpace = 0;
			params[0].Constants.Num32BitValues = 32;
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
			params[4].Descriptor.ShaderRegister = 0;
			params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
			rsDesc.NumParameters = impl->rtSupported ? 5u : 4u;
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
		if (!impl->device || impl->frameOpen || !verts || !indices || vcount == 0 || icount == 0 ||
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
		Impl::MeshGpu m;
		D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RANGE noRead = { 0, 0 };

		const UINT vbSize = UINT(vcount * sizeof(Vertex));
		D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbSize);
		if (FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.vertexBuffer))))
		{
			impl->status = L"메시 정점 버퍼 생성 실패.";
			return kInvalidMesh;
		}
		void* vp = nullptr;
		if (FAILED(m.vertexBuffer->Map(0, &noRead, &vp)))
		{
			impl->status = L"메시 정점 버퍼 매핑 실패.";
			return kInvalidMesh;
		}
		memcpy(vp, verts, vbSize);
		m.vertexBuffer->Unmap(0, nullptr);
		m.vbv.BufferLocation = m.vertexBuffer->GetGPUVirtualAddress();
		m.vbv.StrideInBytes = sizeof(Vertex);
		m.vbv.SizeInBytes = vbSize;

		const UINT ibSize = UINT(icount * sizeof(uint32_t));
		D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibSize);
		if (FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m.indexBuffer))))
		{
			impl->status = L"메시 인덱스 버퍼 생성 실패.";
			return kInvalidMesh;
		}
		void* ip = nullptr;
		if (FAILED(m.indexBuffer->Map(0, &noRead, &ip)))
		{
			impl->status = L"메시 인덱스 버퍼 매핑 실패.";
			return kInvalidMesh;
		}
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
			geo.Triangles.VertexBuffer.StartAddress = m.vertexBuffer->GetGPUVirtualAddress() + offsetof(Vertex, position);
			geo.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
			geo.Triangles.VertexCount = UINT(vcount);
			geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geo.Triangles.IndexBuffer = m.indexBuffer->GetGPUVirtualAddress();
			geo.Triangles.IndexCount = UINT(icount);
			geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

			if (!impl->WaitForGpu()) return kInvalidMesh;
			if (FAILED(impl->commandAllocator->Reset()) ||
				FAILED(impl->commandList->Reset(impl->commandAllocator.Get(), nullptr)))
			{
				impl->status = L"메시 가속 구조 명령 초기화 실패.";
				return kInvalidMesh;
			}
			m.blasIndex = impl->accel.AddMesh(impl->device.Get(), impl->commandList.Get(), geo);
			if (m.blasIndex == AccelStructure::kInvalidBlas)
			{
				impl->commandList->Close();
				impl->status = L"메시 가속 구조 생성 실패.";
				return kInvalidMesh;
			}
			if (!impl->FlushCommands()) return kInvalidMesh;
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
		if (!impl->FlushCommands()) return kInvalidTexture;
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

	void GRenderer::Render(const RenderView& view, const std::vector<RenderItem>& items, const XMFLOAT4X4* worlds)
	{
		if (!worlds || impl->materials.empty()) return;
		const bool rtActive = impl->rtSupported && impl->rtParams.enabled;

		// ── TLAS 재빌드 (씬 노드 → 인스턴스) ──
		if (rtActive)
		{
			impl->accel.ResetInstances();
			for (const RenderItem& it : items)
			{
				if (it.mesh >= impl->meshes.size()) continue;
				const uint32_t blasIndex = impl->meshes[it.mesh].blasIndex;
				if (blasIndex == AccelStructure::kInvalidBlas) continue;
				impl->accel.AddInstance(blasIndex, worlds[it.node]);
			}
			impl->accel.BuildTlas(impl->commandList.Get());
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

		// ── 래스터 패스 ──
		impl->commandList->SetGraphicsRootSignature(impl->rootSig.Get());
		impl->commandList->SetPipelineState(impl->pso.Get());
		ID3D12DescriptorHeap* heaps[] = { impl->materialHeap.Get() };
		impl->commandList->SetDescriptorHeaps(1, heaps);
		impl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		impl->commandList->SetGraphicsRootConstantBufferView(1, impl->frameCB->GetGPUVirtualAddress());
		if (impl->rtSupported)
			impl->commandList->SetGraphicsRootShaderResourceView(4, impl->accel.TlasAddress());

		for (const RenderItem& it : items)
		{
			if (it.mesh >= impl->meshes.size()) continue;
			const Impl::MeshGpu& m = impl->meshes[it.mesh];

			const XMMATRIX world = XMLoadFloat4x4(&worlds[it.node]);
			XMVECTOR determinant;
			const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, world);
			const float det = XMVectorGetX(determinant);
			if (!std::isfinite(det) || std::fabs(det) < 1e-12f) continue;
			ObjectConstants object;
			XMStoreFloat4x4(&object.world, XMMatrixTranspose(world));
			// 법선 행렬은 inverse-transpose. 셰이더 업로드 전치까지 적용하면 inverse가 남는다.
			XMStoreFloat4x4(&object.normalWorld, inverseWorld);
			impl->commandList->SetGraphicsRoot32BitConstants(0, 32, &object, 0);

			const MaterialHandle materialIndex = it.material < impl->materials.size() ? it.material : 0;
			const Impl::MaterialGpu& material = impl->materials[materialIndex];
			impl->commandList->SetGraphicsRoot32BitConstants(2, 12, &material.constants, 0);
			impl->commandList->SetGraphicsRootDescriptorTable(3, material.textures);

			impl->commandList->IASetVertexBuffers(0, 1, &m.vbv);
			impl->commandList->IASetIndexBuffer(&m.ibv);
			impl->commandList->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
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
		impl->frameOpen = false;
	}

	bool GRenderer::SupportsRaytracing() const { return impl->rtSupported; }
	void GRenderer::SetRayTracingParams(const RayTracingParams& p) { impl->rtParams = p; }
	const RayTracingParams& GRenderer::GetRayTracingParams() const { return impl->rtParams; }
	void GRenderer::SetSunDirection(const XMFLOAT3& d) { impl->sunDir = d; }
	void GRenderer::SetDebugMode(uint32_t m) { impl->debugMode = m; }
	uint32_t GRenderer::DebugMode() const { return impl->debugMode; }
	const std::wstring& GRenderer::StatusText() const { return impl->status; }
}
