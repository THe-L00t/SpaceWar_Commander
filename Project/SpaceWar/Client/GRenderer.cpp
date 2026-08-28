#include "GRenderer.h"
#include "DXCommon.h"
#include "ShaderCompiler.h"
#include "AccelStructure.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <cstring>
#include <psapi.h>      // GetProcessMemoryInfo — 진단용

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

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

	struct GRenderer::Impl
	{
		static const UINT FrameCount = 2;
		static const UINT MaxInstances = 4096;
		static const UINT FrameCBSize = 256;   // CBV 는 256바이트 정렬

		struct MeshGpu
		{
			ComPtr<ID3D12Resource> vertexBuffer;
			ComPtr<ID3D12Resource> indexBuffer;
			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			D3D12_INDEX_BUFFER_VIEW ibv = {};
			UINT indexCount = 0;
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

		// ── 진단용 ────────────────────────────────────────────
		//  누수가 시스템 메모리인지 VRAM 인지 가리려면 둘을 따로 봐야 한다.
		ComPtr<IDXGIAdapter3> adapter3;      // QueryVideoMemoryInfo 용
		DiagInfo diag;

		// 끄면 디바이스가 죽어도 계속 렌더를 시도한다 = 수정 이전 동작 재현
		bool deviceLostGuard = true;

		void WaitForGpu()
		{
			const UINT64 target = ++fenceValue;

			// ★ Signal 실패는 디바이스가 이미 죽었다는 뜻이다
			if (FAILED(commandQueue->Signal(fence.Get(), target)))
			{
				CaptureDeviceRemoved(L"Signal 실패");
				return;
			}

			if (fence->GetCompletedValue() < target)
			{
				fence->SetEventOnCompletion(target, fenceEvent);

				// ★ INFINITE 를 쓰면 디바이스가 죽었을 때 영원히 멈춘다 (= 먹통)
				//   교수님이 겪은 "일정 시간 후 먹통" 이 정확히 이 자리다.
				const DWORD r = WaitForSingleObject(fenceEvent, 5000);
				if (r == WAIT_TIMEOUT)
				{
					CaptureDeviceRemoved(L"GPU 대기 5초 초과");
					return;
				}
			}
			frameIndex = swapChain->GetCurrentBackBufferIndex();
		}

		// 디바이스 제거 사유를 붙잡아 둔다. 이게 없으면 원인을 영영 모른다.
		void CaptureDeviceRemoved(const wchar_t* where)
		{
			if (diag.deviceRemoved) return;
			diag.deviceRemoved = true;

			const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
			const wchar_t* text = L"알 수 없음";
			switch (reason)
			{
			case DXGI_ERROR_DEVICE_HUNG:            text = L"DEVICE_HUNG (우리 명령이 GPU 를 멈춤)"; break;
			case DXGI_ERROR_DEVICE_REMOVED:         text = L"DEVICE_REMOVED"; break;
			case DXGI_ERROR_DEVICE_RESET:           text = L"DEVICE_RESET (TDR)"; break;
			case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  text = L"DRIVER_INTERNAL_ERROR"; break;
			case DXGI_ERROR_INVALID_CALL:           text = L"INVALID_CALL"; break;
			case E_OUTOFMEMORY:                     text = L"E_OUTOFMEMORY (메모리 고갈)"; break;
			case S_OK:                              text = L"S_OK (제거 아님)"; break;
			}
			wchar_t buf[256];
			swprintf_s(buf, L"[%s] 0x%08X %s", where, static_cast<unsigned>(reason), text);
			diag.removedReason = buf;
			OutputDebugStringW((std::wstring(L"*** 디바이스 제거: ") + buf + L"\n").c_str());

			DumpDred();
		}

		// DRED 자취를 출력창에 쏟아낸다. 어느 명령에서 죽었는지 여기서 드러난다.
		void DumpDred()
		{
#if defined(_DEBUG)
			ComPtr<ID3D12DeviceRemovedExtendedData> dred;
			if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return;

			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
			if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
			{
				OutputDebugStringW(L"*** DRED 자취 (마지막에 실행된 명령들) ***\n");
				for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode;
					n != nullptr; n = n->pNext)
				{
					// 완료 개수 < 전체 개수 = 이 커맨드리스트 중간에서 멈췄다는 뜻
					const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
					wchar_t line[512];
					swprintf_s(line, L"  [%s / %s] %u / %u 실행됨\n",
						n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"이름없음",
						n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"이름없음",
						done, n->BreadcrumbCount);
					OutputDebugStringW(line);

					if (done < n->BreadcrumbCount && n->pCommandHistory)
					{
						swprintf_s(line, L"    -> 멈춘 지점의 명령 코드: %u\n",
							static_cast<unsigned>(n->pCommandHistory[done]));
						OutputDebugStringW(line);
					}
				}
			}

			D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
			if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
			{
				wchar_t line[256];
				swprintf_s(line, L"*** DRED 페이지 폴트 주소: 0x%llX (이미 해제한 리소스를 GPU 가 읽었다는 뜻)\n",
					static_cast<unsigned long long>(fault.PageFaultVA));
				OutputDebugStringW(line);
			}
#endif
		}

		// 시스템 메모리(커밋) + GPU 메모리(로컬/논로컬)를 한 번에 읽는다
		void SampleMemory()
		{
			PROCESS_MEMORY_COUNTERS_EX pmc = {};
			pmc.cb = sizeof(pmc);
			if (GetProcessMemoryInfo(GetCurrentProcess(),
				reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
			{
				diag.privateBytes = pmc.PrivateUsage;
				diag.workingSet = pmc.WorkingSetSize;
			}

			if (adapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonLocal = {};
				adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
				adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
				diag.vramUsed = local.CurrentUsage;          // 전용 VRAM
				diag.sharedUsed = nonLocal.CurrentUsage;     // 공유 시스템 메모리
			}
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

		// ★ DRED — 디바이스가 왜 죽었는지 알아내는 유일한 방법
		//
		//   GetDeviceRemovedReason() 은 "DEVICE_HUNG" 같은 분류만 알려준다.
		//   그것만으로는 «우리 코드 어디가» 원인인지 알 수 없다.
		//   DRED 를 켜두면 GPU 가 마지막으로 실행한 명령의 자취(breadcrumb)와
		//   페이지 폴트 주소를 남겨준다. 반드시 디바이스 생성 «전에» 켜야 한다.
		{
			ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
			{
				dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
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

		// 진단용으로 어댑터를 붙잡아 둔다 (QueryVideoMemoryInfo 는 IDXGIAdapter3 부터)
		pick.adapter.As(&impl->adapter3);

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

		// Root signature
		//   b1 = 32비트 상수 16개 (월드 행렬) / b0 = 프레임 CBV / t0 = TLAS (루트 SRV)
		//   TLAS 를 루트 SRV 로 바인딩하면 셰이더 가시 디스크립터 힙이 필요 없다.
		{
			D3D12_ROOT_PARAMETER params[3] = {};

			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 1;
			params[0].Constants.RegisterSpace = 0;
			params[0].Constants.Num32BitValues = 16;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[1].Descriptor.ShaderRegister = 0;
			params[1].Descriptor.RegisterSpace = 0;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[2].Descriptor.ShaderRegister = 0;
			params[2].Descriptor.RegisterSpace = 0;
			params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = impl->rtSupported ? 3u : 2u;
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

	// ★ 정점·인덱스 버퍼는 DEFAULT 힙(= VRAM)에 둔다.
	//
	//   예전에는 UPLOAD 힙에 만들어 그대로 썼다. 동작은 하지만 UPLOAD 는
	//   write-combined "시스템 메모리" 다. 그러면 이렇게 된다:
	//     - 매 프레임 모든 드로우가 PCIe 너머로 지오메트리를 읽는다
	//     - BLAS 빌드도 시스템 메모리를 읽는다 (지형은 삼각형 52만 개다)
	//   게다가 UPLOAD 힙을 가속 구조 입력으로 쓰는 건 흔한 경로가 아니라서,
	//   드라이버가 내부적으로 어떻게 처리하는지가 구현·세대마다 갈린다.
	//   정석대로 DEFAULT 힙에 복사해 두면 그 변수가 사라지고 성능도 오른다.
	//
	//   스테이징 버퍼는 지역 ComPtr 이라 FlushCommands() 로 GPU 완료를 기다린 뒤
	//   함수를 빠져나가면서 해제된다. 복사가 끝나기 전에 사라지지 않는다.
	MeshHandle GRenderer::CreateMesh(const Vertex* verts, size_t vcount, const uint32_t* indices, size_t icount)
	{
		Impl::MeshGpu m;
		D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RANGE noRead = { 0, 0 };

		const UINT vbSize = UINT(vcount * sizeof(Vertex));
		const UINT ibSize = UINT(icount * sizeof(uint32_t));
		D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbSize);
		D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibSize);

		// 최종 버퍼 — VRAM
		if (FAILED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.vertexBuffer))) ||
			FAILED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.indexBuffer))))
		{
			return kInvalidMesh;
		}

		// 스테이징 — 시스템 메모리. 복사가 끝나면 버린다.
		ComPtr<ID3D12Resource> vbStaging, ibStaging;
		if (FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbStaging))) ||
			FAILED(impl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ibStaging))))
		{
			return kInvalidMesh;
		}

		void* p = nullptr;
		vbStaging->Map(0, &noRead, &p);  memcpy(p, verts, vbSize);   vbStaging->Unmap(0, nullptr);
		ibStaging->Map(0, &noRead, &p);  memcpy(p, indices, ibSize); ibStaging->Unmap(0, nullptr);

		impl->commandAllocator->Reset();
		impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);

		impl->commandList->CopyBufferRegion(m.vertexBuffer.Get(), 0, vbStaging.Get(), 0, vbSize);
		impl->commandList->CopyBufferRegion(m.indexBuffer.Get(), 0, ibStaging.Get(), 0, ibSize);

		// 읽기 상태들은 함께 걸어둘 수 있다. 한 번 걸어두면 이후 전이가 필요 없다.
		//   NON_PIXEL_SHADER_RESOURCE 는 BLAS 빌드가 지오메트리를 읽을 때 요구하는 상태다.
		const D3D12_RESOURCE_STATES readStates =
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
			D3D12_RESOURCE_STATE_INDEX_BUFFER |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		D3D12_RESOURCE_BARRIER toRead[2] = {};
		for (int i = 0; i < 2; ++i)
		{
			toRead[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toRead[i].Transition.pResource = (i == 0) ? m.vertexBuffer.Get() : m.indexBuffer.Get();
			toRead[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			toRead[i].Transition.StateAfter = readStates;
			toRead[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		}
		impl->commandList->ResourceBarrier(2, toRead);

		m.vbv.BufferLocation = m.vertexBuffer->GetGPUVirtualAddress();
		m.vbv.StrideInBytes = sizeof(Vertex);
		m.vbv.SizeInBytes = vbSize;

		m.ibv.BufferLocation = m.indexBuffer->GetGPUVirtualAddress();
		m.ibv.Format = DXGI_FORMAT_R32_UINT;
		m.ibv.SizeInBytes = ibSize;

		m.indexCount = UINT(icount);

		// BLAS 는 메쉬가 만들어질 때 한 번만 빌드한다 (정적 지오메트리).
		// 위 복사·전이와 같은 커맨드 리스트에 이어 기록하므로 순서가 보장된다.
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

			m.blasIndex = impl->accel.AddMesh(impl->device.Get(), impl->commandList.Get(), geo);
		}

		impl->FlushCommands();   // 실행 + GPU 완료 대기 → 스테이징을 안전하게 버릴 수 있다

		impl->meshes.push_back(std::move(m));
		return MeshHandle(impl->meshes.size() - 1);
	}

	void GRenderer::BeginFrame()
	{
		// ★ 디바이스가 죽은 뒤에는 아무것도 기록하지 않는다.
		//
		//   죽은 디바이스에 계속 밀어넣으면 이렇게 된다:
		//     - Present 가 즉시 실패해 v-sync 대기가 사라진다 -> 루프가 최대 속도로 돈다
		//     - 그런데 GPU 는 아무것도 완료하지 않으므로 WaitForGpu 도 무의미하다
		//     - 매 프레임 커맨드가 쌓이기만 하고 회수되지 않는다
		//   결과가 "화면은 새까만데 메모리만 폭주" 다. 여기서 끊어야 한다.
		if (impl->deviceLostGuard && impl->diag.deviceRemoved) return;

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
		if (impl->deviceLostGuard && impl->diag.deviceRemoved) return;

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
			++impl->diag.tlasBuilds;          // 진단: TLAS 재빌드 누적 횟수
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
		impl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		impl->commandList->SetGraphicsRootConstantBufferView(1, impl->frameCB->GetGPUVirtualAddress());
		if (impl->rtSupported && impl->accel.InstanceCount() > 0)
			impl->commandList->SetGraphicsRootShaderResourceView(2, impl->accel.TlasAddress());

		for (const RenderItem& it : items)
		{
			if (it.mesh >= impl->meshes.size()) continue;
			const Impl::MeshGpu& m = impl->meshes[it.mesh];

			XMFLOAT4X4 world;
			XMStoreFloat4x4(&world, XMMatrixTranspose(XMLoadFloat4x4(&worlds[it.node])));
			impl->commandList->SetGraphicsRoot32BitConstants(0, 16, &world, 0);

			impl->commandList->IASetVertexBuffers(0, 1, &m.vbv);
			impl->commandList->IASetIndexBuffer(&m.ibv);
			impl->commandList->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
		}
	}

	void GRenderer::EndFrame()
	{
		if (impl->deviceLostGuard && impl->diag.deviceRemoved) return;

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

		// ★ Present 반환값을 반드시 본다.
		//   여기를 버리면 디바이스가 죽어도 아무 흔적이 안 남는다.
		const HRESULT hr = impl->swapChain->Present(1, 0);
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			impl->CaptureDeviceRemoved(L"Present");
			return;                      // 죽은 디바이스에 더 밀어넣지 않는다
		}

		impl->WaitForGpu();
		impl->SampleMemory();            // 진단: 매 프레임 메모리 표본
	}

	const DiagInfo& GRenderer::Diagnostics() const { return impl->diag; }
	bool GRenderer::IsDeviceLost() const { return impl->diag.deviceRemoved; }

	// ── 재현 실험용 ──────────────────────────────────────────────
	void GRenderer::DebugForceDeviceRemoval()
	{
		if (!impl->device) return;
		OutputDebugStringW(L"*** [실험] RemoveDevice() 호출 — 디바이스를 강제로 제거한다\n");
		impl->device->RemoveDevice();
	}

	void GRenderer::SetDeviceLostGuard(bool on) { impl->deviceLostGuard = on; }
	bool GRenderer::DeviceLostGuard() const { return impl->deviceLostGuard; }

	bool GRenderer::SupportsRaytracing() const { return impl->rtSupported; }
	void GRenderer::SetRayTracingParams(const RayTracingParams& p) { impl->rtParams = p; }
	const RayTracingParams& GRenderer::GetRayTracingParams() const { return impl->rtParams; }
	void GRenderer::SetSunDirection(const XMFLOAT3& d) { impl->sunDir = d; }
	void GRenderer::SetDebugMode(uint32_t m) { impl->debugMode = m; }
	uint32_t GRenderer::DebugMode() const { return impl->debugMode; }
	const std::wstring& GRenderer::StatusText() const { return impl->status; }
}
