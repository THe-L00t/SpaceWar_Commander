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
#include <initializer_list>
#include <utility>

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

	// ── [6] 하늘 — 태양 직접광 색 (2026-10-27 렌더 6단계) ──
	//  Shaders/Sky.hlsl 의 SunTransmittance 와 같은 식 · 같은 대기 값(Hillaire 지구 기본값, 1/m).
	//  셰이더는 하늘 텍스처를 만들 때 쓰고, 여기(CPU)는 «땅에 닿는 태양 조도» 를 한 번 구해 모든 패스의 상수로 넘긴다.
	constexpr double kAtmBottomRadius = 6360.0e3;
	constexpr double kAtmTopRadius = 6460.0e3;
	constexpr double kAtmRayleigh[3] = { 5.802e-6, 13.558e-6, 33.1e-6 };
	constexpr double kAtmRayleighScaleHeight = 8.0e3;
	constexpr double kAtmMieExtinction = 4.440e-6;
	constexpr double kAtmMieScaleHeight = 1.2e3;
	constexpr double kAtmOzone[3] = { 0.650e-6, 1.881e-6, 0.085e-6 };

	// 관측 높이(m)에서 «위와 cos muS» 인 해 쪽으로 대기 끝까지의 투과율(RGB). 해가 지평선(대기 속 지면)에 막히면 0.
	XMFLOAT3 SunTransmittance(double muS, double observerHeight)
	{
		const double r = kAtmBottomRadius + observerHeight;
		if (muS < 0.0)
		{
			const double disc = r * r * muS * muS - (r - kAtmBottomRadius) * (r + kAtmBottomRadius);
			if (disc >= 0.0 && -r * muS - std::sqrt(disc) >= 0.0)
				return XMFLOAT3(0.0f, 0.0f, 0.0f);
		}
		const double tTop = -r * muS + std::sqrt((std::max)(0.0, (kAtmTopRadius - r) * (kAtmTopRadius + r) + r * r * muS * muS));
		constexpr int kSteps = 64;
		const double dt = tTop / kSteps;
		double depth[3] = { 0.0, 0.0, 0.0 };
		for (int i = 0; i < kSteps; ++i)
		{
			const double t = (i + 0.5) * dt;
			const double h = std::sqrt(r * r + t * t + 2.0 * r * muS * t) - kAtmBottomRadius;
			const double rayleigh = std::exp(-h / kAtmRayleighScaleHeight);
			const double mie = std::exp(-h / kAtmMieScaleHeight);
			const double km = h * 1.0e-3;
			const double ozone = std::clamp(km < 25.0 ? km / 15.0 - 2.0 / 3.0 : -km / 15.0 + 8.0 / 3.0, 0.0, 1.0);
			for (int c = 0; c < 3; ++c)
				depth[c] += (kAtmRayleigh[c] * rayleigh + kAtmMieExtinction * mie + kAtmOzone[c] * ozone) * dt;
		}
		return XMFLOAT3(float(std::exp(-depth[0])), float(std::exp(-depth[1])), float(std::exp(-depth[2])));
	}
}

namespace swc {

	// 셰이더(Forward.hlsl)의 FrameCB 와 레이아웃이 반드시 일치해야 한다.
	//  ★ 2026-10-27 2단계 — 옛 인라인 반사용 rtEnabled · rouletteKnee · fresnelBoost · frameIndex 를 뺐다(96바이트).
	//  ★ 2026-10-27 5단계 — prevViewProj(모션 벡터용) 추가 → 160바이트. 패스마다 256바이트 칸이라 그대로 들어간다.
	struct FrameConstants
	{
		XMFLOAT4X4 viewProj;
		XMFLOAT3   eyePos;    float pad0;
		XMFLOAT3   sunDir;    uint32_t debugMode;
		XMFLOAT4X4 prevViewProj;   // 이전 프레임 viewProj(전치해서 올림). 첫 프레임 · 배경 패스는 viewProj 와 같다
		XMFLOAT3   sunIlluminance; float pad1;   // 6단계 — 땅에 닿는 태양 조도(RGB, 대기 투과 포함). 배경 패스가 쓴다
	};
	static_assert(sizeof(FrameConstants) == 176, "FrameCB 는 176바이트");

	// Lighting.hlsl 의 LightingCB 와 같은 순서 · 크기 (2026-10-27 파이프라인 1단계, [5] 라이팅 합성)
	struct LightingConstants
	{
		XMFLOAT4X4 invViewProj;   // viewProj 의 역행렬. FrameConstants::viewProj 처럼 전치해서 올린다
		XMFLOAT3   eyePos;    float pad0;
		XMFLOAT3   sunDir;    float pad1;
		uint32_t   width;
		uint32_t   height;
		uint32_t   debugMode;
		uint32_t   shadowEnabled;       // 2026-10-27 2단계 — 1 이면 그림자 마스크를 읽는다(이번 프레임에 [4a] 가 돌았다)
		uint32_t   reflectionEnabled;   // 2026-10-27 4단계 — 1 이면 반사 텍스처를 읽는다(이번 프레임에 [4b] 가 돌았다)
		uint32_t   aoEnabled;           // 2026-10-27 8단계 — 1 이면 AO 텍스처(t13)를 읽는다(이번 프레임에 [2] 가 돌았다)
		uint32_t   pad3[2];
		XMFLOAT3   sunIlluminance;      // 2026-10-27 6단계 — 땅에 닿는 태양 조도(RGB)
		uint32_t   skyEnabled;          // 2026-10-27 6단계 — 1 이면 하늘 조도 텍스처(t9)를 주변광으로 읽는다
		struct ProbeVolumeGpu
		{
			// Shaders/Probes.hlsli 의 ProbeVolume 과 같은 배치 (128 B, 10-28 두 겹 64 → 80, 지면에 맞춘 축 → 128) — 라이팅 · 반사 · 반투명 · GI 상수에 그대로 들어간다
			int32_t  baseCell[3];  uint32_t generation;    // 격자 맨 앞 칸의 월드 칸 번호 · 세대
			int32_t  dims[3];      float    spacing;       // 축별 프로브 수 · 간격(m)
			float    normalBias;   float    viewBias;      // 샘플 위치 띄우기(간격 배수)
			uint32_t probesPerRow; uint32_t enabled;       // 아틀라스 한 줄 프로브 수 · 1 = 이번 프레임에 쓸 수 있다
			float    irradianceAtlasSize[2];  float distanceAtlasSize[2];   // 두 겹이 같이 쓰는 아틀라스 전체 크기
			uint32_t storageOffset; uint32_t padV[3];      // 2026-10-28 — 이 겹 프로브의 아틀라스 · 상태 버퍼 시작 번호
			float    axisX[4], axisY[4], axisZ[4];         // 2026-10-28 — 격자 축(월드 단위 벡터). axisY = 짧은 축 = 지면 위쪽
		} probeVolume;                  // 2026-10-27 7단계 — [4c] GI 프로브 가까운 겹 (enabled 0 이면 못 씀)
		ProbeVolumeGpu probeVolumeFar;  // 2026-10-28 — 먼 겹 (가까운 겹에서 못 쓰면 이것, 둘 다 못 쓰면 하늘 조도)
	};
	using ProbeVolumeGpu = LightingConstants::ProbeVolumeGpu;
	static_assert(sizeof(ProbeVolumeGpu) == 128, "ProbeVolume 은 128바이트 (10-28 64 → 80 → 128)");
	static_assert(sizeof(LightingConstants) == 400, "LightingCB 는 400바이트 (10-28 GI 두 겹 208 → 304, 축 → 400)");

	// ── RT 표 (2026-10-27 렌더 4단계) — Shaders/RtCommon.hlsli 와 같은 배치 ──
	//  재질 표: 재질 번호(TLAS InstanceID)로 찾는다. 텍스처 번호는 SM 6.6 바인드리스 힙 번호.
	struct RtMaterialGpu
	{
		XMFLOAT4 baseColor;
		XMFLOAT3 emissive;      float    roughness;
		float    metallic;      uint32_t hasNormalMap;   uint32_t doubleSided;   uint32_t terrain;   // 7단계 — 양면(GI 레이가 뒷면을 맞아도 «벽 속» 아님)
		//  ★ terrain(2026-10-28, 예전 pad0) = 지형 레이어 재질. 이때 칸 몇 개를 다른 뜻으로 쓴다(CreateTerrainMaterial):
		//    baseColor = 레이어별 높이 계수 · roughness = 경계 폭 · metallic = 노멀 세기 · textures = 배열 4개 · emissiveTexture = 마스크
		//    · emissive.xy = 4단계 색 흔들림 세기 · 바탕
		uint32_t textures[4];   // BaseColor · Normal · Roughness · Metallic
		uint32_t emissiveTexture; uint32_t pad1[3];
		XMFLOAT3 transmittance; uint32_t transmissive;
		// 9단계 — [7] 반투명 패스가 읽는 투과 · 부피 · 굴절률 (MaterialData 그대로)
		float    transmission;  float ior;  float thickness;  float attenuationDistance;
		XMFLOAT3 attenuationColor; float alphaCutoff;   // 2026-10-28 — 알파 마스크 기준(0 = 마스크 아님), 예전 pad2 자리
	};
	static_assert(sizeof(RtMaterialGpu) == 128, "RtMaterial 은 128바이트 (9단계에서 96 → 128)");
	//  인스턴스 표: TLAS 인스턴스 순번(InstanceIndex)으로 찾는다. 프레임마다 다시 채운다.
	struct RtInstanceGpu
	{
		uint32_t vertexBuffer;   // 바인드리스 힙 번호 — 정점(raw)
		uint32_t indexBuffer;    // 바인드리스 힙 번호 — 인덱스(raw)
		uint32_t material;
		uint32_t pad;
	};
	static_assert(sizeof(RtInstanceGpu) == 16, "RtInstance 는 16바이트");

	// RtReflection.hlsl 의 ReflectionCB 와 같은 순서 · 크기 (2026-10-27 렌더 4단계, [4b] RT 반사)
	struct ReflectionConstants
	{
		XMFLOAT4X4 invViewProj;
		XMFLOAT3   eyePos;    float maxDistance;
		XMFLOAT3   sunDir;    float maxRoughness;
		uint32_t   width, height;
		uint32_t   normalIndex, materialIndex;   // 바인드리스 힙 번호
		uint32_t   depthIndex, outputIndex;
		uint32_t   frameIndex;   // 5단계 — B 방식 난수 씨앗(프레임마다 다른 방향)
		uint32_t   skyIndex;     // 6단계 — [6] 하늘 텍스처(바인드리스) — 미스 색
		XMFLOAT3   sunIlluminance;            // 6단계 — 태양 조도(RGB) — 맞은 점 조명
		uint32_t   skyIrradianceIndex;        // 6단계 — 하늘 조도 텍스처(바인드리스) — 맞은 점 주변광
		ProbeVolumeGpu probeVolume;           // 7단계 — [4c] GI 프로브 격자 — 맞은 점 주변광
		uint32_t   probeIrradianceIndex, probeDistanceIndex, probeStateIndex;   // 7단계 — 바인드리스 번호
		uint32_t   tileListIndex;   // 11단계 — [3] 분류 타일 목록 SRV (0xFFFFFFFF 면 화면 전체 DispatchRays)
		ProbeVolumeGpu probeVolumeFar;        // 2026-10-28 — GI 먼 겹
	};
	static_assert(sizeof(ReflectionConstants) == 416, "ReflectionCB 는 416바이트 (10-28 GI 두 겹 224 → 320, 축 → 416)");

	// ReflectionClassify.hlsl 의 ClassifyCB 와 같은 순서 · 크기 (2026-10-27 렌더 11단계, [3] 분류)
	struct ClassifyConstants
	{
		uint32_t width, height; float maxRoughness; uint32_t depthIndex;   // ↓ 바인드리스 번호
		uint32_t materialIndex, reflectionUav, tileListUav, argsUav;
		uint32_t argsWidthOffset;   // offsetof(D3D12_DISPATCH_RAYS_DESC, Width) — 분류가 여기에 64 씩 더한다
		uint32_t pad[3];
	};
	static_assert(sizeof(ClassifyConstants) == 48, "ClassifyCB 는 48바이트");

	// ProbeGI.hlsl 의 GiCB 와 같은 순서 · 크기 (2026-10-27 렌더 7단계, [4c] GI 프로브) — 세 진입점이 같은 상수를 쓴다
	//  2026-10-28 두 겹 — 겹마다 이 상수를 한 벌씩(512 B 칸) 채워 세 진입점을 겹마다 돌린다. layer = 이번 겹.
	struct GiConstants
	{
		ProbeVolumeGpu nearVolume;           // 128 — 가까운 겹
		ProbeVolumeGpu farVolume;            // 128 — 먼 겹
		XMFLOAT4   rayRotation[3];           // 이번 프레임 레이 회전(행 3개)
		XMFLOAT3   sunDir;           uint32_t firstProbe;      // 이번 프레임에 갱신하는 첫 번호(겹 안)
		XMFLOAT3   sunIlluminance;   uint32_t probeCount;      // 이번 겹의 프로브 수
		uint32_t   rayDataUav, rayDataSrv, irradianceUav, irradianceSrv;   // ↓ 바인드리스 번호
		uint32_t   distanceUav, distanceSrv, stateUav, stateSrv;
		uint32_t   skySrv, skyIrradianceSrv, probesThisFrame;  float maxDistance;
		uint32_t   probeCycle;       uint32_t layer;   uint32_t pad0[2];   // 이번 겹을 다 돈 횟수(꺼 둔 프로브 다시 잴 바퀴) · 이번 겹
	};
	static_assert(sizeof(GiConstants) == 400, "GiCB 는 400바이트 (10-28 바퀴 번호 192 → 208, 두 겹 → 304, 축 → 400)");

	// AO.hlsl 의 AoCB 와 같은 순서 · 크기 (2026-10-27 렌더 8단계, [2] AO) — 세 진입점이 같은 상수를 쓴다
	struct AoConstants
	{
		XMFLOAT4X4 viewProj;          // 월드 → 클립 (화면 공간 추적, 전치해서 올림)
		XMFLOAT4X4 invViewProj;
		XMFLOAT4X4 prevInvViewProj;
		XMFLOAT3   eyePos;     float radius;       // AO 반경(m)
		uint32_t   width, height, frameIndex, rtEnabled;
		uint32_t   depthIndex, normalIndex, motionIndex, prevDepthIndex;   // ↓ 바인드리스 번호
		uint32_t   prevNormalIndex, rawUav, rawSrv, historyInSrv;
		uint32_t   historyOutUav, historyOutSrv, outputUav, historyValid;
		float      thickness;  uint32_t pad0[3];   // 화면 공간 추적에서 «깊이 표면 바로 뒤» 로 볼 두께(m)
	};
	static_assert(sizeof(AoConstants) == 288, "AoCB 는 288바이트");

	// Transparent.hlsl 의 TransparentCB 와 같은 순서 · 크기 (2026-10-27 렌더 9단계, [7] 반투명)
	struct TransparentConstants
	{
		XMFLOAT4X4 viewProj;               // 전치해서 올림
		XMFLOAT4X4 invViewProj;
		XMFLOAT3   eyePos;          float maxDistance;
		XMFLOAT3   sunDir;          uint32_t sceneColorSrv;   // ↓ 바인드리스 번호
		XMFLOAT3   sunIlluminance;  uint32_t depthSrv;
		uint32_t   width, height, mipCount, rtEnabled;
		uint32_t   skySrv, skyIrradianceSrv, probeIrradianceSrv, probeDistanceSrv;
		uint32_t   probeStateSrv, pad0[3];
		ProbeVolumeGpu probeVolume;        // GI 가까운 겹
		ProbeVolumeGpu probeVolumeFar;     // 2026-10-28 — GI 먼 겹
	};
	static_assert(sizeof(TransparentConstants) == 480, "TransparentCB 는 480바이트 (10-28 GI 두 겹 288 → 384, 축 → 480)");

	// ColorPyramid.hlsl 의 PyramidCB (2026-10-27 렌더 9단계) — 디스패치마다 256 B 칸 하나
	struct PyramidConstants
	{
		uint32_t srcIndex, dstIndex;
		uint32_t dstWidth, dstHeight;
		float    srcTexelX, srcTexelY;
		uint32_t pad0[2];
	};
	static_assert(sizeof(PyramidConstants) == 32, "PyramidCB 는 32바이트");

	// PostProcess.hlsl 의 PostCB (2026-10-27 렌더 10단계, [8] 후처리) — 디스패치마다 256 B 칸 하나
	struct PostConstants
	{
		uint32_t srcIndex, dstIndex, addIndex, first;        // ↓ 바인드리스 번호 · 첫 내리기(Karis)
		uint32_t dstWidth, dstHeight;  float srcTexelX, srcTexelY;
		uint32_t histogramIndex, exposureUav;  float deltaTime;  uint32_t pixelCount;
		float    logMin, logMax, lowPercent, highPercent;     // 자동 노출 (UE 기본값)
		float    minBrightness, maxBrightness, speedUp, speedDown;
		uint32_t hdrWidth, hdrHeight, frameIndex, pad0;
	};
	static_assert(sizeof(PostConstants) == 96, "PostCB 는 96바이트");

	// Tonemap.hlsl PSTonemap 의 TonemapCB — 루트 상수 8개 (2026-10-27 렌더 10단계)
	struct TonemapConstants
	{
		uint32_t hdrIndex, bloomIndex, exposureIndex;
		float    bloomStrength;
		uint32_t bloomEnabled;
		float    bloomScale;      // 1 / 블룸 단 수 — 올리기 0단은 모든 단의 합이라 평균으로 맞춘다
		uint32_t pad0[2];
	};
	static_assert(sizeof(TonemapConstants) == 32, "TonemapCB 는 루트 상수 8개");

	// Sky.hlsl 의 SkyCB 와 같은 순서 · 크기 (2026-10-27 렌더 6단계, [6] 하늘) — 세 진입점이 같은 상수를 쓴다
	struct SkyConstants
	{
		XMFLOAT4X4 invViewProj;            // 픽셀 방향 되살리기 (전치해서 올림)
		XMFLOAT3   eyePos;           float sunIlluminanceTop;   // 대기 위 태양 조도 (π)
		XMFLOAT3   toSun;            float cosSunRadius;        // 해 쪽 단위 방향 = −sunDir, 해 원반 반지름의 cos
		XMFLOAT3   up;               float observerHeight;      // 카메라 자리의 위, 대기 계산 속 관측 높이(m)
		XMFLOAT3   sunDiskRadiance;  uint32_t hdrUav;           // 해 원반 휘도 · HDR UAV(바인드리스)
		uint32_t   width, height, skyWidth, skyHeight;
		uint32_t   irradianceWidth, irradianceHeight, skyUav, skySrv;
		uint32_t   irradianceUav;
		uint32_t   msUav, msSrv, msSize;          // 다중 산란 LUT (정사각, 바인드리스)
	};
	static_assert(sizeof(SkyConstants) == 176, "SkyCB 는 176바이트");

	// RtReflectionDenoise.hlsl 의 DenoiseCB 와 같은 순서 · 크기 (2026-10-27 렌더 5단계, [4d] 임시 노이즈 제거)
	//  번호는 전부 바인드리스 힙 번호. 시간 누적 · 공간 필터 두 패스가 같은 상수를 쓴다.
	struct DenoiseConstants
	{
		XMFLOAT4X4 invViewProj;       // 이번 프레임 (전치해서 올림)
		XMFLOAT4X4 prevInvViewProj;   // 이전 프레임 — 이전 깊이에서 이전 위치를 되살린다
		XMFLOAT3   eyePos;    float maxRoughness;
		uint32_t   width, height;
		uint32_t   historyValid;      // 0 = 이력 없이 새로 시작(첫 프레임 · 지난 프레임에 반사가 안 돌았음)
		uint32_t   pad0;
		uint32_t   rawIndex, motionIndex, depthIndex, normalIndex;
		uint32_t   materialIndex, prevDepthIndex, prevNormalIndex, historyInIndex;
		uint32_t   historyOutUav, historyOutSrv, outputUav, pad1;
	};
	static_assert(sizeof(DenoiseConstants) == 208, "DenoiseCB 는 208바이트");

	// RtShadow.hlsl 의 ShadowCB 와 같은 순서 · 크기 (2026-10-27 렌더 2단계, [4a] RT 그림자)
	//  2026-10-28 — 알파 마스크 판정에 재질 텍스처가 필요해 그림자도 바인드리스(컴퓨트 공용 루트)로 옮겼다:
	//  법선 · 깊이 · 그림자 마스크 UAV 를 바인드리스 번호로 넘긴다(112 → 128바이트).
	struct ShadowConstants
	{
		XMFLOAT4X4 invViewProj;   // 전치해서 올린다(LightingConstants 와 같은 값)
		XMFLOAT3   eyePos;    float maxDistance;   // 그림자 레이 길이 상한(m)
		XMFLOAT3   sunDir;    float pad0;
		uint32_t   width;
		uint32_t   height;
		uint32_t   normalIndex;   // 바인드리스 — G-buffer 법선 SRV
		uint32_t   depthIndex;    // 바인드리스 — 깊이 SRV
		uint32_t   shadowUav;     // 바인드리스 — 그림자 마스크 UAV
		uint32_t   pad1[3];
	};
	static_assert(sizeof(ShadowConstants) == 128, "ShadowCB 는 128바이트");

	// Forward.hlsl의 MaterialCB와 16바이트 단위로 맞춘다.
	struct MaterialConstants
	{
		XMFLOAT4 baseColor;
		XMFLOAT3 emissive; float roughness;
		float metallic; uint32_t hasNormalMap; float alphaCutoff; float pad;   // 2026-10-28 — 알파 마스크 기준(0 = 마스크 아님)
	};
	static_assert(sizeof(MaterialConstants) == 12 * sizeof(uint32_t));

	// 지형 레이어 재질의 루트 상수 (2026-10-28) — Forward.hlsl TERRAIN_LAYERS 판의 TerrainCB 와 같은 배치.
	//  MaterialConstants 와 같은 자리(루트 파라미터 2)를 다른 뜻으로 쓴다. 4단계 값 5개가 늘어 12 → 20 DWORD —
	//  루트 파라미터 2 를 20 DWORD 로 넓혔다(kMaterialRootConstants). 일반 재질은 앞 12 DWORD 만 채운다.
	struct TerrainConstants
	{
		XMFLOAT4 heightBlend;                     // 레이어(마스크 R G B A)마다 Height 에 곱하는 값
		float    triplanarTileA;                  // 3단계 — 세 방향 투영 크기 A(m)
		float    triplanarTileB;                  //          크기 B(m)
		float    warpScale;                       //          좌표 비틀기 노이즈 규모(m), 0 = 안 함
		float    blendDepth;                      // 경계 폭
		float    normalStrength;
		uint32_t triplanarLayers;                 //          비트 = 레이어(R 1 · G 2 · B 4 · A 8)
		float    warpAmount;                      //          좌표 비틀기 최대 거리(m)
		float    triplanarOffsetB;                //          1 = 크기 B 좌표에 (3.7, 1.9, 5.3) 을 더한다
		float    macroAmount;                     // 4단계 — 큰 규모 색 흔들림: 노이즈 × amount + bias
		float    macroBias;
		float    bumpDistance;                    //          범프 1(섞은 높이맵) Distance(m)
		float    reliefDistance;                  //          범프 2(지면 요철) Distance(m)
		float    reliefStrength;                  //          범프 2 Strength
		float    pad[3];
	};
	constexpr UINT kMaterialRootConstants = 20;   // 루트 파라미터 2 의 DWORD 수 = 두 상수 중 큰 쪽
	static_assert(sizeof(TerrainConstants) == kMaterialRootConstants * sizeof(uint32_t), "지형 상수 = 재질 루트 상수 20 DWORD");
	static_assert(sizeof(MaterialConstants) <= sizeof(TerrainConstants), "일반 재질 상수는 루트 상수 자리 안에 들어가야 한다");

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
			// 4단계 — 반사 히트 셰이더가 정점 · 인덱스를 읽는 바인드리스 힙 번호(raw SRV). 바인드리스를 못 쓰면 kNoDescriptor.
			uint32_t vertexDescriptor = 0xFFFFFFFFu;
			uint32_t indexDescriptor = 0xFFFFFFFFu;
		};

		struct TextureGpu
		{
			ComPtr<ID3D12Resource> resource;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
			uint32_t bindless = 0xFFFFFFFFu;   // 4단계 — 바인드리스 힙 번호(SRV). 바인드리스를 못 쓰면 kNoDescriptor
			uint32_t mipLevels = 1;            // 2026-10-28 지형 레이어 — 밉맵 DDS 는 1×1 까지 다 있다
			uint32_t arraySize = 1;            // 1 = Texture2D, 2 이상 = Texture2DArray (지형 레이어 4장 묶음)
		};

		// 텍스처의 SRV — 밉 전부 · 배열이면 배열 SRV. 재질 테이블 · 바인드리스 칸 · UI 칸이 같은 설명을 쓴다(2026-10-28).
		static D3D12_SHADER_RESOURCE_VIEW_DESC TextureSrvDesc(const TextureGpu& texture)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.Format = texture.format;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (texture.arraySize > 1)
			{
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srv.Texture2DArray.MipLevels = texture.mipLevels;
				srv.Texture2DArray.ArraySize = texture.arraySize;
			}
			else
			{
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Texture2D.MipLevels = texture.mipLevels;
			}
			return srv;
		}

		// ── SM 6.6 바인드리스 힙 (2026-10-27 렌더 4단계) ──
		//  셰이더가 ResourceDescriptorHeap[번호] 로 자원을 꺼내는 셰이더 가시 힙 하나.
		//  지금은 [4b] RT 반사만 이 힙을 쓴다(래스터 · 라이팅 · 그림자는 기존 테이블 방식 그대로 — 옮기는 건 다음 일).
		//  앞 16칸은 프레임 자원 자리(BindlessSlot), 그 뒤로 텍스처 · 메시가 만들어질 때 한 칸씩 받는다.
		//  장치가 SM 6.6 + 바인딩 티어 3 이 아니면 만들지 않는다 → [4b] 반사만 꺼진다.
		static constexpr uint32_t kNoDescriptor = 0xFFFFFFFFu;
		static constexpr UINT kBindlessCapacity = 65536;
		//  (5단계에서 4 ~ 12 를 더 씀 — [4d] 노이즈 제거가 읽고 쓰는 프레임 자원)
		enum BindlessSlot : uint32_t {
			BindlessGbNormal = 0, BindlessGbMaterial = 1, BindlessDepth = 2, BindlessReflectionUav = 3,
			BindlessReflectionSrv = 4,                         // [4b] 반사(노이즈 제거 전) SRV
			BindlessMotion = 5,                                // GB4 모션 벡터 SRV
			BindlessPrevDepth = 6, BindlessPrevNormal = 7,     // 이전 프레임 깊이 · 법선 사본 SRV
			BindlessHistorySrv0 = 8, BindlessHistoryUav0 = 9,  // 반사 이력 0번 (프레임마다 0 · 1 을 번갈아 읽고 쓴다)
			BindlessHistorySrv1 = 10, BindlessHistoryUav1 = 11,
			BindlessDenoisedUav = 12,                          // [4d] 결과(라이팅 t7 이 읽는 텍스처)
			// 6단계 — [6] 하늘
			BindlessHdrUav = 13,                               // HDR 컬러 UAV — 하늘 그리기(CSDrawSky)
			BindlessSkySrv = 14, BindlessSkyUav = 15,          // 하늘 텍스처 (등장방형 256×128)
			BindlessSkyIrradianceSrv = 16, BindlessSkyIrradianceUav = 17,   // 하늘 조도 / π (32×16)
			BindlessSkyMsSrv = 18, BindlessSkyMsUav = 19,      // 다중 산란 LUT (32×32)
			// 7단계 — [4c] GI 프로브
			BindlessProbeRayDataSrv = 20, BindlessProbeRayDataUav = 21,         // 레이 데이터 64 × 이번 프레임 프로브 수
			BindlessProbeIrradianceSrv = 22, BindlessProbeIrradianceUav = 23,   // 조도 아틀라스
			BindlessProbeDistanceSrv = 24, BindlessProbeDistanceUav = 25,       // 거리 아틀라스
			BindlessProbeStateSrv = 26, BindlessProbeStateUav = 27,             // 상태 버퍼 (int4 × 프로브 수)
			// 8단계 — [2] AO
			BindlessAoRawSrv = 28, BindlessAoRawUav = 29,                       // 레이 결과 (가시성, RT 썼나)
			BindlessAoHistorySrv0 = 30, BindlessAoHistoryUav0 = 31,             // 이력 0번 (AO, 누적 수)
			BindlessAoHistorySrv1 = 32, BindlessAoHistoryUav1 = 33,             // 이력 1번
			BindlessAoOutputSrv = 34, BindlessAoOutputUav = 35,                 // 노이즈 제거 결과 = 라이팅 t13
			// 9단계 — [7] 반투명
			BindlessHdrSrv = 36,                                                // HDR 컬러 SRV (색 피라미드 복사 원본)
			BindlessSceneColorSrv = 37,                                         // 색 피라미드 전체 밉 SRV (얼음이 흐린 단계로 읽는다)
			BindlessSceneColorMipSrv0 = 38,                                     // 밉마다 SRV 38 ~ 44 (내리기 원본)
			BindlessSceneColorMipUav0 = 45,                                     // 밉마다 UAV 45 ~ 51 (내리기 대상)
			// 10단계 — [8] 후처리
			BindlessBloomDownSrv0 = 52, BindlessBloomDownUav0 = 58,            // 블룸 내리기 밉마다 SRV 52~57 · UAV 58~63
			BindlessBloomUpSrv0 = 64, BindlessBloomUpUav0 = 70,                // 블룸 올리기 밉마다 SRV 64~69 · UAV 70~75
			BindlessHistogramUav = 76,                                          // 휘도 히스토그램 (raw, uint × 256)
			BindlessExposureSrv = 77, BindlessExposureUav = 78,                // 노출 버퍼 (float4 하나)
			// 11단계 — [3] 분류
			BindlessReflectionTileSrv = 79, BindlessReflectionTileUav = 80,    // 반사 타일 목록 (uint × 화면 타일 수)
			BindlessReflectionArgsUav = 81,                                     // DispatchRays 간접 인자 (raw — Width 에 원자 더하기)
			// 2026-10-28 — [4a] 그림자를 바인드리스로 옮기면서
			BindlessShadowMaskUav = 82,                                         // 그림자 마스크 UAV (RtShadow.hlsl 이 쓴다)
			// 앞 128칸을 프레임 자원 자리로 잡아 둔다(6단계에서 16 → 64, 10단계에서 → 128).
			//  텍스처 · 메시 번호는 그 뒤부터 받으므로 이 값이 바뀌어도 셰이더는 영향이 없다(재질 표 · 인스턴스 표에 번호가 적힌다).
			BindlessReserved = 128 };
		bool                         bindlessSupported = false;
		ComPtr<ID3D12DescriptorHeap> bindlessHeap;
		UINT                         bindlessDescriptorSize = 0;
		uint32_t                     bindlessNext = BindlessReserved;
		bool                         bindlessOverflow = false;   // 힙이 차서 번호를 못 받은 텍스처 · 메시가 있다 → [4b] 반사를 끈다

		// 다음 빈 칸 번호. 다 찼으면 kNoDescriptor 를 돌려주고 bindlessOverflow 를 켠다
		// (번호 없는 자원을 셰이더가 읽으면 힙 밖을 읽게 되므로, 그런 일이 한 번이라도 있으면 반사를 아예 돌리지 않는다).
		uint32_t AllocateBindless()
		{
			if (!bindlessHeap) return kNoDescriptor;
			if (bindlessNext >= kBindlessCapacity)
			{
				if (!bindlessOverflow)
					OutputDebugStringW(L"[GRenderer] 바인드리스 힙이 찼다 — [4b] RT 반사 끔\n");
				bindlessOverflow = true;
				return kNoDescriptor;
			}
			return bindlessNext++;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE BindlessCpu(uint32_t index) const
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handle = bindlessHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += SIZE_T(index) * bindlessDescriptorSize;
			return handle;
		}

		// ── [4b] RT 반사 (2026-10-27 렌더 4단계) ──
		//  반사 텍스처: RtReflection.hlsl 이 바인드리스 UAV 로 쓰고, Lighting.hlsl 이 타깃 힙 SRV 로 읽는다.
		static constexpr DXGI_FORMAT kReflectionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // 반사 색은 1 을 넘을 수 있다
		static constexpr float kReflectionMaxRoughness = 0.4f;   // 이 거칠기 이상은 반사 레이를 안 쏜다 — UE Lumen 기본값(«첫 판 범위» 와 같이 다시 정한다)
		ComPtr<ID3D12RootSignature> rtReflectionRootSig;   // 전역 루트 시그니처 (CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)
		ComPtr<ID3D12StateObject>   rtReflectionPso;       // RT 파이프라인 상태 객체 (RayGen · Miss · HitGroup)
		ComPtr<ID3D12Resource>      rtShaderTable;         // UPLOAD — raygen 0 · miss 64 · hit group 128 바이트 자리
		ComPtr<ID3D12Resource>      reflectionCB;          // UPLOAD, 영속 매핑
		uint8_t*                    reflectionCBData = nullptr;
		//  TLAS 인스턴스 표: 인스턴스 순번 → 정점 · 인덱스 버퍼 번호 · 재질. 프레임 슬롯마다 한 벌(AccelStructure 와 같은 규칙).
		ComPtr<ID3D12Resource>      rtInstances;
		uint8_t*                    rtInstanceData = nullptr;

		// ── [3] 분류 — 반사 레이가 필요한 타일만 DispatchRays (2026-10-27 렌더 11단계, ReflectionClassify.hlsl) ──
		//  분류 컴퓨트가 8×8 타일 목록과 D3D12_DISPATCH_RAYS_DESC(Width = 타일 수 × 64)를 GPU 에서 채우고,
		//  ExecuteIndirect(DISPATCH_RAYS 명령 시그니처)가 그 인자로 RayGen 을 띄운다. 하나라도 못 만들면 예전 DispatchRays(화면 크기).
		//  타일 목록 · 인자는 RenderTarget 과 같은 «자원 + 지금 상태» 로 들고 TransitionTargets 로 배리어를 맞춘다(정의는 아래).
		static constexpr UINT kReflectionTileSize = 8;              // ReflectionTiles.hlsli 의 REFLECTION_TILE_SIZE 와 같아야 한다
		ComPtr<ID3D12PipelineState>    classifyPso;                 // CSClassifyTiles (바인드리스 컴퓨트 공용 루트)
		ComPtr<ID3D12CommandSignature> dispatchRaysSignature;       // DISPATCH_RAYS 하나, 루트 시그니처 없음(바인딩을 안 바꾼다)
		ComPtr<ID3D12Resource>         reflectionArgsTemplate;      // UPLOAD — 셰이더 테이블 주소 · Width 0 · Height 1 · Depth 1
		ComPtr<ID3D12Resource>         classifyCB;                  // UPLOAD, 영속 매핑
		uint8_t*                       classifyCBData = nullptr;
		UINT                           reflectionTilesX = 0, reflectionTilesY = 0;

		// [4b] 셰이더 테이블(초기화 때 써 둔 raygen 0 · miss 64 · hit group 128 바이트 자리, 레코드 32 B)로 DispatchRays 인자를 만든다.
		//  분류가 꺼졌을 때의 직접 DispatchRays(화면 크기)와 [3] 분류 인자 원본(Width 0)이 같이 쓴다 — 표 배치를 한 곳에만 둔다.
		D3D12_DISPATCH_RAYS_DESC ReflectionDispatchDesc(UINT gridWidth, UINT gridHeight) const
		{
			const D3D12_GPU_VIRTUAL_ADDRESS table = rtShaderTable->GetGPUVirtualAddress();
			const UINT64 align = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
			const UINT64 record = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			D3D12_DISPATCH_RAYS_DESC rays = {};
			rays.RayGenerationShaderRecord = { table, record };
			rays.MissShaderTable = { table + align, record, record };
			rays.HitGroupTable = { table + 2 * align, record, record };
			rays.Width = gridWidth;
			rays.Height = gridHeight;
			rays.Depth = 1;
			return rays;
		}

		// ── [4d] 반사 노이즈 제거 — 임시 구현 (2026-10-27 렌더 5단계, RtReflectionDenoise.hlsl) ──
		//  B 방식 반사(레이 1개라 점박이)를 시간 누적 + 공간 필터로 지운다. NRD · DLSS RR 이 정해지면 바뀌는 자리.
		//  텍스처(이력 2장 · 결과 · 이전 깊이/법선 사본)는 RenderTarget 정의 뒤, reflection 옆에 있다.
		ComPtr<ID3D12RootSignature> denoiseRootSig;         // b0 + CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
		ComPtr<ID3D12PipelineState> denoiseTemporalPso;     // cs_6_6 CSTemporal
		ComPtr<ID3D12PipelineState> denoiseSpatialPso;      // cs_6_6 CSSpatial
		ComPtr<ID3D12Resource>      denoiseCB;              // UPLOAD, 영속 매핑
		uint8_t*                    denoiseCBData = nullptr;

		// ── 바인드리스 컴퓨트 공용 루트 시그니처 (2026-10-27 렌더 6단계) ──
		//  6단계부터 새로 붙는 컴퓨트 패스([6] 하늘, 뒤의 GI · AO · 후처리)가 같이 쓴다.
		//   b0 = 패스 상수(루트 CBV), t0 = TLAS, t1 = 재질 표, t2 = 인스턴스 표 (루트 SRV — RT 를 안 쓰는 패스는 안 읽는다)
		//   s0 = 선형 반복, s1 = 선형 U 반복 · V 고정(등장방형), s2 = 선형 고정
		//   플래그 CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED — 텍스처는 전부 바인드리스 번호로 읽고 쓴다.
		ComPtr<ID3D12RootSignature> computeRootSig;
		uint32_t                    historyIndex = 0;       // 이번 프레임에 쓸 이력 번호 (지난 프레임 것 = 1 - historyIndex)
		bool                        historyValid = false;   // 지난 프레임에 반사 · 노이즈 제거가 돌아 이력 · 사본이 유효한가

		// ── 이전 프레임 카메라 (2026-10-27 렌더 5단계 — 모션 벡터 · 이력 위치 되살리기용) ──
		XMFLOAT4X4                  prevMainViewProj{};     // 지난 프레임 메인 패스 viewProj (전치 전, 패킷 그대로)
		bool                        hasPrevMainView = false;

		struct MaterialGpu
		{
			bool doubleSided = false;
			MaterialConstants constants{};
			D3D12_GPU_DESCRIPTOR_HANDLE textures{};
			// ── 투과 (2026-10-27 렌더 2단계) — CreateMaterial 이 MaterialData 의 투과 값으로 한 번 계산한다 ──
			//  transmissive 면 TLAS 인스턴스를 FORCE_NON_OPAQUE 로 넣어 [4a] 그림자 레이가 «통과 후보» 로 본다.
			bool transmissive = false;
			XMFLOAT3 transmittance{ 0.0f, 0.0f, 0.0f };   // 그림자 레이가 이 물체 하나를 지나고 남는 빛 (RGB)
			// ── 알파 마스크 (2026-10-28) — glTF alphaMode MASK · alphaCutoff > 0 ──
			//  래스터: 묶음 키에 4 를 더해 discard 가 있는 PSO 로 그린다(마스크 아닌 물체는 discard 없는 PSO 그대로).
			//  RT: TLAS 인스턴스를 FORCE_NON_OPAQUE 로 넣어 레이가 후보로 받아 빈 곳을 거른다.
			bool alphaMask = false;
			// ── 지형 레이어 (2026-10-28) — CreateTerrainMaterial 이 만든 재질 ──
			//  묶음 키 kTerrainPsoKey(+1 반전)의 PSO(Forward.hlsl TERRAIN_LAYERS=1 판)로 그린다.
			//  루트 상수는 terrainConstants(20 DWORD, 4단계부터 constants 보다 크다), 텍스처 5칸에는 배열 4개 + 마스크가 들어 있다.
			bool terrain = false;
			TerrainConstants terrainConstants{};
		};

		// 묶음 키(Batch::pso) — 0 ~ 7 은 «양면 1 · 반전 2 · 알파 마스크 4» 조합, 8 · 9 = 지형 레이어 판(9 = 반전).
		//  지형은 단면 · 알파 없음이라 반전 여부만 나눈다(2026-10-28).
		static constexpr uint32_t kTerrainPsoKey = 8;
		static constexpr uint32_t kPsoKeyCount = 10;

		// RT 재질 표 — 재질 번호(TLAS InstanceID) → RtMaterialGpu(96 B). [4a] 그림자는 투과만, [4b] 반사는 전부 읽는다.
		//  (2단계에는 float4 남는 빛뿐이었고, 4단계에서 텍스처 번호 · 상수까지 넓혔다)
		//  UPLOAD 영속 매핑. CreateMaterial 이 자기 칸만 쓴다(프레임 밖에서만 불리므로 GPU 와 겹치지 않는다).
		ComPtr<ID3D12Resource> rtMaterials;
		uint8_t*               rtMaterialData = nullptr;

		ComPtr<ID3D12Device5>              device;
		ComPtr<ID3D12CommandQueue>         commandQueue;
		ComPtr<IDXGISwapChain3>            swapChain;
		ComPtr<ID3D12DescriptorHeap>       rtvHeap;
		ComPtr<ID3D12DescriptorHeap>       dsvHeap;
		ComPtr<ID3D12DescriptorHeap>       materialHeap;
		ComPtr<ID3D12Resource>             renderTargets[FrameCount];
		ComPtr<ID3D12CommandAllocator>     commandAllocator;
		ComPtr<ID3D12GraphicsCommandList4> commandList;
		ComPtr<ID3D12RootSignature>        rootSig;
		// forward PSO 4종 — 2026-10-27 부터 배경 패스(먼 행성)만 쓴다. 메인 패스는 gbufferPsos.
		ComPtr<ID3D12PipelineState>        pso;
		ComPtr<ID3D12PipelineState>        doubleSidedPso;
		ComPtr<ID3D12PipelineState>        mirroredPso;
		ComPtr<ID3D12PipelineState>        mirroredDoubleSidedPso;
		// 2026-10-28 알파 마스크 — 배경 forward 의 마스크 판 4종(PSMain + ALPHA_MASK=1, 묶음 키 4 ~ 7)
		ComPtr<ID3D12PipelineState>        forwardMaskedPsos[4];
		// 2026-10-28 지형 레이어 — 배경 forward 의 지형 판 2종(PSMain + TERRAIN_LAYERS=1, 묶음 키 8 · 9 = 기본 · 반전)
		ComPtr<ID3D12PipelineState>        forwardTerrainPsos[2];
		ComPtr<ID3D12Resource>             frameCB;
		uint8_t*                           frameCBData = nullptr;

		// ── 렌더 타깃 (2026-10-27 파이프라인 1단계) ──
		//  장면은 백버퍼가 아니라 HDR 컬러에 그리고, 마지막 [8] 에서 백버퍼로 옮긴다.
		//  메인 패스는 [1] G-buffer 에 표면 정보만 쓰고, [5] 라이팅 컴퓨트가 HDR 컬러에 조명 결과를 쓴다.
		//  타깃마다 «지금 상태» 를 같이 들고 다닌다 — 패스가 늘어도 배리어를 한 곳(TransitionTargets)에서 맞춘다.
		struct RenderTarget
		{
			ComPtr<ID3D12Resource> resource;
			D3D12_RESOURCE_STATES  state = D3D12_RESOURCE_STATE_COMMON;
		};
		enum GBufferSlot : UINT { GbAlbedo, GbNormal, GbMaterial, GbEmissive, GbMotion, GbCount };
		// Shaders/GBuffer.hlsli 의 표와 순서 · 형식이 같아야 한다.
		//  (5단계 후속 — GB4 모션 벡터도 이 배열에 넣었다. 처음엔 셰이더 레지스터 번호를 안 밀려고 따로 들었는데,
		//   그러면 생성 · RTV · SRV · PSO 형식 · 배리어마다 GB4 만 따로 챙겨야 하고 이 «hlsli 표와 같다» 규칙도 깨졌다)
		static constexpr DXGI_FORMAT kGBufferFormats[GbCount] = {
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,   // GB0 알베도 (a 안 씀)
			DXGI_FORMAT_R16G16_SNORM,          // GB1 월드 법선 — 팔면체 인코딩
			DXGI_FORMAT_R8G8B8A8_UNORM,        // GB2 r 거칠기 · g 금속성
			DXGI_FORMAT_R11G11B10_FLOAT,       // GB3 자체발광
			DXGI_FORMAT_R16G16_FLOAT };        // GB4 모션 벡터 (2026-10-27 렌더 5단계)
		static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		static constexpr float kClearColor[4] = { 0.05f, 0.08f, 0.14f, 1.0f };   // 예전 백버퍼 지우기 색 그대로

		// RTV 힙: 0~1 백버퍼, 2 HDR, 3~7 G-buffer(GB0~GB4).
		//  G-buffer 5장은 붙어 있어야 OMSetRenderTargets 한 번(연속 범위)으로 묶인다.
		static constexpr UINT kRtvHdr = FrameCount;
		static constexpr UINT kRtvGBuffer = FrameCount + 1;
		static constexpr UINT kRtvCount = kRtvGBuffer + GbCount;

		// 타깃 SRV·UAV 힙(셰이더 가시).
		//  라이팅 테이블 = 슬롯 0 시작 (SRV t0~t4 G-buffer GB0~GB4, t5 깊이 / UAV u0 HDR 슬롯 6 /
		//                  SRV t6 그림자 마스크 · t7 반사(노이즈 제거 후) · t8 반사(제거 전) = 슬롯 9~11)
		//  톤맵 테이블   = 슬롯 7 (SRV t0 HDR)
		//  RT 그림자 테이블 = 슬롯 0 시작 (SRV t0~t5 / UAV u0 그림자 마스크 슬롯 8)   ← 2026-10-27 렌더 2단계
		//  (5단계 후속 — GB4 를 G-buffer 배열에 넣으면서 깊이가 t4 → t5 로, 그 뒤 번호가 하나씩 밀렸다)
		enum TargetSlot : UINT {
			SlotGBuffer = 0, SlotDepth = 5, SlotHdrUav = 6, SlotHdrSrv = 7,
			SlotShadowUav = 8, SlotShadowSrv = 9,
			SlotReflectionSrv = 10,    // 4단계 — 라이팅 t7 (5단계부터 [4d] 노이즈 제거 후 반사)
			SlotReflectionRawSrv = 11, // 5단계 — 라이팅 t8 (노이즈 제거 전 반사, 디버그 뷰 7)
			SlotSkyIrradianceSrv = 12, // 6단계 — 라이팅 t9 ([6] 하늘 조도 / π — 주변광)
			SlotProbeIrradianceSrv = 13, SlotProbeDistanceSrv = 14, SlotProbeStateSrv = 15,   // 7단계 — 라이팅 t10 ~ t12 ([4c] GI 프로브)
			SlotAoSrv = 16,            // 8단계 — 라이팅 t13 ([2] AO)
			TargetSlotCount = 17 };
		// static_assert 메시지는 좁은 문자열(CP949)이라 «—» 같은 문자를 쓰면 C4566 경고가 난다 — 쓰지 않는다.
		static_assert(SlotDepth == GbCount, "깊이 SRV 는 G-buffer SRV 바로 뒤: 라이팅 테이블 t5");
		static_assert(SlotReflectionSrv == SlotShadowSrv + 1 && SlotReflectionRawSrv == SlotShadowSrv + 2 &&
			SlotSkyIrradianceSrv == SlotShadowSrv + 3 && SlotProbeIrradianceSrv == SlotShadowSrv + 4 &&
			SlotProbeDistanceSrv == SlotShadowSrv + 5 && SlotProbeStateSrv == SlotShadowSrv + 6 && SlotAoSrv == SlotShadowSrv + 7,
			"라이팅 테이블 t6 부터 SRV 는 붙어 있어야 한다: 그림자 / 반사 / 제거 전 반사 / 하늘 조도 / 프로브 3개 / AO");
		static constexpr UINT kLightingTailSrvCount = SlotAoSrv - SlotShadowSrv + 1;   // t6 부터 이어지는 SRV 수 (t6 ~ t13)

		// ── [4a] RT 그림자 (2026-10-27 렌더 2단계) ──
		//  그림자 마스크: 픽셀마다 태양 가시도 rgb(투과 물체를 지나면 연하게). RtShadow.hlsl 이 쓰고 Lighting.hlsl 이 읽는다.
		//  0~1 값이라 8비트로 충분하다. RT 미지원 장치에서도 자원은 만들어 둔다(라이팅 테이블이 항상 가리키므로).
		static constexpr DXGI_FORMAT kShadowFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		static constexpr float kShadowMaxDistance = 5000.0f;   // 그림자 레이 길이(m) — 행성 지름 3.2 km 를 넘는다
		RenderTarget                shadowMask;
		RenderTarget                reflection;   // 4단계 [4b] — 반사 색 (R16G16B16A16_FLOAT, UAV + SRV). 5단계부터 «노이즈 제거 전», a = 맞은 거리
		// [4d] 노이즈 제거 텍스처 (5단계). 이력 두 장을 프레임마다 번갈아 «지난 프레임 것(읽기) / 이번 프레임 것(쓰기)» 으로 쓴다.
		//  이전 프레임 깊이 · 법선은 프레임 끝에 사본을 떠 둔다 — 이력을 «같은 표면» 에서만 가져오려고.
		RenderTarget                reflectionHistory[2];   // R16G16B16A16_FLOAT — rgb 누적 반사, a 누적 수
		RenderTarget                reflectionDenoised;     // R16G16B16A16_FLOAT — [5] 라이팅 t7
		RenderTarget                prevDepth;              // R32_TYPELESS — 깊이 사본 (SRV R32_FLOAT)
		RenderTarget                prevNormal;             // R16G16_SNORM — GB1 사본
		// ── [6] 하늘 (2026-10-27 렌더 6단계, Sky.hlsl) ──
		//  하늘 텍스처(방향 → 휘도)를 하늘 그리기 · 반사 미스 · 주변광이 같이 읽는다. 지금은 대기 산란으로 계산해 채운다(임시).
		//  바인드리스(SM 6.6)가 있어야 만든다 — 없으면 예전처럼 지우기 색 하늘 · 상수 주변광.
		static constexpr UINT  kSkyWidth = 256, kSkyHeight = 128;                     // 등장방형 하늘 텍스처
		static constexpr UINT  kSkyIrradianceWidth = 32, kSkyIrradianceHeight = 16;   // 하늘 조도 / π
		static constexpr UINT  kSkyMsSize = 32;                                       // 다중 산란 LUT (Hillaire 2020 과 같은 32×32)
		static constexpr DXGI_FORMAT kSkyFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		static constexpr float kSunIlluminanceTop = 3.14159265f;   // 대기 위 태양 조도 — 흰 표면이 해를 정면으로 받으면 휘도 1
		static constexpr float kSunAngularRadius = 0.004675f;      // 해 원반 각반지름(rad) — Hillaire 기본값
		static constexpr float kSkyObserverHeight = 2.0f;          // 대기 계산 속 관측 높이(m)
		RenderTarget                skyTexture;        // UAV(CSGenerateSky) + SRV
		RenderTarget                skyIrradiance;     // UAV(CSSkyIrradiance) + SRV(라이팅 t9 · 반사 히트)
		RenderTarget                skyMultiScattering;   // UAV(CSMultiScattering) + SRV(CSGenerateSky) — 처음 한 번
		bool                        skyMsValid = false;
		ComPtr<ID3D12PipelineState> skyMultiScatteringPso;
		ComPtr<ID3D12PipelineState> skyGeneratePso;
		ComPtr<ID3D12PipelineState> skyIrradiancePso;
		ComPtr<ID3D12PipelineState> skyDrawPso;
		ComPtr<ID3D12Resource>      skyCB;             // UPLOAD, 영속 매핑
		uint8_t*                    skyCBData = nullptr;
		bool                        skyValid = false;  // 하늘 텍스처가 지금 해 · 위 값으로 만들어져 있다
		XMFLOAT3                    skyBuiltUp{};       // 하늘 텍스처를 만든 때의 위 · 해 방향 — 바뀌면 다시 만든다
		XMFLOAT3                    skyBuiltSunDir{};
		bool                        skyDoneThisFrame = false;   // 이번 프레임의 첫 Render 에서 해 · 하늘을 이미 처리했다
		XMFLOAT3                    mainViewUp{ 0.0f, 1.0f, 0.0f };   // 2026-10-28 — 마지막 메인 화면의 위쪽(행성 기준) — 하늘 · 해 높이 기준
		bool                        hasMainViewUp = false;
		XMFLOAT3                    sunIlluminance{ 0.85f, 0.85f, 0.85f };   // 이번 프레임 «땅에 닿는 태양 조도» (모든 패스 상수)

		// ── [4c] GI 프로브 (2026-10-27 렌더 7단계, ProbeGI.hlsl · Probes.hlsli) ──
		//  설계 문서 3.2 · 6.4 의 DDGI. 카메라를 따라 스크롤하는 32×8×32 격자(간격 4 m) — 짧은 축(8)은 «위» 에 가장 가까운 월드 축.
		//  (설계 문서는 y 가 짧은 축. 행성 위라 걷는 자리마다 위가 달라서, 위에 가까운 축을 고른다 — 바뀌면 세대를 올려 다시 채운다)
		//  프레임마다 1/4(2048개)씩 64 레이로 갱신. RT + 바인드리스 장치에서만.
		//  ★ 2026-10-28 두 겹(캐스케이드, Majercik 외 2021 7.3) — 사용자 «GI 겹은 언제 늘릴건데» (지평선까지 덮기).
		//    겹 0 = 가까운 겹(4 m), 겹 1 = 먼 겹(16 m). 두 겹 다 32×8×32 이고 아틀라스 · 상태 버퍼를 같이 쓴다(먼 겹은 kProbeCount 뒤).
		//    프레임마다 겹마다 2048개씩 갱신한다(겹마다 4 프레임에 한 바퀴).
		//  ★ 2026-10-28 지면에 맞춘 격자 — 사용자 «플레이어 기준 거리를 좀 늘려» · «정육면체면 또 바닥 아래까지 측정하는거 아니야?».
		//    짧은 축(8)을 월드 축이 아니라 지면 위쪽에 맞춰 격자를 돌린다(probeAxes, 행성 중심 기준). 그래서 얇은 격자가 땅과 나란해
		//    가까운 겹은 플레이어 둘레 수평 ±64 m, 먼 겹은 ±256 m 를 고르게 덮고 땅 속으로 들어가는 칸이 적다.
		//    걸어서 위쪽이 격자 축에서 kProbeReorientDegrees 넘게 기울면 다시 맞추고 두 겹을 새로 채운다(세대 +1).
		static constexpr int   kProbeGridLong = 32, kProbeGridShort = 8;
		static constexpr UINT  kProbeCount = UINT(kProbeGridLong * kProbeGridLong * kProbeGridShort);   // 8192 — 겹마다
		static constexpr UINT  kProbesPerFrame = kProbeCount / 4;                                      // 2048 — 겹마다
		static constexpr int   kProbeLayerCount = 2;
		static constexpr UINT  kProbeFarCount = kProbeCount;                                           // 먼 겹도 32×8×32
		static constexpr float kProbeFarSpacing = 16.0f;
		static constexpr UINT  kProbeTotalCount = kProbeCount + kProbeFarCount;                        // 16384 — 아틀라스 · 상태 버퍼
		static constexpr float kProbeReorientDegrees = 10.0f;                                          // 격자를 다시 맞추는 기울기(임시값)
		static_assert(kProbeFarCount % kProbesPerFrame == 0, "먼 겹 프로브 수는 프레임당 갱신 수의 배수(바퀴 계산)");
		static constexpr UINT  kProbeRays = 64;
		static constexpr UINT  kProbesPerRow = 128;                                                     // 아틀라스 한 줄
		static constexpr UINT  kProbeIrradianceTexels = 8, kProbeDistanceTexels = 16;
		static constexpr float kProbeSpacing = 4.0f;
		static constexpr float kProbeNormalBias = 0.2f;    // 간격 배수 — 설계 문서 6.4 «노멀 쪽으로 0.2 × 간격»
		static constexpr float kProbeViewBias = 0.1f;      // 간격 배수 — RTXGI 의 시선 쪽 띄우기(벽 너머 새는 빛 줄이기)
		static constexpr float kProbeMaxDistance = 1000.0f;   // 프로브 레이 길이(m) — 설계 문서
		static constexpr DXGI_FORMAT kProbeIrradianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // 설계 문서는 R11G11B10 — 0.97 섞기의 3 % 변화가 6비트 가수에 묻혀 수렴이 멈출 수 있어 16비트로
		static constexpr DXGI_FORMAT kProbeDistanceFormat = DXGI_FORMAT_R16G16_FLOAT;
		RenderTarget                probeRayData;       // 64 × 2048, rgb = 휘도 · a = 거리(뒷면이면 음수) — 두 겹이 차례로 쓴다
		RenderTarget                probeIrradiance;    // 1024 × 1024 (프로브 8×8 × 128 × 128, 10-28 두 겹으로 512 → 1024)
		RenderTarget                probeDistance;      // 2048 × 2048 (프로브 16×16 × 128 × 128)
		RenderTarget                probeStates;        // 버퍼 int4 × 16384 — 격자 칸 · 세대 · 상태 (가까운 겹 0 ~ 8191 · 먼 겹 8192 ~)
		ComPtr<ID3D12PipelineState> giTracePso;
		ComPtr<ID3D12PipelineState> giBlendDistancePso;
		ComPtr<ID3D12PipelineState> giBlendIrradiancePso;
		ComPtr<ID3D12Resource>      giCB;               // UPLOAD, 영속 매핑 — 겹마다 512 B 칸(10-28)
		uint8_t*                    giCBData = nullptr;
		static constexpr UINT       kGiCBSlot = 512;    // GiConstants 400 B → CBV 256 배수
		// 겹마다의 상태 (2026-10-28 두 겹, 예전 probeDims · probeGeneration · probeCursor · probeCycle)
		struct ProbeLayer
		{
			uint32_t generation = 0;          // 격자 축(probeAxes)을 다시 맞추면 +1
			uint32_t cursor = 0;              // 다음 프레임에 갱신할 첫 번호(겹 안)
			uint32_t cycle = 0;               // 커서가 한 바퀴 돌 때마다 +1 (ProbeGI.hlsl ProbeOff 가 다시 잴 바퀴를 고른다)
		};
		ProbeLayer                  probeLayers[kProbeLayerCount];
		XMFLOAT3                    probeAxes[3] = {};            // 2026-10-28 — 두 겹이 같이 쓰는 격자 축(X · Y = 지면 위쪽 · Z)
		bool                        probeAxesValid = false;
		ProbeVolumeGpu              probeVolume{};                // 이번 프레임 라이팅 · 반사 · 반투명이 쓸 가까운 겹 (enabled 0 = 못 씀)
		ProbeVolumeGpu              probeVolumeFar{};             // 2026-10-28 — 먼 겹

		// ── [2] AO (2026-10-27 렌더 8단계, AO.hlsl) ──
		//  화면 공간으로 레이를 따라가다 «모름» 인 레이만 RayQuery — 사용자 생각(«기본 SSAO + 화면에 없는 부분만 RTAO»)을 레이 단위로.
		//  레이 1개/픽셀 → 시간 누적 + 공간 필터. 바인드리스 장치에서만(RT 가 없으면 순수 SSAO 로 돈다).
		static constexpr float kAoRadius = 1.5f;      // AO 반경(m) — 임시값(미정)
		static constexpr float kAoThickness = 0.5f;   // 깊이 표면 뒤 이 두께 안이면 «가려짐» — 임시값(미정)
		static constexpr DXGI_FORMAT kAoRawFormat = DXGI_FORMAT_R16G16_FLOAT;   // (가시성, RT 썼나) · 이력 (AO, 누적 수)
		static constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16_FLOAT;
		RenderTarget                aoRaw;
		RenderTarget                aoHistory[2];
		RenderTarget                aoOutput;          // 라이팅 t13
		ComPtr<ID3D12PipelineState> aoTracePso;
		ComPtr<ID3D12PipelineState> aoTemporalPso;
		ComPtr<ID3D12PipelineState> aoSpatialPso;
		ComPtr<ID3D12Resource>      aoCB;              // UPLOAD, 영속 매핑 (288 B → 512 B 칸)
		uint8_t*                    aoCBData = nullptr;
		uint32_t                    aoHistoryIndex = 0;
		bool                        aoHistoryValid = false;

		// ── [7] 반투명 (2026-10-27 렌더 9단계, Transparent.hlsl · ColorPyramid.hlsl) ──
		//  투과 재질(얼음) 물체는 G-buffer 에서 빼고 라이팅 뒤 forward 로 그린다. 흐린 굴절 = 화면 색 피라미드(HDRP 방식, 임시).
		//  픽셀 셰이더 RayQuery 를 써서 RT 장치 + 바인드리스에서만. 파이프라인이 없으면 예전처럼 G-buffer 에 불투명으로 그린다.
		static constexpr UINT kSceneColorMips = 7;   // 1280×720 → 20×11
		RenderTarget                sceneColor;      // R16G16B16A16_FLOAT, 밉 7단 — UAV(피라미드) + SRV(얼음)
		ComPtr<ID3D12PipelineState> pyramidCopyPso;
		ComPtr<ID3D12PipelineState> pyramidDownsamplePso;
		ComPtr<ID3D12Resource>      pyramidCB;       // UPLOAD, 디스패치마다 256 B 칸 (밉 수만큼)
		uint8_t*                    pyramidCBData = nullptr;
		ComPtr<ID3D12RootSignature> transparentRootSig;
		ComPtr<ID3D12PipelineState> transparentPso;
		ComPtr<ID3D12Resource>      transparentCB;   // UPLOAD, 288 B → 512 B
		uint8_t*                    transparentCBData = nullptr;
		struct TransparentDraw { uint32_t instance; MaterialHandle material; MeshHandle mesh; float distanceSq; };
		std::vector<TransparentDraw> transparentDraws;   // 이번 프레임에 그릴 투과 물체(뒤에서 앞으로 정렬)
		// ── [8] 후처리 (2026-10-27 렌더 10단계, PostProcess.hlsl · Tonemap.hlsl PSTonemap) ──
		//  블룸(Jimenez 2014) → 자동 노출(휘도 히스토그램, UE 기본값) → ACES 근사 톤 곡선(설계 문서 6.7) → sRGB.
		//  디버그 뷰(0 이 아님)와 바인드리스가 없는 장치는 예전 «0~1 자르기» 톤맵을 쓴다(값을 숫자 그대로 보려고).
		static constexpr UINT  kBloomMips = 6;            // 반 해상도 640×360 → 20×11
		static constexpr float kBloomStrength = 0.04f;    // 화면에 섞는 비율 (LearnOpenGL 해설의 값, 임시)
		static constexpr float kExposureLogMin = -8.0f, kExposureLogMax = 4.0f;           // UE 기본값
		static constexpr float kExposureLowPercent = 0.10f, kExposureHighPercent = 0.90f; // UE 기본값
		static constexpr float kExposureMinBrightness = 0.03f, kExposureMaxBrightness = 8.0f;
		static constexpr float kExposureSpeedUp = 3.0f, kExposureSpeedDown = 1.0f;       // 스톱/초, UE 기본값
		static constexpr UINT  kPostDispatchSlots = 16;   // postCB 256 B 칸 수 (히스토그램 · 노출 · 내리기 6 · 올리기 5)
		RenderTarget                bloomDown;           // R16G16B16A16_FLOAT 반 해상도, 밉 6단
		RenderTarget                bloomUp;             // 같은 크기 · 밉 — 0 ~ 4 단만 쓴다
		RenderTarget                histogram;           // 버퍼 uint × 256 (raw)
		RenderTarget                exposureBuffer;      // 버퍼 float4 × 1 — (노출, 적응 log2 밝기, 목표, 유효)
		ComPtr<ID3D12PipelineState> histogramPso, exposurePso, bloomDownPso, bloomUpPso;
		ComPtr<ID3D12Resource>      postCB;
		uint8_t*                    postCBData = nullptr;
		ComPtr<ID3D12RootSignature> tonemapBindlessRootSig;
		ComPtr<ID3D12PipelineState> tonemapBindlessPso;
		LARGE_INTEGER               lastFrameCounter{};   // 자동 노출 적응(스톱/초)에 쓰는 프레임 시간
		float                       frameDeltaSeconds = 1.0f / 60.0f;

		// ── [3] 분류 버퍼 (2026-10-27 렌더 11단계 — 파이프라인 · 인자 원본은 [4b] 옆 «[3] 분류» 주석) ──
		RenderTarget                reflectionTiles;     // 버퍼 uint × 화면 타일 수 — 고른 타일 (x | y << 16)
		RenderTarget                reflectionArgs;      // 버퍼 D3D12_DISPATCH_RAYS_DESC 하나 — ExecuteIndirect 인자

		D3D12_CPU_DESCRIPTOR_HANDLE ReadOnlyDsv() const   // DSV 힙 1번 — 깊이 테스트만, 쓰지 않음(셰이더가 같은 깊이를 SRV 로 읽는다)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
			return handle;
		}

		// [4a] RT 그림자 — 2026-10-28 부터 컴퓨트 공용 바인드리스 루트(computeRootSig) · cs_6_6 (알파 마스크 판정에 재질 텍스처가 필요)
		ComPtr<ID3D12PipelineState> rtShadowPso;        // RT + 바인드리스 장치에서만 만든다
		ComPtr<ID3D12Resource>      shadowCB;           // UPLOAD, 영속 매핑
		uint8_t*                    shadowCBData = nullptr;

		RenderTarget hdr;                  // HDR 장면 컬러: 배경 forward 가 그리고, 라이팅이 쓰고, 톤맵이 읽는다
		RenderTarget gbuffer[GbCount];     // GB0~GB4 (5단계부터 GB4 모션 벡터 포함)
		RenderTarget depth;               // 자원 R32_TYPELESS — DSV 는 D32_FLOAT, 컴퓨트 SRV 는 R32_FLOAT
		ComPtr<ID3D12DescriptorHeap> targetHeap;
		UINT targetDescriptorSize = 0;

		// Batch::pso 순서: 0 기본 · 1 양면 · 2 반전 · 3 반전 양면, +4 = 알파 마스크 판(2026-10-28), 8 · 9 = 지형 레이어 판(기본 · 반전, 2026-10-28)
		ComPtr<ID3D12PipelineState> gbufferPsos[kPsoKeyCount];
		ComPtr<ID3D12RootSignature> lightingRootSig;
		ComPtr<ID3D12PipelineState> lightingPso;
		ComPtr<ID3D12Resource>      lightingCB;       // UPLOAD, 영속 매핑. 프레임마다 GPU 를 기다리므로 한 벌이면 된다
		uint8_t*                    lightingCBData = nullptr;
		static constexpr UINT LightingGroupSize = 8;  // Lighting.hlsl 의 numthreads(8, 8, 1)
		ComPtr<ID3D12RootSignature> tonemapRootSig;
		ComPtr<ID3D12PipelineState> tonemapPso;
		bool resolved = false;                        // 이번 프레임에 HDR → 백버퍼를 이미 옮겼나

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
			uint32_t pso = 0;                  // 0 기본 · 1 양면 · 2 반전 · 3 반전 양면, +4 알파 마스크, 8 · 9 지형 레이어(kTerrainPsoKey)
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

		// ── [0] 가속 구조 갱신 — 비동기 컴퓨트 큐 (2026-10-27 렌더 3단계) ──
		//  TLAS 빌드를 그래픽(직접) 큐가 아니라 따로 둔 컴퓨트 큐에 보내, 직접 큐의 컬링 · G-buffer 와 GPU 에서 겹쳐 돌린다.
		//  직접 큐는 [4a] RT 그림자 직전에 asFence 를 GPU 쪽에서 기다린다(JoinAsyncTlasBuild). CPU 는 기다리지 않는다.
		//  큐를 못 만들면 asyncAs = false — 예전처럼 직접 커맨드 리스트에서 빌드한다.
		bool                               asyncAs = false;
		ComPtr<ID3D12CommandQueue>         computeQueue;
		ComPtr<ID3D12CommandAllocator>     computeAllocator;
		ComPtr<ID3D12GraphicsCommandList4> computeList;
		ComPtr<ID3D12Fence>                asFence;
		UINT64                             asFenceValue = 0;
		bool                               asPending = false;   // 컴퓨트 큐로 보냈고, 직접 큐가 아직 기다리지 않았다
		// 정규화 필수 — GGX 의 H = normalize(L+V) 계산에 들어간다
		XMFLOAT3 sunDir{ 0.3563f, -0.8144f, 0.4581f };
		uint32_t debugMode = 0;
		uint32_t frameCounter = 0;
		std::wstring status;
		RenderStats stats;

		// ── GPU 시간 계측 (2026-10-28) — 직접 큐의 패스 경계마다 타임스탬프를 찍고, 2초마다 구간별 평균을 디버그 출력으로 낸다 ──
		//  구간 = 한 표시(MarkGpuTime)에서 다음 표시까지. 같은 이름(배경 · 메인 Render 의 같은 자리)은 합친다.
		//  «[0] TLAS 대기» 구간에는 직접 큐가 컴퓨트 큐의 TLAS 빌드를 기다린 시간이 들어간다.
		//  EndFrame 이 GPU 완료를 기다리므로 같은 프레임 끝에서 바로 읽는다.
		static constexpr UINT kMaxTimestamps = 64;
		ComPtr<ID3D12QueryHeap> timestampHeap;
		ComPtr<ID3D12Resource>  timestampReadback;
		UINT64         timestampFrequency = 0;
		UINT           timestampCount = 0;
		const wchar_t* timestampNames[kMaxTimestamps] = {};
		std::vector<std::pair<std::wstring, double>> gpuTimeSums;   // 구간 이름 → 누적 ms (처음 나온 순서)
		double         gpuFrameSum = 0.0;
		uint32_t       gpuTimedFrames = 0;
		LARGE_INTEGER  gpuReportStart = {};

		void MarkGpuTime(const wchar_t* name)
		{
			if (!timestampHeap || timestampCount >= kMaxTimestamps) return;
			timestampNames[timestampCount] = name;
			commandList->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestampCount++);
		}

		void CollectGpuTimes(UINT count)
		{
			if (!timestampHeap || count < 2 || timestampFrequency == 0) return;
			D3D12_RANGE range = { 0, count * sizeof(UINT64) };
			void* mapped = nullptr;
			if (FAILED(timestampReadback->Map(0, &range, &mapped))) return;
			const UINT64* t = static_cast<const UINT64*>(mapped);
			const double toMs = 1000.0 / double(timestampFrequency);
			for (UINT i = 0; i + 1 < count; ++i)
			{
				const double ms = t[i + 1] >= t[i] ? double(t[i + 1] - t[i]) * toMs : 0.0;
				auto it = std::find_if(gpuTimeSums.begin(), gpuTimeSums.end(),
					[&](const std::pair<std::wstring, double>& e) { return e.first == timestampNames[i]; });
				if (it == gpuTimeSums.end()) gpuTimeSums.emplace_back(timestampNames[i], ms);
				else it->second += ms;
			}
			gpuFrameSum += t[count - 1] >= t[0] ? double(t[count - 1] - t[0]) * toMs : 0.0;
			++gpuTimedFrames;
			D3D12_RANGE noWrite = { 0, 0 };
			timestampReadback->Unmap(0, &noWrite);

			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			if (gpuReportStart.QuadPart == 0) gpuReportStart = now;
			if (double(now.QuadPart - gpuReportStart.QuadPart) / double(frequency.QuadPart) < 2.0) return;

			wchar_t line[96];
			swprintf_s(line, L"[GPU 시간] %u 프레임 평균 — 전체 %.3f ms", gpuTimedFrames, gpuFrameSum / gpuTimedFrames);
			std::wstring report = line;
			for (const std::pair<std::wstring, double>& e : gpuTimeSums)
			{
				swprintf_s(line, L" | %s %.3f", e.first.c_str(), e.second / gpuTimedFrames);
				report += line;
			}
			report += L"\n";
			OutputDebugStringW(report.c_str());
			gpuTimeSums.clear();
			gpuFrameSum = 0.0;
			gpuTimedFrames = 0;
			gpuReportStart = now;
		}

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

		// ── 렌더 타깃 상태 전환 (2026-10-27) ──
		//  상태가 이미 같은 것은 건너뛰고, 바뀌는 것만 모아 ResourceBarrier 한 번으로 보낸다.
		//  각 RenderTarget 이 «지금 상태» 를 들고 있으므로 패스는 «무슨 상태가 필요한지» 만 말하면 된다.
		void TransitionTargets(std::initializer_list<std::pair<RenderTarget*, D3D12_RESOURCE_STATES>> wanted)
		{
			D3D12_RESOURCE_BARRIER barriers[8] = {};
			UINT count = 0;
			for (const auto& [target, after] : wanted)
			{
				if (target->state == after) continue;
				if (count == _countof(barriers))
				{
					commandList->ResourceBarrier(count, barriers);
					count = 0;
				}
				barriers[count++] = Transition(target->resource.Get(), target->state, after);
				target->state = after;
			}
			if (count > 0)
				commandList->ResourceBarrier(count, barriers);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += SIZE_T(index) * rtvDescriptorSize;
			return handle;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return dsvHeap->GetCPUDescriptorHandleForHeapStart(); }
		D3D12_CPU_DESCRIPTOR_HANDLE TargetCpu(UINT slot) const
		{
			D3D12_CPU_DESCRIPTOR_HANDLE handle = targetHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += SIZE_T(slot) * targetDescriptorSize;
			return handle;
		}
		D3D12_GPU_DESCRIPTOR_HANDLE TargetGpu(UINT slot) const
		{
			D3D12_GPU_DESCRIPTOR_HANDLE handle = targetHeap->GetGPUDescriptorHandleForHeapStart();
			handle.ptr += UINT64(slot) * targetDescriptorSize;
			return handle;
		}

		// ── 래스터 묶음 그리기 (2026-10-27 Render 에서 떼어 냄) ──
		//  배경 forward 와 메인 G-buffer 가 같은 묶음 · 같은 루프를 쓰고 PSO 표만 다르다.
		//  루트 시그니처 · 힙 · 프레임 CBV · 인스턴스 SRV 는 부르기 전에 묶어 둔다.
		// 재질 하나의 텍스처 5칸(재질 힙 materialIndex 번째 묶음)에 SRV 를 쓰고 그 묶음의 GPU 핸들을 돌려준다.
		//  SRV 는 텍스처의 밉 수 · 배열 여부 그대로(TextureSrvDesc). CreateMaterial · CreateTerrainMaterial 이 같이 쓴다(2026-10-28).
		D3D12_GPU_DESCRIPTOR_HANDLE WriteMaterialTextures(size_t materialIndex,
			const std::array<TextureHandle, kMaterialTextureCount>& slots)
		{
			const size_t descriptorOffset = materialIndex * kMaterialTextureCount * materialDescriptorSize;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = materialHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += descriptorOffset;
			D3D12_GPU_DESCRIPTOR_HANDLE gpu = materialHeap->GetGPUDescriptorHandleForHeapStart();
			gpu.ptr += descriptorOffset;
			for (size_t slot = 0; slot < kMaterialTextureCount; ++slot)
			{
				const TextureGpu& texture = textures[slots[slot]];
				const D3D12_SHADER_RESOURCE_VIEW_DESC srv = TextureSrvDesc(texture);
				device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
				cpu.ptr += materialDescriptorSize;
			}
			return gpu;
		}

		void DrawBatches(const PassGpu& pass, ID3D12PipelineState* const (&psoByKey)[kPsoKeyCount])
		{
			ID3D12PipelineState* currentPso = nullptr;
			MaterialHandle currentMaterial = kInvalidMaterial;
			for (size_t b = 0; b < pass.batches.size(); ++b)
			{
				const Batch& batch = pass.batches[b];
				ID3D12PipelineState* selectedPso = psoByKey[batch.pso];
				if (currentPso != selectedPso)
				{
					commandList->SetPipelineState(selectedPso);
					currentPso = selectedPso;
				}
				if (currentMaterial != batch.material)
				{
					const MaterialGpu& material = materials[batch.material];
					if (material.terrain)   // 2026-10-28 4단계 — 지형은 20 DWORD, 일반 재질은 앞 12 DWORD
						commandList->SetGraphicsRoot32BitConstants(2, kMaterialRootConstants, &material.terrainConstants, 0);
					else
						commandList->SetGraphicsRoot32BitConstants(2, sizeof(MaterialConstants) / sizeof(uint32_t), &material.constants, 0);
					commandList->SetGraphicsRootDescriptorTable(3, material.textures);
					currentMaterial = batch.material;
				}
				const MeshGpu& m = meshes[batch.mesh];
				commandList->IASetVertexBuffers(0, 1, &m.vbv);
				commandList->IASetIndexBuffer(&m.ibv);
				// 그리기에 넘기는 것은 «이 묶음의 보이는 목록 시작 번호» 하나다(수정방향 3.2).
				commandList->SetGraphicsRoot32BitConstant(0, batch.first, 0);
				commandList->ExecuteIndirect(drawSignature.Get(), 1, pass.args.Get(),
					UINT64(b) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
				++stats.drawCalls;
			}
		}

		// ── [0] TLAS 빌드를 비동기 컴퓨트 큐로 보낸다 (2026-10-27 렌더 3단계) ──
		//  인스턴스 디스크립터(UPLOAD)는 부르기 전에 이번 프레임 값으로 채워 둔다(ResetInstances · AddInstance).
		//  실패하면 false — 부른 쪽이 직접 커맨드 리스트에서 빌드한다.
		bool SubmitTlasBuildAsync()
		{
			// 지난 프레임의 컴퓨트 작업은 끝나 있어야 할당자를 Reset 할 수 있다.
			//  직접 큐가 그 펜스를 기다렸고 프레임 끝에서 직접 큐를 기다렸으므로 보통은 바로 통과한다(안전장치).
			if (asFence->GetCompletedValue() < asFenceValue)
			{
				if (FAILED(asFence->SetEventOnCompletion(asFenceValue, fenceEvent)) ||
					WaitForSingleObject(fenceEvent, INFINITE) != WAIT_OBJECT_0)
					return false;
			}
			if (FAILED(computeAllocator->Reset()) ||
				FAILED(computeList->Reset(computeAllocator.Get(), nullptr)))
				return false;
			accel.BuildTlas(computeList.Get());   // 빌드 + UAV 배리어(같은 컴퓨트 리스트 안)
			if (FAILED(computeList->Close()))
				return false;
			ID3D12CommandList* lists[] = { computeList.Get() };
			computeQueue->ExecuteCommandLists(1, lists);
			if (FAILED(computeQueue->Signal(asFence.Get(), ++asFenceValue)))
				return false;
			asPending = true;
			return true;
		}

		// ── [0] 의 짝 — 직접 큐를 여기서 한 번 끊는다 (2026-10-27 렌더 3단계) ──
		//  앞부분(프레임 시작 · 컬링 · G-buffer)을 지금 GPU 에 보내 컴퓨트 큐의 TLAS 빌드와 같이 돌게 하고,
		//  뒷부분(TLAS 를 읽는 [4a] RT 그림자부터)은 asFence 를 GPU 쪽에서 기다린 뒤 돌게 한다.
		//  ★ 커맨드 리스트는 실행 중에도 Reset 할 수 있고 같은 할당자를 다시 써도 된다(MS 문서). 할당자 Reset 은 프레임 시작에서만.
		//  ★ Reset 은 묶어 둔 상태(루트 · 힙 · 타깃 · 뷰포트)를 다 푼다. 뒤 패스는 루트 · 힙 · 타깃을 각자 묶으므로 뷰포트 · 가위만 다시 건다.
		//  ★ 버퍼는 ExecuteCommandLists 가 끝나면 COMMON 으로 돌아가지만, 명시적으로 상태를 바꾼 텍스처는 그대로다
		//    → RenderTarget 들의 상태 추적은 끊긴 뒤에도 그대로 맞다.
		void JoinAsyncTlasBuild()
		{
			if (!asPending) return;
			asPending = false;

			commandList->Close();
			ID3D12CommandList* lists[] = { commandList.Get() };
			commandQueue->ExecuteCommandLists(1, lists);
			commandQueue->Wait(asFence.Get(), asFenceValue);   // GPU 쪽 대기 — CPU 는 멈추지 않는다
			commandList->Reset(commandAllocator.Get(), nullptr);

			const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
			const D3D12_RECT scissor = { 0, 0, LONG(width), LONG(height) };
			commandList->RSSetViewports(1, &vp);
			commandList->RSSetScissorRects(1, &scissor);
		}

		// 컴퓨트 큐에 보낸 마지막 TLAS 빌드가 끝날 때까지 CPU 가 기다린다(종료할 때).
		void WaitForCompute()
		{
			if (!asFence || asFence->GetCompletedValue() >= asFenceValue) return;
			if (SUCCEEDED(asFence->SetEventOnCompletion(asFenceValue, fenceEvent)))
				WaitForSingleObject(fenceEvent, INFINITE);
		}

		// ── [6] 해 · 하늘 (2026-10-27 렌더 6단계) — 프레임의 첫 Render(배경이든 메인이든)에서 한 번 ──
		//  ① 땅에 닿는 태양 조도 = 대기 위 조도 × 해 쪽 대기 투과율 (카메라 자리의 위 기준 — 행성 위치마다 해 높이가 다르다)
		//  ② 해 · 위가 바뀌었으면 하늘 텍스처 → 하늘 조도 텍스처를 다시 만든다 (Sky.hlsl CSGenerateSky · CSSkyIrradiance)
		//  ③ HDR 컬러 전체에 하늘 + 해 원반을 쓴다 (CSDrawSky). 그 위에 배경 행성 · 메인 라이팅이 덮어 그린다.
		//  바인드리스가 없어 하늘 파이프라인을 못 만든 장치는 ① 만 하고, 하늘은 BeginFrame 의 지우기 색이 남는다.
		static constexpr D3D12_RESOURCE_STATES kSkyReadState =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		void UpdateSunAndSky(const RenderView& view)
		{
			if (skyDoneThisFrame) return;
			skyDoneThisFrame = true;

			// ① 태양 조도
			//  위쪽 = 행성 기준(메인 화면의 위쪽). 배경 패스가 먼저면 직전 프레임 메인 화면 값(위쪽은 걸을 때 천천히 바뀐다).
			//  메인 화면을 아직 한 번도 안 그렸으면 이 view 의 값.
			const XMFLOAT3 upSource = (view.background && hasMainViewUp) ? mainViewUp : view.up;
			XMVECTOR up = XMLoadFloat3(&upSource);
			up = XMVectorGetX(XMVector3LengthSq(up)) > 1.0e-12f ? XMVector3Normalize(up) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			const XMVECTOR toSun = XMVector3Normalize(XMVectorNegate(XMLoadFloat3(&sunDir)));
			const float muS = XMVectorGetX(XMVector3Dot(up, toSun));
			const XMFLOAT3 transmittance = SunTransmittance(muS, kSkyObserverHeight);
			sunIlluminance = XMFLOAT3(kSunIlluminanceTop * transmittance.x,
				kSunIlluminanceTop * transmittance.y, kSunIlluminanceTop * transmittance.z);

			if (!skyDrawPso || !bindlessHeap) return;

			XMFLOAT3 upF, toSunF;
			XMStoreFloat3(&upF, up);
			XMStoreFloat3(&toSunF, toSun);

			SkyConstants sc = {};
			XMStoreFloat4x4(&sc.invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&view.viewProj))));
			sc.eyePos = view.eyePosition;
			sc.sunIlluminanceTop = kSunIlluminanceTop;
			sc.toSun = toSunF;
			sc.cosSunRadius = std::cos(kSunAngularRadius);
			sc.up = upF;
			sc.observerHeight = kSkyObserverHeight;
			// 해 원반 휘도 = 땅에 닿는 조도 ÷ 원반 입체각 2π(1 − cos r)
			const float sunSolidAngle = 2.0f * 3.14159265f * (1.0f - std::cos(kSunAngularRadius));
			sc.sunDiskRadiance = XMFLOAT3(sunIlluminance.x / sunSolidAngle, sunIlluminance.y / sunSolidAngle, sunIlluminance.z / sunSolidAngle);
			sc.hdrUav = BindlessHdrUav;
			sc.width = width;
			sc.height = height;
			sc.skyWidth = kSkyWidth;
			sc.skyHeight = kSkyHeight;
			sc.irradianceWidth = kSkyIrradianceWidth;
			sc.irradianceHeight = kSkyIrradianceHeight;
			sc.skyUav = BindlessSkyUav;
			sc.skySrv = BindlessSkySrv;
			sc.irradianceUav = BindlessSkyIrradianceUav;
			sc.msUav = BindlessSkyMsUav;
			sc.msSrv = BindlessSkyMsSrv;
			sc.msSize = kSkyMsSize;
			memcpy(skyCBData, &sc, sizeof(sc));

			// 힙을 먼저, 루트 시그니처를 나중에 (CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED 규칙)
			ID3D12DescriptorHeap* heaps[] = { bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, heaps);
			commandList->SetComputeRootSignature(computeRootSig.Get());
			commandList->SetComputeRootConstantBufferView(0, skyCB->GetGPUVirtualAddress());

			// ⓪ 다중 산란 LUT — 대기 값만으로 정해지므로 처음 한 번
			if (!skyMsValid)
			{
				TransitionTargets({ { &skyMultiScattering, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
				commandList->SetPipelineState(skyMultiScatteringPso.Get());
				commandList->Dispatch((kSkyMsSize + 7) / 8, (kSkyMsSize + 7) / 8, 1);
				TransitionTargets({ { &skyMultiScattering, kSkyReadState } });
				skyMsValid = true;
				skyValid = false;   // LUT 가 새로 생겼으니 하늘도 다시
			}

			// ② 해 · 위가 바뀔 때만 하늘 텍스처를 다시 만든다 (방향 차이 1e-4 넘게)
			auto differs = [](const XMFLOAT3& a, const XMFLOAT3& b)
			{
				return std::fabs(a.x - b.x) > 1.0e-4f || std::fabs(a.y - b.y) > 1.0e-4f || std::fabs(a.z - b.z) > 1.0e-4f;
			};
			if (!skyValid || differs(upF, skyBuiltUp) || differs(sunDir, skyBuiltSunDir))
			{
				TransitionTargets({ { &skyTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
				commandList->SetPipelineState(skyGeneratePso.Get());
				commandList->Dispatch((kSkyWidth + 7) / 8, (kSkyHeight + 7) / 8, 1);
				TransitionTargets({
					{ &skyTexture,    kSkyReadState },
					{ &skyIrradiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
				commandList->SetPipelineState(skyIrradiancePso.Get());
				commandList->Dispatch((kSkyIrradianceWidth + 7) / 8, (kSkyIrradianceHeight + 7) / 8, 1);
				TransitionTargets({ { &skyIrradiance, kSkyReadState } });
				skyValid = true;
				skyBuiltUp = upF;
				skyBuiltSunDir = sunDir;
			}

			// ③ 화면에 하늘 — HDR 을 UAV 로 쓰고 다시 그리기 상태로 돌려 둔다(뒤 패스가 RTV 로 덮어 그린다).
			TransitionTargets({ { &hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			commandList->SetPipelineState(skyDrawPso.Get());
			commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			TransitionTargets({ { &hdr, D3D12_RESOURCE_STATE_RENDER_TARGET } });
		}

		// ── [4c] GI 프로브 갱신 (2026-10-27 렌더 7단계) — 메인 패스, TLAS 빌드가 끝난 뒤 · [4a] 그림자 앞 ──
		//  ① 격자: 카메라가 있는 칸을 가운데로 두고 칸 단위로 스크롤한다.
		//  ② 이번 프레임 1/4 프로브: 추적(CSProbeTrace) → 거리 섞기 → 조도 섞기 + 상태 (ProbeGI.hlsl)
		//  끝나면 probeVolume.enabled = 1 — 라이팅 · 반사가 이 격자를 주변광으로 읽는다.
		//  ★ 2026-10-28 두 겹 — 겹 0(가까운 겹 4 m) · 겹 1(먼 겹 16 m)에 차례로 ② 를 돌린다.
		//    두 겹 다 카메라 칸을 가운데로 스크롤하고, 아틀라스 · 상태 버퍼는 같이 쓴다(먼 겹은 kProbeCount 번부터).
		//    상수는 겹마다 giCB 의 512 B 칸 하나 — 두 겹 정보를 다 넣는다(맞은 점의 다중 반사가 두 겹을 차례로 읽는다).
		//  ★ 2026-10-28 지면에 맞춘 격자 — 짧은 축(격자 Y) = 지면 위쪽. 예전에는 위쪽에 가장 가까운 «월드 축» 이라
		//    행성 위에서 36 ~ 47° 비스듬했다(앞쪽 약 30 m 에서 GI 가 끊김). 격자 칸 = floor(dot(위치, 축) / 간격).
		ProbeVolumeGpu MakeProbeVolume(int layer, const int32_t dims[3], float spacing, const XMFLOAT3& eyePosition) const
		{
			const XMVECTOR eye = XMLoadFloat3(&eyePosition);
			ProbeVolumeGpu volume = {};
			float* axisOut[3] = { volume.axisX, volume.axisY, volume.axisZ };
			for (int a = 0; a < 3; ++a)
			{
				const float coord = XMVectorGetX(XMVector3Dot(eye, XMLoadFloat3(&probeAxes[a])));   // 격자 좌표(m)
				volume.baseCell[a] = int32_t(std::floor(coord / spacing)) - dims[a] / 2;
				volume.dims[a] = dims[a];
				axisOut[a][0] = probeAxes[a].x; axisOut[a][1] = probeAxes[a].y; axisOut[a][2] = probeAxes[a].z; axisOut[a][3] = 0.0f;
			}
			volume.generation = probeLayers[layer].generation;
			volume.spacing = spacing;
			volume.normalBias = kProbeNormalBias;
			volume.viewBias = kProbeViewBias;
			volume.probesPerRow = kProbesPerRow;
			volume.enabled = 1;
			volume.irradianceAtlasSize[0] = float(kProbesPerRow * kProbeIrradianceTexels);
			volume.irradianceAtlasSize[1] = float(kProbeTotalCount / kProbesPerRow * kProbeIrradianceTexels);
			volume.distanceAtlasSize[0] = float(kProbesPerRow * kProbeDistanceTexels);
			volume.distanceAtlasSize[1] = float(kProbeTotalCount / kProbesPerRow * kProbeDistanceTexels);
			volume.storageOffset = (layer == 0) ? 0u : kProbeCount;
			return volume;
		}

		void UpdateProbes(const RenderView& view)
		{
			// ① 격자 축 — 처음이거나 지면 위쪽이 격자 Y 축에서 kProbeReorientDegrees 넘게 기울면 다시 맞춘다.
			//  Y = 위쪽, X = 위쪽과 월드 Z(위쪽이 Z 에 가까우면 월드 X)의 외적, Z = X × Y. 다시 맞추면 두 겹 다 세대 +1(새로 채움).
			XMVECTOR up = XMLoadFloat3(&view.up);
			up = XMVectorGetX(XMVector3LengthSq(up)) > 1.0e-12f ? XMVector3Normalize(up) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			const float reorientCos = std::cos(kProbeReorientDegrees * 3.14159265f / 180.0f);
			if (!probeAxesValid || XMVectorGetX(XMVector3Dot(up, XMLoadFloat3(&probeAxes[1]))) < reorientCos)
			{
				const XMVECTOR ref = std::fabs(XMVectorGetZ(up)) > 0.9f ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f) : XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
				const XMVECTOR axisX = XMVector3Normalize(XMVector3Cross(up, ref));
				const XMVECTOR axisZ = XMVector3Cross(axisX, up);
				XMStoreFloat3(&probeAxes[0], axisX);
				XMStoreFloat3(&probeAxes[1], up);
				XMStoreFloat3(&probeAxes[2], axisZ);
				probeAxesValid = true;
				for (ProbeLayer& L : probeLayers)
				{
					++L.generation;   // 칸 배치(축)가 바뀌었다 — 옛 데이터는 상태 검사에서 빠진다
					L.cursor = 0;
				}
			}
			const int32_t dims[3] = { kProbeGridLong, kProbeGridShort, kProbeGridLong };   // 격자 Y(위쪽)가 짧은 축
			const float layerSpacing[kProbeLayerCount] = { kProbeSpacing, kProbeFarSpacing };
			const UINT  layerCount[kProbeLayerCount] = { kProbeCount, kProbeFarCount };
			ProbeVolumeGpu volumes[kProbeLayerCount];
			for (int layer = 0; layer < kProbeLayerCount; ++layer)
				volumes[layer] = MakeProbeVolume(layer, dims, layerSpacing[layer], view.eyePosition);

			// ② 상수 — 레이 방향은 프레임마다 무작위 회전(고정 방향 줄무늬 방지, 설계 문서 6.4)
			auto hash = [](uint32_t v)   // PCG (RtCommon.hlsli PcgHash 와 같은 식)
			{
				const uint32_t state = v * 747796405u + 2891336453u;
				const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
				return (word >> 22u) ^ word;
			};
			const float twoPi = 6.28318531f;
			const uint32_t h0 = hash(frameCounter), h1 = hash(h0), h2 = hash(h1);
			const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
				float(h0 >> 8) * (1.0f / 16777216.0f) * twoPi, float(h1 >> 8) * (1.0f / 16777216.0f) * twoPi,
				float(h2 >> 8) * (1.0f / 16777216.0f) * twoPi);
			XMFLOAT4X4 rot;
			XMStoreFloat4x4(&rot, rotation);

			ID3D12DescriptorHeap* heaps[] = { bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, heaps);
			commandList->SetComputeRootSignature(computeRootSig.Get());
			commandList->SetComputeRootShaderResourceView(1, accel.TlasAddress());
			commandList->SetComputeRootShaderResourceView(2, rtMaterials->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(3, rtInstances->GetGPUVirtualAddress() +
				UINT64(frameIndex % AccelStructure::kFrameSlots) * MaxInstances * sizeof(RtInstanceGpu));

			for (int layer = 0; layer < kProbeLayerCount; ++layer)
			{
				ProbeLayer& L = probeLayers[layer];
				GiConstants gc = {};
				gc.nearVolume = volumes[0];
				gc.farVolume = volumes[1];
				gc.rayRotation[0] = XMFLOAT4(rot._11, rot._12, rot._13, 0.0f);
				gc.rayRotation[1] = XMFLOAT4(rot._21, rot._22, rot._23, 0.0f);
				gc.rayRotation[2] = XMFLOAT4(rot._31, rot._32, rot._33, 0.0f);
				gc.sunDir = sunDir;
				gc.firstProbe = L.cursor;
				gc.sunIlluminance = sunIlluminance;
				gc.probeCount = layerCount[layer];
				gc.rayDataUav = BindlessProbeRayDataUav;       gc.rayDataSrv = BindlessProbeRayDataSrv;
				gc.irradianceUav = BindlessProbeIrradianceUav; gc.irradianceSrv = BindlessProbeIrradianceSrv;
				gc.distanceUav = BindlessProbeDistanceUav;     gc.distanceSrv = BindlessProbeDistanceSrv;
				gc.stateUav = BindlessProbeStateUav;           gc.stateSrv = BindlessProbeStateSrv;
				gc.skySrv = BindlessSkySrv;
				gc.skyIrradianceSrv = BindlessSkyIrradianceSrv;
				gc.probesThisFrame = kProbesPerFrame;
				gc.maxDistance = kProbeMaxDistance;
				gc.probeCycle = L.cycle;   // 2026-10-28 꺼 둔(비활성) 프로브는 이 바퀴 번호로 8 바퀴에 한 번만 다시 잰다
				gc.layer = uint32_t(layer);
				memcpy(giCBData + size_t(layer) * kGiCBSlot, &gc, sizeof(gc));
				L.cursor = (L.cursor + kProbesPerFrame) % layerCount[layer];
				if (L.cursor == 0)
					++L.cycle;   // 그 겹을 한 번 다 돌았다 (겹의 프로브 수는 kProbesPerFrame 의 배수)

				commandList->SetComputeRootConstantBufferView(0, giCB->GetGPUVirtualAddress() + UINT64(layer) * kGiCBSlot);

				// 추적 — 아틀라스 · 상태는 «읽기» (다중 반사: 맞은 점에서 지난 프로브를 읽는다)
				//  레이 데이터는 두 겹이 차례로 쓴다 — 앞 겹의 섞기가 다 읽은 뒤(읽기 → UAV 전이) 다음 겹이 쓴다.
				TransitionTargets({
					{ &probeRayData,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
					{ &probeIrradiance, kSkyReadState },
					{ &probeDistance,   kSkyReadState },
					{ &probeStates,     kSkyReadState } });
				commandList->SetPipelineState(giTracePso.Get());
				commandList->Dispatch(kProbesPerFrame, 1, 1);

				// 거리 섞기 — 상태는 읽기만 하지만 다음 단계와 같은 UAV 로 둔다
				TransitionTargets({
					{ &probeRayData,  kSkyReadState },
					{ &probeDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
					{ &probeStates,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
				commandList->SetPipelineState(giBlendDistancePso.Get());
				commandList->Dispatch(kProbesPerFrame, 1, 1);

				// 조도 섞기 + 상태 쓰기 (거리 섞기가 «옛 상태» 를 다 읽은 뒤에 쓴다 — 아래 UAV 배리어)
				D3D12_RESOURCE_BARRIER uavBarrier = {};
				uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				uavBarrier.UAV.pResource = probeStates.resource.Get();
				commandList->ResourceBarrier(1, &uavBarrier);
				TransitionTargets({
					{ &probeDistance,   kSkyReadState },
					{ &probeIrradiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
				commandList->SetPipelineState(giBlendIrradiancePso.Get());
				commandList->Dispatch(kProbesPerFrame, 1, 1);

				TransitionTargets({
					{ &probeIrradiance, kSkyReadState },
					{ &probeStates,     kSkyReadState } });
			}
			probeVolume = volumes[0];      // 라이팅 · 반사 · 반투명이 이번 프레임에 읽는다
			probeVolumeFar = volumes[1];
		}

		// ── [8] HDR 컬러 → 백버퍼 (2026-10-27) ──
		//  UI(DrawSprites)는 백버퍼 위에 덮어 그리므로 UI 전에 한 번 옮겨야 한다.
		//  UI 가 없는 프레임은 EndFrame 이 부른다. 한 프레임에 한 번만 돈다(resolved).
		//  지금은 톤 곡선 없이 0~1 로 자르기만 한다(Tonemap.hlsl) — 예전 화면과 같게.
		void ResolveToBackBuffer()
		{
			if (resolved) return;
			resolved = true;
			MarkGpuTime(L"[8] 후처리 · 톤맵 · UI");

			// ★ 10단계 — 최종 화면(디버그 뷰 0)은 [8] 후처리: 자동 노출 → 블룸 → 톤맵(ACES · sRGB).
			//   디버그 뷰 · 바인드리스 없는 장치는 예전처럼 0~1 자르기(값을 숫자 그대로 본다).
			const bool post = tonemapBindlessPso && debugMode == 0;
			if (post)
				RunPostProcess();

			TransitionTargets({ { &hdr, post ? kSkyReadState : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE } });
			const D3D12_RESOURCE_BARRIER toTarget = Transition(renderTargets[frameIndex].Get(),
				D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			commandList->ResourceBarrier(1, &toTarget);

			// 깊이는 묶지 않는다 — 톤맵 · UI PSO 는 DSV 형식이 UNKNOWN 이다.
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(frameIndex);
			commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			if (post)
			{
				ID3D12DescriptorHeap* heaps[] = { bindlessHeap.Get() };
				commandList->SetDescriptorHeaps(1, heaps);
				commandList->SetGraphicsRootSignature(tonemapBindlessRootSig.Get());
				commandList->SetPipelineState(tonemapBindlessPso.Get());
				TonemapConstants tc = {};
				tc.hdrIndex = BindlessHdrSrv;
				tc.bloomIndex = BindlessBloomUpSrv0;
				tc.exposureIndex = BindlessExposureSrv;
				tc.bloomStrength = kBloomStrength;
				tc.bloomEnabled = 1;
				tc.bloomScale = 1.0f / float(kBloomMips);
				commandList->SetGraphicsRoot32BitConstants(0, sizeof(tc) / sizeof(uint32_t), &tc, 0);
			}
			else
			{
				commandList->SetGraphicsRootSignature(tonemapRootSig.Get());
				commandList->SetPipelineState(tonemapPso.Get());
				ID3D12DescriptorHeap* heaps[] = { targetHeap.Get() };
				commandList->SetDescriptorHeaps(1, heaps);
				commandList->SetGraphicsRootDescriptorTable(0, TargetGpu(SlotHdrSrv));
			}
			commandList->DrawInstanced(3, 1, 0, 0);   // 화면 전체 삼각형 하나
		}

		// ── [8] 후처리 컴퓨트 (2026-10-27 렌더 10단계, PostProcess.hlsl) — ResolveToBackBuffer 가 톤맵 직전에 부른다 ──
		//  ① 히스토그램 → ② 노출(적응) → ③ 블룸 내리기 6단 → ④ 블룸 올리기 5단. 결과: 노출 버퍼 · 블룸 올리기 0단.
		//  블룸 밉마다 상태가 달라(원본은 읽기, 대상은 UAV) 하위 자원 배리어를 직접 건다 — 색 피라미드와 같은 방식.
		void SubresourceToRead(ID3D12Resource* resource, UINT mip)
		{
			const D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
				{ { resource, mip, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } } };
			commandList->ResourceBarrier(1, &barrier);
		}
		void RunPostProcess()
		{
			ID3D12DescriptorHeap* heaps[] = { bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, heaps);
			commandList->SetComputeRootSignature(computeRootSig.Get());

			UINT slot = 0;
			auto bindConstants = [&](const PostConstants& pc)
			{
				memcpy(postCBData + 256 * slot, &pc, sizeof(pc));
				commandList->SetComputeRootConstantBufferView(0, postCB->GetGPUVirtualAddress() + 256 * slot);
				++slot;
			};
			PostConstants base = {};
			base.histogramIndex = BindlessHistogramUav;
			base.exposureUav = BindlessExposureUav;
			base.deltaTime = frameDeltaSeconds;
			base.pixelCount = width * height;
			base.logMin = kExposureLogMin;
			base.logMax = kExposureLogMax;
			base.lowPercent = kExposureLowPercent;
			base.highPercent = kExposureHighPercent;
			base.minBrightness = kExposureMinBrightness;
			base.maxBrightness = kExposureMaxBrightness;
			base.speedUp = kExposureSpeedUp;
			base.speedDown = kExposureSpeedDown;
			base.hdrWidth = width;
			base.hdrHeight = height;
			base.frameIndex = frameCounter;

			// ① ② 자동 노출
			TransitionTargets({
				{ &hdr,            kSkyReadState },
				{ &histogram,      D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
				{ &exposureBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			PostConstants pc = base;
			pc.srcIndex = BindlessHdrSrv;
			bindConstants(pc);
			commandList->SetPipelineState(histogramPso.Get());
			commandList->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
			D3D12_RESOURCE_BARRIER uav = {};
			uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uav.UAV.pResource = histogram.resource.Get();
			commandList->ResourceBarrier(1, &uav);
			bindConstants(base);
			commandList->SetPipelineState(exposurePso.Get());
			commandList->Dispatch(1, 1, 1);
			TransitionTargets({ { &exposureBuffer, kSkyReadState } });

			// ③ 블룸 내리기 — 0 단은 HDR(전체 해상도)에서, 그 뒤는 윗 단계에서
			TransitionTargets({ { &bloomDown, D3D12_RESOURCE_STATE_UNORDERED_ACCESS }, { &bloomUp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			UINT mipWidth[kBloomMips], mipHeight[kBloomMips];
			for (UINT mip = 0; mip < kBloomMips; ++mip)
			{
				mipWidth[mip] = (std::max)(1u, (width / 2) >> mip);
				mipHeight[mip] = (std::max)(1u, (height / 2) >> mip);
			}
			commandList->SetPipelineState(bloomDownPso.Get());
			for (UINT mip = 0; mip < kBloomMips; ++mip)
			{
				pc = base;
				pc.srcIndex = mip == 0 ? BindlessHdrSrv : BindlessBloomDownSrv0 + mip - 1;
				pc.dstIndex = BindlessBloomDownUav0 + mip;
				pc.first = mip == 0 ? 1u : 0u;
				pc.dstWidth = mipWidth[mip];
				pc.dstHeight = mipHeight[mip];
				const UINT srcW = mip == 0 ? width : mipWidth[mip - 1];
				const UINT srcH = mip == 0 ? height : mipHeight[mip - 1];
				pc.srcTexelX = 1.0f / float(srcW);
				pc.srcTexelY = 1.0f / float(srcH);
				if (mip > 0)
					SubresourceToRead(bloomDown.resource.Get(), mip - 1);
				bindConstants(pc);
				commandList->Dispatch((mipWidth[mip] + 7) / 8, (mipHeight[mip] + 7) / 8, 1);
			}
			SubresourceToRead(bloomDown.resource.Get(), kBloomMips - 1);   // 이제 내리기 밉 전부 «읽기»

			// ④ 블룸 올리기 — 가장 작은 단(내리기 5단)부터: 올리기 i = 내리기 i + 텐트(올리기 i+1)
			commandList->SetPipelineState(bloomUpPso.Get());
			for (int mip = int(kBloomMips) - 2; mip >= 0; --mip)
			{
				pc = base;
				pc.srcIndex = (mip == int(kBloomMips) - 2) ? BindlessBloomDownSrv0 + kBloomMips - 1 : BindlessBloomUpSrv0 + mip + 1;
				pc.addIndex = BindlessBloomDownSrv0 + mip;
				pc.dstIndex = BindlessBloomUpUav0 + mip;
				pc.dstWidth = mipWidth[mip];
				pc.dstHeight = mipHeight[mip];
				pc.srcTexelX = 1.0f / float(mipWidth[mip + 1]);
				pc.srcTexelY = 1.0f / float(mipHeight[mip + 1]);
				bindConstants(pc);
				commandList->Dispatch((mipWidth[mip] + 7) / 8, (mipHeight[mip] + 7) / 8, 1);
				SubresourceToRead(bloomUp.resource.Get(), UINT(mip));
			}
			SubresourceToRead(bloomUp.resource.Get(), kBloomMips - 1);   // 쓰지 않은 마지막 단도 맞춰 둔다
			// 두 장 모두 밉 전부 NON_PIXEL — 상태 기록을 맞추고 «둘 다 읽기» 로(톤맵 픽셀 셰이더가 올리기 0단을 읽는다)
			bloomDown.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			bloomUp.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			TransitionTargets({ { &bloomDown, kSkyReadState }, { &bloomUp, kSkyReadState } });
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
			impl->WaitForCompute();   // [0] 컴퓨트 큐에 남은 TLAS 빌드 (보통은 이미 끝나 있다)
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
		// ★ 2026-10-27 2단계 — 임시 스위치 kEnableRaytracing 을 없앴다. 장치가 DXR 1.1 + SM 6.5 를 지원하면 RT 를 쓴다.
		//   (지원하지 않으면 BLAS · TLAS 를 만들지 않고, [4a] RT 그림자를 건너뛰며, 라이팅은 그림자 없이 합성한다)
		impl->rtSupported = pick.raytracing;

		impl->status = pick.name;
		if (impl->rtSupported)
			impl->status += L"  [DXR Tier 1.1]";
		else
			impl->status += L"  [RT 미지원 — 래스터만]";

		// ── SM 6.6 바인드리스 지원 확인 (2026-10-27 렌더 4단계) ──
		//  사양(HLSL SM 6.6 Dynamic Resources): ResourceDescriptorHeap 은 SM 6.6 과 바인딩 티어 3 을 둘 다 지원하는 장치에서만 된다.
		//  OS: SM 6.6 은 Windows 11 과 Agility SDK 에서 추가됐다(MS 문서) — 이 앱은 Agility SDK 를 쓰지 않으므로 Windows 10 에서는 보통 실패한다.
		//  하나라도 아니면 바인드리스 힙을 만들지 않고 [4b] 반사만 끈다(나머지는 그대로).
		{
			D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_6 };
			const bool smQueryOk = SUCCEEDED(impl->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));
			D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
			const bool optionsOk = SUCCEEDED(impl->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
			const bool sm66 = smQueryOk && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
			const bool tier3 = optionsOk && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
			impl->bindlessSupported = sm66 && tier3;

			wchar_t line[256];
			swprintf_s(line, L"[GRenderer] 셰이더 모델 최고 %d.%d · 리소스 바인딩 티어 %d → SM 6.6 바인드리스 %s\n",
				smQueryOk ? (shaderModel.HighestShaderModel >> 4) : 0, smQueryOk ? (shaderModel.HighestShaderModel & 0xF) : 0,
				optionsOk ? int(options.ResourceBindingTier) : 0, impl->bindlessSupported ? L"사용" : L"못 씀 — [4b] 반사 끔");
			OutputDebugStringW(line);
			if (!impl->bindlessSupported && impl->rtSupported)
				impl->status += L"  (SM 6.6 · 바인딩 티어 3 아님 — RT 반사 끔)";
		}

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
		rtvHeapDesc.NumDescriptors = Impl::kRtvCount;   // 백버퍼 2 + HDR 1 + G-buffer 5 (GB4 모션 벡터는 5단계부터)
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
		//  ★ 2026-10-27 — 자원을 R32_TYPELESS 로 만든다. 래스터는 D32_FLOAT DSV 로 쓰고,
		//    [5] 라이팅 컴퓨트는 R32_FLOAT SRV 로 읽어 픽셀의 월드 위치를 되살린다(같은 자원, 보기만 둘).
		{
			D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
			dsvHeapDesc.NumDescriptors = 2;   // 0 = 쓰기용, 1 = 읽기 전용(9단계 [7] 반투명 — 깊이 테스트하며 같은 깊이를 SRV 로 읽는다)
			dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			if (FAILED(impl->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&impl->dsvHeap))))
				return false;

			D3D12_RESOURCE_DESC depthDesc = {};
			depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			depthDesc.Width = width;
			depthDesc.Height = height;
			depthDesc.DepthOrArraySize = 1;
			depthDesc.MipLevels = 1;
			depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clearVal = {};
			clearVal.Format = DXGI_FORMAT_D32_FLOAT;
			clearVal.DepthStencil.Depth = 1.0f;

			D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&impl->depth.resource))))
				return false;
			impl->depth.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

			D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
			dsv.Format = DXGI_FORMAT_D32_FLOAT;
			dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			impl->device->CreateDepthStencilView(impl->depth.resource.Get(), &dsv,
				impl->dsvHeap->GetCPUDescriptorHandleForHeapStart());
			// 9단계 — 읽기 전용 DSV: 깊이 상태를 DEPTH_READ | 셰이더 읽기 로 두고 테스트만 한다(MS 문서 «read-only depth»)
			dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
			impl->device->CreateDepthStencilView(impl->depth.resource.Get(), &dsv, impl->ReadOnlyDsv());
		}

		// ── HDR 컬러 · G-buffer · 타깃 SRV/UAV 힙 (2026-10-27 파이프라인 1단계) ──
		//  HDR   : RTV(배경 forward) + UAV(라이팅 컴퓨트) + SRV(톤맵)
		//  GB0~4 : RTV([1] G-buffer 패스) + SRV([5] 라이팅 컴퓨트). GB4 모션 벡터는 5단계부터.
		//          G-buffer 는 지우지 않는다(깊이 1 인 픽셀은 뒤 단계가 읽지 않는다).
		//  최적화 지우기 값을 실제로 지우는 값과 맞춰 둔다(다르면 디버그 레이어 경고).
		{
			D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			auto createTarget = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, const float* clear,
				D3D12_RESOURCE_STATES initial, Impl::RenderTarget& out) -> bool
			{
				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				desc.Width = width;
				desc.Height = height;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.Format = format;
				desc.SampleDesc.Count = 1;
				desc.Flags = flags;
				D3D12_CLEAR_VALUE clearValue = {};
				clearValue.Format = format;
				for (int i = 0; i < 4; ++i) clearValue.Color[i] = clear[i];
				if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					initial,
					// 최적화 지우기 값은 렌더 타깃에만 줄 수 있다(UAV 전용 텍스처 — 그림자 마스크 — 에는 nullptr).
					(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clearValue : nullptr,
					IID_PPV_ARGS(&out.resource))))
					return false;
				out.state = initial;
				return true;
			};

			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (!createTarget(Impl::kHdrFormat,
					D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					Impl::kClearColor, D3D12_RESOURCE_STATE_RENDER_TARGET, impl->hdr))
			{
				impl->status = L"HDR 컬러 타깃 생성 실패.";
				return false;
			}
			for (UINT i = 0; i < Impl::GbCount; ++i)
			{
				if (!createTarget(Impl::kGBufferFormats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
						zero, D3D12_RESOURCE_STATE_RENDER_TARGET, impl->gbuffer[i]))
				{
					impl->status = L"G-buffer 타깃 생성 실패.";
					return false;
				}
			}

			// RTV — HDR 하나, G-buffer 5장(GB0~GB4, 연속)
			impl->device->CreateRenderTargetView(impl->hdr.resource.Get(), nullptr, impl->Rtv(Impl::kRtvHdr));
			for (UINT i = 0; i < Impl::GbCount; ++i)
				impl->device->CreateRenderTargetView(impl->gbuffer[i].resource.Get(), nullptr, impl->Rtv(Impl::kRtvGBuffer + i));

			D3D12_DESCRIPTOR_HEAP_DESC targetHeapDesc = {};
			targetHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			targetHeapDesc.NumDescriptors = Impl::TargetSlotCount;
			targetHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(impl->device->CreateDescriptorHeap(&targetHeapDesc, IID_PPV_ARGS(&impl->targetHeap))))
			{
				impl->status = L"렌더 타깃 디스크립터 힙 생성 실패.";
				return false;
			}
			impl->targetDescriptorSize = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			auto createSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT slot)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
				srv.Format = format;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				impl->device->CreateShaderResourceView(resource, &srv, impl->TargetCpu(slot));
			};
			for (UINT i = 0; i < Impl::GbCount; ++i)
				createSrv(impl->gbuffer[i].resource.Get(), Impl::kGBufferFormats[i], Impl::SlotGBuffer + i);
			createSrv(impl->depth.resource.Get(), DXGI_FORMAT_R32_FLOAT, Impl::SlotDepth);
			createSrv(impl->hdr.resource.Get(), Impl::kHdrFormat, Impl::SlotHdrSrv);

			D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
			uav.Format = Impl::kHdrFormat;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			impl->device->CreateUnorderedAccessView(impl->hdr.resource.Get(), nullptr, &uav, impl->TargetCpu(Impl::SlotHdrUav));

			// [4a] 그림자 마스크 (2026-10-27 렌더 2단계) — UAV(RtShadow.hlsl 가 씀) + SRV(Lighting.hlsl 이 읽음).
			//  처음 상태를 «읽기» 로 둔다 — RT 가 돌지 않는 프레임에도 라이팅 테이블이 유효한 자원을 가리키게.
			if (!createTarget(Impl::kShadowFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl->shadowMask))
			{
				impl->status = L"그림자 마스크 생성 실패.";
				return false;
			}
			createSrv(impl->shadowMask.resource.Get(), Impl::kShadowFormat, Impl::SlotShadowSrv);
			D3D12_UNORDERED_ACCESS_VIEW_DESC shadowUav = {};
			shadowUav.Format = Impl::kShadowFormat;
			shadowUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			impl->device->CreateUnorderedAccessView(impl->shadowMask.resource.Get(), nullptr, &shadowUav,
				impl->TargetCpu(Impl::SlotShadowUav));

			// [4b] 반사 텍스처 (2026-10-27 렌더 4단계) — 처음 상태 «읽기»: 반사가 안 도는 프레임에도 라이팅 테이블이 유효한 자원을 가리키게.
			if (!createTarget(Impl::kReflectionFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl->reflection))
			{
				impl->status = L"반사 텍스처 생성 실패.";
				return false;
			}
			// 5단계 — 라이팅 t7(슬롯 10)은 아래 [4d] 결과를 가리키고, 이 «노이즈 제거 전» 반사는 디버그용 t8(슬롯 11)이다.
			//  (GB4 모션 벡터 SRV 는 위 G-buffer 루프가 슬롯 4 에 만든다 — 라이팅 t4)
			createSrv(impl->reflection.resource.Get(), Impl::kReflectionFormat, Impl::SlotReflectionRawSrv);

			// [4d] 노이즈 제거 텍스처 (2026-10-27 렌더 5단계) — 전부 처음 상태 «읽기».
			//  이력 2장 · 결과: UAV(노이즈 제거 컴퓨트가 씀) + SRV. 결과는 라이팅 t7(슬롯 10).
			//  이전 깊이 · 법선 사본: 복사 대상이라 플래그 없음. 형식은 원본과 같아야 CopyResource 가 된다.
			for (Impl::RenderTarget& history : impl->reflectionHistory)
			{
				if (!createTarget(Impl::kReflectionFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
						zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, history))
				{
					impl->status = L"반사 이력 텍스처 생성 실패.";
					return false;
				}
			}
			if (!createTarget(Impl::kReflectionFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl->reflectionDenoised) ||
				!createTarget(DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_NONE,
					zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl->prevDepth) ||
				!createTarget(Impl::kGBufferFormats[Impl::GbNormal], D3D12_RESOURCE_FLAG_NONE,
					zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl->prevNormal))
			{
				impl->status = L"반사 노이즈 제거 텍스처 생성 실패.";
				return false;
			}
			createSrv(impl->reflectionDenoised.resource.Get(), Impl::kReflectionFormat, Impl::SlotReflectionSrv);

			// [6] 하늘 텍스처 · 하늘 조도 (2026-10-27 렌더 6단계) — 크기가 화면과 달라 따로 만든다.
			//  처음 상태 «읽기»: 라이팅 t9 가 항상 가리키므로(하늘을 못 만드는 장치는 gSkyEnabled 0 이라 읽지 않는다).
			auto createSized = [&](UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
				D3D12_RESOURCE_STATES initial, Impl::RenderTarget& out) -> bool
			{
				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				desc.Width = w;
				desc.Height = h;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.Format = format;
				desc.SampleDesc.Count = 1;
				desc.Flags = flags;
				if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
					IID_PPV_ARGS(&out.resource))))
					return false;
				out.state = initial;
				return true;
			};
			if (!createSized(Impl::kSkyWidth, Impl::kSkyHeight, Impl::kSkyFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					Impl::kSkyReadState, impl->skyTexture) ||
				!createSized(Impl::kSkyIrradianceWidth, Impl::kSkyIrradianceHeight, Impl::kSkyFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					Impl::kSkyReadState, impl->skyIrradiance) ||
				!createSized(Impl::kSkyMsSize, Impl::kSkyMsSize, Impl::kSkyFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					Impl::kSkyReadState, impl->skyMultiScattering))
			{
				impl->status = L"하늘 텍스처 생성 실패.";
				return false;
			}
			createSrv(impl->skyIrradiance.resource.Get(), Impl::kSkyFormat, Impl::SlotSkyIrradianceSrv);

			// [4c] GI 프로브 자원 (2026-10-27 렌더 7단계) — 처음 상태 «읽기»(라이팅 t10 ~ t12 가 항상 가리킨다).
			//  상태 버퍼는 0 으로 시작해야 한다(= 데이터 없음) — 기본 힙의 커밋 자원은 0 으로 채워져 나온다(CREATE_NOT_ZEROED 를 안 씀).
			{
				constexpr UINT probeRows = Impl::kProbeTotalCount / Impl::kProbesPerRow;   // 10-28 두 겹 — 64 → 320 줄
				const bool probeOk =
					createSized(Impl::kProbeRays, Impl::kProbesPerFrame, DXGI_FORMAT_R16G16B16A16_FLOAT,
						D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, Impl::kSkyReadState, impl->probeRayData) &&
					createSized(Impl::kProbesPerRow * Impl::kProbeIrradianceTexels, probeRows * Impl::kProbeIrradianceTexels,
						Impl::kProbeIrradianceFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, Impl::kSkyReadState, impl->probeIrradiance) &&
					createSized(Impl::kProbesPerRow * Impl::kProbeDistanceTexels, probeRows * Impl::kProbeDistanceTexels,
						Impl::kProbeDistanceFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, Impl::kSkyReadState, impl->probeDistance);
				D3D12_RESOURCE_DESC stateDesc = BufferDesc(UINT64(Impl::kProbeTotalCount) * 16);
				stateDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				// 버퍼는 만들 때 상태를 무시하고 COMMON 으로 시작한다(디버그 레이어 #1328) — 그래서 COMMON 으로 만들고 그렇게 적어 둔다.
				if (!probeOk || FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &stateDesc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&impl->probeStates.resource))))
				{
					impl->status = L"GI 프로브 자원 생성 실패.";
					return false;
				}
				impl->probeStates.state = D3D12_RESOURCE_STATE_COMMON;
				// [2] AO 텍스처 (2026-10-27 렌더 8단계) — 처음 상태 «읽기»(라이팅 t13 이 항상 가리킨다)
				for (Impl::RenderTarget* aoTarget : { &impl->aoRaw, &impl->aoHistory[0], &impl->aoHistory[1] })
				{
					if (!createSized(width, height, Impl::kAoRawFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
							Impl::kSkyReadState, *aoTarget))
					{
						impl->status = L"AO 텍스처 생성 실패.";
						return false;
					}
				}
				if (!createSized(width, height, Impl::kAoFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
						Impl::kSkyReadState, impl->aoOutput))
				{
					impl->status = L"AO 텍스처 생성 실패.";
					return false;
				}
				createSrv(impl->aoOutput.resource.Get(), Impl::kAoFormat, Impl::SlotAoSrv);

				// [7] 화면 색 피라미드 (2026-10-27 렌더 9단계) — 밉 7단, 처음 상태 «읽기»
				{
					D3D12_RESOURCE_DESC desc = {};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					desc.Width = width;
					desc.Height = height;
					desc.DepthOrArraySize = 1;
					desc.MipLevels = Impl::kSceneColorMips;
					desc.Format = Impl::kHdrFormat;
					desc.SampleDesc.Count = 1;
					desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
					if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, Impl::kSkyReadState, nullptr,
						IID_PPV_ARGS(&impl->sceneColor.resource))))
					{
						impl->status = L"화면 색 피라미드 생성 실패.";
						return false;
					}
					impl->sceneColor.state = Impl::kSkyReadState;

					// [8] 블룸 (2026-10-27 렌더 10단계) — 반 해상도, 밉 6단, 내리기 · 올리기 두 장. 처음 상태 «읽기»
					desc.Width = (std::max)(1u, width / 2);
					desc.Height = (std::max)(1u, height / 2);
					desc.MipLevels = Impl::kBloomMips;
					if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, Impl::kSkyReadState, nullptr,
							IID_PPV_ARGS(&impl->bloomDown.resource))) ||
						FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, Impl::kSkyReadState, nullptr,
							IID_PPV_ARGS(&impl->bloomUp.resource))))
					{
						impl->status = L"블룸 텍스처 생성 실패.";
						return false;
					}
					impl->bloomDown.state = impl->bloomUp.state = Impl::kSkyReadState;

					// [8] 자동 노출 버퍼 — 히스토그램 uint × 256(raw) · 노출 float4 × 1. 버퍼라 COMMON 으로 만든다(#1328)
					D3D12_RESOURCE_DESC histogramDesc = BufferDesc(256 * 4);
					histogramDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
					D3D12_RESOURCE_DESC exposureDesc = BufferDesc(16);
					exposureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
					if (FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &histogramDesc, D3D12_RESOURCE_STATE_COMMON,
							nullptr, IID_PPV_ARGS(&impl->histogram.resource))) ||
						FAILED(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &exposureDesc, D3D12_RESOURCE_STATE_COMMON,
							nullptr, IID_PPV_ARGS(&impl->exposureBuffer.resource))))
					{
						impl->status = L"자동 노출 버퍼 생성 실패.";
						return false;
					}
					impl->histogram.state = impl->exposureBuffer.state = D3D12_RESOURCE_STATE_COMMON;
				}

				createSrv(impl->probeIrradiance.resource.Get(), Impl::kProbeIrradianceFormat, Impl::SlotProbeIrradianceSrv);
				createSrv(impl->probeDistance.resource.Get(), Impl::kProbeDistanceFormat, Impl::SlotProbeDistanceSrv);
				D3D12_SHADER_RESOURCE_VIEW_DESC stateSrv = {};
				stateSrv.Format = DXGI_FORMAT_UNKNOWN;
				stateSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				stateSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				stateSrv.Buffer.NumElements = Impl::kProbeTotalCount;
				stateSrv.Buffer.StructureByteStride = 16;
				impl->device->CreateShaderResourceView(impl->probeStates.resource.Get(), &stateSrv, impl->TargetCpu(Impl::SlotProbeStateSrv));
			}

			// ── SM 6.6 바인드리스 힙 (2026-10-27 렌더 4단계) ──
			//  텍스처 · 메시가 만들어질 때(CreateTexture · CreateMesh) 칸을 받으므로 그보다 먼저 만든다.
			//  앞 16칸(BindlessSlot)은 RT 반사가 읽고 쓰는 프레임 자원: GB1 법선 · GB2 거칠기/금속 · 깊이 SRV, 반사 UAV.
			if (impl->bindlessSupported)
			{
				D3D12_DESCRIPTOR_HEAP_DESC bindlessDesc = {};
				bindlessDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				bindlessDesc.NumDescriptors = Impl::kBindlessCapacity;
				bindlessDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(impl->device->CreateDescriptorHeap(&bindlessDesc, IID_PPV_ARGS(&impl->bindlessHeap))))
				{
					impl->bindlessSupported = false;
					impl->status += L"  (바인드리스 힙 생성 실패 — RT 반사 끔)";
				}
				else
				{
					impl->bindlessDescriptorSize = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					auto bindlessSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t slot)
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
						srv.Format = format;
						srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
						srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						srv.Texture2D.MipLevels = 1;
						impl->device->CreateShaderResourceView(resource, &srv, impl->BindlessCpu(slot));
					};
					bindlessSrv(impl->gbuffer[Impl::GbNormal].resource.Get(), Impl::kGBufferFormats[Impl::GbNormal], Impl::BindlessGbNormal);
					bindlessSrv(impl->gbuffer[Impl::GbMaterial].resource.Get(), Impl::kGBufferFormats[Impl::GbMaterial], Impl::BindlessGbMaterial);
					bindlessSrv(impl->depth.resource.Get(), DXGI_FORMAT_R32_FLOAT, Impl::BindlessDepth);
					auto bindlessUav = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t slot)
					{
						D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
						uavDesc.Format = format;
						uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
						impl->device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, impl->BindlessCpu(slot));
					};
					bindlessUav(impl->reflection.resource.Get(), Impl::kReflectionFormat, Impl::BindlessReflectionUav);
					// 2026-10-28 — [4a] 그림자 마스크 (그림자를 바인드리스로 옮기면서)
					bindlessUav(impl->shadowMask.resource.Get(), Impl::kShadowFormat, Impl::BindlessShadowMaskUav);

					// 5단계 — [4d] 노이즈 제거가 읽고 쓰는 프레임 자원 (BindlessSlot 4 ~ 12)
					bindlessSrv(impl->reflection.resource.Get(), Impl::kReflectionFormat, Impl::BindlessReflectionSrv);
					bindlessSrv(impl->gbuffer[Impl::GbMotion].resource.Get(), Impl::kGBufferFormats[Impl::GbMotion], Impl::BindlessMotion);
					bindlessSrv(impl->prevDepth.resource.Get(), DXGI_FORMAT_R32_FLOAT, Impl::BindlessPrevDepth);
					bindlessSrv(impl->prevNormal.resource.Get(), Impl::kGBufferFormats[Impl::GbNormal], Impl::BindlessPrevNormal);
					bindlessSrv(impl->reflectionHistory[0].resource.Get(), Impl::kReflectionFormat, Impl::BindlessHistorySrv0);
					bindlessUav(impl->reflectionHistory[0].resource.Get(), Impl::kReflectionFormat, Impl::BindlessHistoryUav0);
					bindlessSrv(impl->reflectionHistory[1].resource.Get(), Impl::kReflectionFormat, Impl::BindlessHistorySrv1);
					bindlessUav(impl->reflectionHistory[1].resource.Get(), Impl::kReflectionFormat, Impl::BindlessHistoryUav1);
					bindlessUav(impl->reflectionDenoised.resource.Get(), Impl::kReflectionFormat, Impl::BindlessDenoisedUav);

					// 6단계 — [6] 하늘 (BindlessSlot 13 ~ 17)
					bindlessUav(impl->hdr.resource.Get(), Impl::kHdrFormat, Impl::BindlessHdrUav);
					bindlessSrv(impl->skyTexture.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkySrv);
					bindlessUav(impl->skyTexture.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkyUav);
					bindlessSrv(impl->skyIrradiance.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkyIrradianceSrv);
					bindlessUav(impl->skyIrradiance.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkyIrradianceUav);
					bindlessSrv(impl->skyMultiScattering.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkyMsSrv);
					bindlessUav(impl->skyMultiScattering.resource.Get(), Impl::kSkyFormat, Impl::BindlessSkyMsUav);

					// 9단계 — [7] 반투명 (BindlessSlot 36 ~ 51): HDR SRV, 피라미드 전체 SRV, 밉마다 SRV · UAV
					bindlessSrv(impl->hdr.resource.Get(), Impl::kHdrFormat, Impl::BindlessHdrSrv);
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
						srv.Format = Impl::kHdrFormat;
						srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
						srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						srv.Texture2D.MipLevels = Impl::kSceneColorMips;
						impl->device->CreateShaderResourceView(impl->sceneColor.resource.Get(), &srv, impl->BindlessCpu(Impl::BindlessSceneColorSrv));
						for (UINT mip = 0; mip < Impl::kSceneColorMips; ++mip)
						{
							srv.Texture2D.MostDetailedMip = mip;
							srv.Texture2D.MipLevels = 1;
							impl->device->CreateShaderResourceView(impl->sceneColor.resource.Get(), &srv,
								impl->BindlessCpu(Impl::BindlessSceneColorMipSrv0 + mip));
							D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
							uavDesc.Format = Impl::kHdrFormat;
							uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
							uavDesc.Texture2D.MipSlice = mip;
							impl->device->CreateUnorderedAccessView(impl->sceneColor.resource.Get(), nullptr, &uavDesc,
								impl->BindlessCpu(Impl::BindlessSceneColorMipUav0 + mip));
						}
					}

					// 10단계 — [8] 후처리 (BindlessSlot 52 ~ 78): 블룸 두 장의 밉마다 SRV · UAV, 히스토그램 UAV, 노출 SRV · UAV
					for (Impl::RenderTarget* bloomTarget : { &impl->bloomDown, &impl->bloomUp })
					{
						const uint32_t srvBase = bloomTarget == &impl->bloomDown ? Impl::BindlessBloomDownSrv0 : Impl::BindlessBloomUpSrv0;
						const uint32_t uavBase = bloomTarget == &impl->bloomDown ? Impl::BindlessBloomDownUav0 : Impl::BindlessBloomUpUav0;
						for (UINT mip = 0; mip < Impl::kBloomMips; ++mip)
						{
							D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
							srv.Format = Impl::kHdrFormat;
							srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
							srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
							srv.Texture2D.MostDetailedMip = mip;
							srv.Texture2D.MipLevels = 1;
							impl->device->CreateShaderResourceView(bloomTarget->resource.Get(), &srv, impl->BindlessCpu(srvBase + mip));
							D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
							uavDesc.Format = Impl::kHdrFormat;
							uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
							uavDesc.Texture2D.MipSlice = mip;
							impl->device->CreateUnorderedAccessView(bloomTarget->resource.Get(), nullptr, &uavDesc, impl->BindlessCpu(uavBase + mip));
						}
					}
					{
						D3D12_UNORDERED_ACCESS_VIEW_DESC rawUav = {};
						rawUav.Format = DXGI_FORMAT_R32_TYPELESS;
						rawUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
						rawUav.Buffer.NumElements = 256;
						rawUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
						impl->device->CreateUnorderedAccessView(impl->histogram.resource.Get(), nullptr, &rawUav, impl->BindlessCpu(Impl::BindlessHistogramUav));
						D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
						srv.Format = DXGI_FORMAT_UNKNOWN;
						srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
						srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						srv.Buffer.NumElements = 1;
						srv.Buffer.StructureByteStride = 16;
						impl->device->CreateShaderResourceView(impl->exposureBuffer.resource.Get(), &srv, impl->BindlessCpu(Impl::BindlessExposureSrv));
						D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
						uavDesc.Format = DXGI_FORMAT_UNKNOWN;
						uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
						uavDesc.Buffer.NumElements = 1;
						uavDesc.Buffer.StructureByteStride = 16;
						impl->device->CreateUnorderedAccessView(impl->exposureBuffer.resource.Get(), nullptr, &uavDesc, impl->BindlessCpu(Impl::BindlessExposureUav));
					}

					// 8단계 — [2] AO (BindlessSlot 28 ~ 35)
					bindlessSrv(impl->aoRaw.resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoRawSrv);
					bindlessUav(impl->aoRaw.resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoRawUav);
					bindlessSrv(impl->aoHistory[0].resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoHistorySrv0);
					bindlessUav(impl->aoHistory[0].resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoHistoryUav0);
					bindlessSrv(impl->aoHistory[1].resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoHistorySrv1);
					bindlessUav(impl->aoHistory[1].resource.Get(), Impl::kAoRawFormat, Impl::BindlessAoHistoryUav1);
					bindlessSrv(impl->aoOutput.resource.Get(), Impl::kAoFormat, Impl::BindlessAoOutputSrv);
					bindlessUav(impl->aoOutput.resource.Get(), Impl::kAoFormat, Impl::BindlessAoOutputUav);

					// 7단계 — [4c] GI 프로브 (BindlessSlot 20 ~ 27)
					bindlessSrv(impl->probeRayData.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, Impl::BindlessProbeRayDataSrv);
					bindlessUav(impl->probeRayData.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, Impl::BindlessProbeRayDataUav);
					bindlessSrv(impl->probeIrradiance.resource.Get(), Impl::kProbeIrradianceFormat, Impl::BindlessProbeIrradianceSrv);
					bindlessUav(impl->probeIrradiance.resource.Get(), Impl::kProbeIrradianceFormat, Impl::BindlessProbeIrradianceUav);
					bindlessSrv(impl->probeDistance.resource.Get(), Impl::kProbeDistanceFormat, Impl::BindlessProbeDistanceSrv);
					bindlessUav(impl->probeDistance.resource.Get(), Impl::kProbeDistanceFormat, Impl::BindlessProbeDistanceUav);
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
						srv.Format = DXGI_FORMAT_UNKNOWN;
						srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
						srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
						srv.Buffer.NumElements = Impl::kProbeTotalCount;
						srv.Buffer.StructureByteStride = 16;
						impl->device->CreateShaderResourceView(impl->probeStates.resource.Get(), &srv, impl->BindlessCpu(Impl::BindlessProbeStateSrv));
						D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
						uavDesc.Format = DXGI_FORMAT_UNKNOWN;
						uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
						uavDesc.Buffer.NumElements = Impl::kProbeTotalCount;
						uavDesc.Buffer.StructureByteStride = 16;
						impl->device->CreateUnorderedAccessView(impl->probeStates.resource.Get(), nullptr, &uavDesc, impl->BindlessCpu(Impl::BindlessProbeStateUav));
					}
				}
			}
		}

		if (FAILED(impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl->commandAllocator))))
			return false;
		if (FAILED(impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->commandAllocator.Get(), nullptr, IID_PPV_ARGS(&impl->commandList))))
			return false;
		impl->commandList->Close();

		// GPU 시간 계측 (2026-10-28) — 만들지 못하면 계측만 끈다(그리기와 상관없다)
		{
			D3D12_QUERY_HEAP_DESC qd = {};
			qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			qd.Count = Impl::kMaxTimestamps;
			D3D12_HEAP_PROPERTIES readbackHeap = HeapProps(D3D12_HEAP_TYPE_READBACK);
			const D3D12_RESOURCE_DESC rd = BufferDesc(Impl::kMaxTimestamps * sizeof(UINT64));
			if (FAILED(impl->commandQueue->GetTimestampFrequency(&impl->timestampFrequency)) ||
				FAILED(impl->device->CreateQueryHeap(&qd, IID_PPV_ARGS(&impl->timestampHeap))) ||
				FAILED(impl->device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rd,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&impl->timestampReadback))))
			{
				impl->timestampHeap.Reset();
				impl->timestampReadback.Reset();
			}
		}

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

		// [5] 라이팅 상수 버퍼 (UPLOAD, 영속 매핑, 2026-10-27) — CBV 는 256바이트 정렬. 10-28 GI 두 겹으로 304 B → 512 B
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC desc = BufferDesc(512);
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->lightingCB))) ||
				FAILED(impl->lightingCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->lightingCBData))))
			{
				impl->status = L"라이팅 상수 버퍼 생성 실패.";
				return false;
			}
		}

		// [4a] RT 그림자 상수 버퍼 (UPLOAD, 영속 매핑, 2026-10-27 렌더 2단계)
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC desc = BufferDesc(256);
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->shadowCB))) ||
				FAILED(impl->shadowCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->shadowCBData))))
			{
				impl->status = L"그림자 상수 버퍼 생성 실패.";
				return false;
			}
		}

		// RT 재질 표 (2026-10-27 렌더 2단계, 4단계에서 RtMaterialGpu 96 B 로 넓힘) — UPLOAD 영속 매핑.
		//  CreateMaterial 보다 먼저 있어야 한다(아래 기본 재질 0번도 여기에 쓴다).
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC desc = BufferDesc(UINT64(Impl::MaxMaterials) * sizeof(RtMaterialGpu));
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->rtMaterials))) ||
				FAILED(impl->rtMaterials->Map(0, &noRead, reinterpret_cast<void**>(&impl->rtMaterialData))))
			{
				impl->status = L"RT 재질 표 생성 실패.";
				return false;
			}
			memset(impl->rtMaterialData, 0, size_t(Impl::MaxMaterials) * sizeof(RtMaterialGpu));
		}

		// [4b] TLAS 인스턴스 표 · 반사 상수 버퍼 (2026-10-27 렌더 4단계) — UPLOAD 영속 매핑.
		//  인스턴스 표는 프레임 슬롯마다 한 벌(AccelStructure 의 인스턴스 디스크립터와 같은 규칙 — 비행 중 프레임 대비).
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC instanceDesc = BufferDesc(UINT64(Impl::MaxInstances) * sizeof(RtInstanceGpu) * AccelStructure::kFrameSlots);
			D3D12_RESOURCE_DESC cbDesc = BufferDesc(256);
			D3D12_RESOURCE_DESC reflectionDesc = BufferDesc(512);   // 2026-10-28 GI 두 겹으로 반사 상수 224 → 320 B
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &instanceDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->rtInstances))) ||
				FAILED(impl->rtInstances->Map(0, &noRead, reinterpret_cast<void**>(&impl->rtInstanceData))) ||
				FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &reflectionDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->reflectionCB))) ||
				FAILED(impl->reflectionCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->reflectionCBData))) ||
				// 5단계 — [4d] 노이즈 제거 상수 버퍼(208 B → 256 B 칸)
				FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cbDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->denoiseCB))) ||
				FAILED(impl->denoiseCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->denoiseCBData))))
			{
				impl->status = L"RT 인스턴스 표 · 반사 · 노이즈 제거 상수 버퍼 생성 실패.";
				return false;
			}
		}

		// [6] 하늘 · [4c] GI 상수 버퍼 (2026-10-27 렌더 6 · 7단계) — UPLOAD 영속 매핑.
		//  하늘 176 B → 256 B 칸. GI 는 10-28 두 겹으로 겹마다 304 B → 512 B 칸 × 2 (kGiCBSlot).
		{
			D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC desc = BufferDesc(256);
			D3D12_RESOURCE_DESC giDesc = BufferDesc(UINT64(Impl::kGiCBSlot) * Impl::kProbeLayerCount);
			D3D12_RANGE noRead = { 0, 0 };
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->skyCB))) ||
				FAILED(impl->skyCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->skyCBData))) ||
				FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &giDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->giCB))) ||
				FAILED(impl->giCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->giCBData))))
			{
				impl->status = L"하늘 · GI 상수 버퍼 생성 실패.";
				return false;
			}
			// 8단계 — [2] AO 상수 버퍼 (288 B → 512 B, CBV 크기는 256 배수)
			// 9단계 — [7] 반투명 상수 버퍼 (288 B → 512 B) · 색 피라미드 상수 (디스패치마다 256 B 칸 × 밉 수)
			D3D12_RESOURCE_DESC aoDesc = BufferDesc(512);
			D3D12_RESOURCE_DESC pyramidDesc = BufferDesc(256 * Impl::kSceneColorMips);
			// 10단계 — [8] 후처리 상수 (디스패치마다 256 B 칸)
			D3D12_RESOURCE_DESC postDesc = BufferDesc(256 * Impl::kPostDispatchSlots);
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &postDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->postCB))) ||
				FAILED(impl->postCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->postCBData))))
			{
				impl->status = L"후처리 상수 버퍼 생성 실패.";
				return false;
			}
			if (FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &aoDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->aoCB))) ||
				FAILED(impl->aoCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->aoCBData))) ||
				FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &aoDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->transparentCB))) ||
				FAILED(impl->transparentCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->transparentCBData))) ||
				FAILED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &pyramidDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->pyramidCB))) ||
				FAILED(impl->pyramidCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->pyramidCBData))))
			{
				impl->status = L"AO · 반투명 상수 버퍼 생성 실패.";
				return false;
			}
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

		// ── [0] 비동기 컴퓨트 큐 (2026-10-27 렌더 3단계) — TLAS 빌드 전용 ──
		//  컴퓨트 커맨드 리스트도 BuildRaytracingAccelerationStructure 를 기록할 수 있다.
		//  하나라도 실패하면 asyncAs = false 로 두고 예전처럼 직접 커맨드 리스트에서 빌드한다(화면은 같다).
		if (impl->rtSupported)
		{
			D3D12_COMMAND_QUEUE_DESC computeDesc = {};
			computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
			impl->asyncAs =
				SUCCEEDED(impl->device->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&impl->computeQueue))) &&
				SUCCEEDED(impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&impl->computeAllocator))) &&
				SUCCEEDED(impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, impl->computeAllocator.Get(), nullptr, IID_PPV_ARGS(&impl->computeList))) &&
				SUCCEEDED(impl->computeList->Close()) &&
				SUCCEEDED(impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->asFence)));
			if (impl->asyncAs)
			{
				// PIX 등 GPU 도구에서 큐를 알아보게 이름을 붙인다.
				impl->commandQueue->SetName(L"직접 큐 (래스터 · RT · 라이팅)");
				impl->computeQueue->SetName(L"[0] 비동기 컴퓨트 큐 (TLAS 빌드)");
				OutputDebugStringW(L"[GRenderer] [0] TLAS 를 비동기 컴퓨트 큐에서 빌드한다\n");
			}
			else
			{
				impl->status += L"  (비동기 컴퓨트 큐 생성 실패 — TLAS 를 직접 큐에서 빌드)";
			}
		}

		// Root signature (래스터 — 배경 forward · 메인 G-buffer 가 같이 쓴다)
		//   b1 = 묶음의 보이는 목록 시작 번호(루트 상수 1개, 수정방향 3.2), b0 = 프레임, b2 = 재질,
		//   t1~t5 = 텍스처, t6 = 인스턴스, t7 = 보이는 인스턴스 번호 목록.
		//   1 + 2 + 20 + 1 + 2 + 2 = 28 DWORD (2026-10-28 지형 4단계로 재질 상수 12 → 20).
		//   (인스턴싱 전에는 b1 루트 상수 32개로 행렬을 그리기마다 넘겼다.)
		//   ★ 2026-10-27 2단계 — TLAS(t0) 슬롯을 뺐다. 픽셀 셰이더 안 인라인 반사가 없어졌고,
		//     RT 는 [4a] RT 그림자 컴퓨트가 자기 루트 시그니처로 TLAS 를 받는다.
		{
			D3D12_ROOT_PARAMETER params[6] = {};

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
			params[2].Constants.Num32BitValues = kMaterialRootConstants;
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

			// s0 = 재질 텍스처 — 이방성 16 (2026-10-28, 설계 문서 «이방성 샘플러 — 비스듬히 보는 넓은 지면의 선명도»).
			//  선형 밉 필터는 화면 픽셀이 덮는 텍스처 영역의 «긴 쪽» 으로 밉을 골라, 비스듬히 보는 땅이 흐려진다.
			//  이방성은 긴 쪽을 따라 여러 번 읽어 짧은 쪽 기준 밉을 쓴다(D3D12 의 최대 16). 지형 판의 SampleGrad 도
			//  넘긴 미분으로 같은 계산을 한다. 밉 필터는 선형 그대로다(D3D12_FILTER_ANISOTROPIC 의 정의).
			D3D12_STATIC_SAMPLER_DESC sampler = {};
			sampler.Filter = D3D12_FILTER_ANISOTROPIC;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.MaxAnisotropy = D3D12_MAX_MAXANISOTROPY;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			// 2026-10-28 지형 레이어 — s1 = 선형 · 가장자리 고정. 레이어 마스크(등면적 팔면체 매핑)는 가장자리 너머가
			//  반대편이 아니라 접힌 이웃이라 반복(WRAP)으로 읽으면 안 된다(리소스 시안도 EXTEND). Forward.hlsl gClampSampler.
			//  마스크는 SampleLevel(밉을 직접 고름)로만 읽어 이방성이 필요 없다.
			D3D12_STATIC_SAMPLER_DESC samplers[2] = { sampler, sampler };
			samplers[1].ShaderRegister = 1;
			samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[1].MaxAnisotropy = 1;
			samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = 6;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 2;
			rsDesc.pStaticSamplers = samplers;
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

			// ★ 2026-10-27 2단계 — 래스터 셰이더에는 RayQuery 가 없으므로 RT 지원 여부와 상관없이 SM 6.0 으로 컴파일한다.
			const std::wstring path = ShaderPath(L"Forward.hlsl");
			const wchar_t* vsTarget = L"vs_6_0";
			const wchar_t* psTarget = L"ps_6_0";

			std::string log;
			std::vector<uint8_t> vs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", vsTarget, nullptr, 0, log);
			if (vs.empty())
			{
				OutputDebugStringA(("[VS] " + log + "\n").c_str());
				impl->status = L"버텍스 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			std::vector<uint8_t> ps = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", psTarget, nullptr, 0, log);
			if (ps.empty())
			{
				OutputDebugStringA(("[PS] " + log + "\n").c_str());
				impl->status = L"픽셀 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			// [1] G-buffer 진입점 (2026-10-27) — 같은 파일 · 같은 정점 셰이더, 픽셀 진입점만 다르다.
			std::vector<uint8_t> gbufferPs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSGBuffer", psTarget, nullptr, 0, log);
			if (gbufferPs.empty())
			{
				OutputDebugStringA(("[PS GBuffer] " + log + "\n").c_str());
				impl->status = L"G-buffer 픽셀 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			// 2026-10-28 알파 마스크 — 같은 두 진입점을 ALPHA_MASK=1 로 한 번 더 컴파일한다(discard 가 들어간 판).
			//  마스크 재질만 이 판으로 그려, 마스크 아닌 물체는 discard 없는 셰이더 그대로 둔다(이른 깊이 쓰기를 잃지 않게).
			const wchar_t* const maskDefine[] = { L"ALPHA_MASK=1" };
			std::vector<uint8_t> psMasked = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", psTarget, maskDefine, 1, log);
			std::vector<uint8_t> gbufferPsMasked = psMasked.empty() ? std::vector<uint8_t>{} :
				impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSGBuffer", psTarget, maskDefine, 1, log);
			if (psMasked.empty() || gbufferPsMasked.empty())
			{
				OutputDebugStringA(("[PS 알파 마스크] " + log + "\n").c_str());
				impl->status = L"알파 마스크 픽셀 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			// 2026-10-28 지형 레이어 — 같은 두 진입점을 TERRAIN_LAYERS=1 로 한 번 더 컴파일한다(레이어 4종 + 마스크 섞기 판).
			//  지형 재질(CreateTerrainMaterial)만 이 판으로 그린다. 정점 셰이더는 같다(VOut 에 메시 좌표가 늘 들어 있다).
			const wchar_t* const terrainDefine[] = { L"TERRAIN_LAYERS=1" };
			std::vector<uint8_t> psTerrain = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", psTarget, terrainDefine, 1, log);
			std::vector<uint8_t> gbufferPsTerrain = psTerrain.empty() ? std::vector<uint8_t>{} :
				impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSGBuffer", psTarget, terrainDefine, 1, log);
			if (psTerrain.empty() || gbufferPsTerrain.empty())
			{
				OutputDebugStringA(("[PS 지형 레이어] " + log + "\n").c_str());
				impl->status = L"지형 레이어 픽셀 셰이더 컴파일 실패 (출력창 참고).";
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
			psoDesc.SampleDesc.Count = 1;

			// 묶음 키(Batch::pso) 순서대로 4종을 만든다: 0 기본 · 1 양면 · 2 반전 · 3 반전 양면.
			//  반전 배율은 와인딩만 뒤집는다. SV_IsFrontFace도 실제 앞면을 가리켜야 한다.
			//  (2026-10-27 forward 와 G-buffer 가 같은 4종을 만들게 되어 순서를 한 곳으로 모았다)
			auto createVariants = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC desc,
				ComPtr<ID3D12PipelineState>* const (&out)[4]) -> bool
			{
				for (UINT key = 0; key < 4; ++key)
				{
					desc.RasterizerState.CullMode = (key & 1u) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
					desc.RasterizerState.FrontCounterClockwise = (key & 2u) ? TRUE : FALSE;
					if (FAILED(impl->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out[key]->ReleaseAndGetAddressOf()))))
						return false;
				}
				return true;
			};
			// 2026-10-28 지형 레이어 판 — 단면이라 «기본 · 반전» 2종만(묶음 키 kTerrainPsoKey + 0 · 1).
			auto createTerrainPair = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC desc,
				ComPtr<ID3D12PipelineState>* const (&out)[2]) -> bool
			{
				for (UINT mirrored = 0; mirrored < 2; ++mirrored)
				{
					desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
					desc.RasterizerState.FrontCounterClockwise = mirrored ? TRUE : FALSE;
					if (FAILED(impl->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out[mirrored]->ReleaseAndGetAddressOf()))))
						return false;
				}
				return true;
			};

			// 배경 패스 forward — 백버퍼가 아니라 HDR 컬러에 그린다(2026-10-27)
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Impl::kHdrFormat;
			ComPtr<ID3D12PipelineState>* const forwardOut[4] = {
				&impl->pso, &impl->doubleSidedPso, &impl->mirroredPso, &impl->mirroredDoubleSidedPso };
			if (!createVariants(psoDesc, forwardOut))
			{
				impl->status = L"forward PSO 생성 실패.";
				return false;
			}
			// 2026-10-28 알파 마스크 판 (묶음 키 4 ~ 7)
			{
				D3D12_GRAPHICS_PIPELINE_STATE_DESC maskedDesc = psoDesc;
				maskedDesc.PS = { psMasked.data(), psMasked.size() };
				ComPtr<ID3D12PipelineState>* const forwardMaskedOut[4] = {
					&impl->forwardMaskedPsos[0], &impl->forwardMaskedPsos[1], &impl->forwardMaskedPsos[2], &impl->forwardMaskedPsos[3] };
				if (!createVariants(maskedDesc, forwardMaskedOut))
				{
					impl->status = L"forward 알파 마스크 PSO 생성 실패.";
					return false;
				}
			}
			// 2026-10-28 지형 레이어 판 (묶음 키 8 · 9)
			{
				D3D12_GRAPHICS_PIPELINE_STATE_DESC terrainDesc = psoDesc;
				terrainDesc.PS = { psTerrain.data(), psTerrain.size() };
				ComPtr<ID3D12PipelineState>* const forwardTerrainOut[2] = { &impl->forwardTerrainPsos[0], &impl->forwardTerrainPsos[1] };
				if (!createTerrainPair(terrainDesc, forwardTerrainOut))
				{
					impl->status = L"forward 지형 레이어 PSO 생성 실패.";
					return false;
				}
			}

			// [1] G-buffer — 렌더 타깃 5장(GB0~GB4, PSGBuffer 의 SV_Target0~4), 형식은 kGBufferFormats
			psoDesc.PS = { gbufferPs.data(), gbufferPs.size() };
			psoDesc.NumRenderTargets = Impl::GbCount;
			for (UINT i = 0; i < Impl::GbCount; ++i)
			{
				psoDesc.RTVFormats[i] = Impl::kGBufferFormats[i];
				psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}
			ComPtr<ID3D12PipelineState>* const gbufferOut[4] = {
				&impl->gbufferPsos[0], &impl->gbufferPsos[1], &impl->gbufferPsos[2], &impl->gbufferPsos[3] };
			if (!createVariants(psoDesc, gbufferOut))
			{
				impl->status = L"G-buffer PSO 생성 실패.";
				return false;
			}
			// 2026-10-28 알파 마스크 판 (묶음 키 4 ~ 7)
			psoDesc.PS = { gbufferPsMasked.data(), gbufferPsMasked.size() };
			ComPtr<ID3D12PipelineState>* const gbufferMaskedOut[4] = {
				&impl->gbufferPsos[4], &impl->gbufferPsos[5], &impl->gbufferPsos[6], &impl->gbufferPsos[7] };
			if (!createVariants(psoDesc, gbufferMaskedOut))
			{
				impl->status = L"G-buffer 알파 마스크 PSO 생성 실패.";
				return false;
			}
			// 2026-10-28 지형 레이어 판 (묶음 키 8 · 9)
			psoDesc.PS = { gbufferPsTerrain.data(), gbufferPsTerrain.size() };
			ComPtr<ID3D12PipelineState>* const gbufferTerrainOut[2] = {
				&impl->gbufferPsos[Impl::kTerrainPsoKey], &impl->gbufferPsos[Impl::kTerrainPsoKey + 1] };
			if (!createTerrainPair(psoDesc, gbufferTerrainOut))
			{
				impl->status = L"G-buffer 지형 레이어 PSO 생성 실패.";
				return false;
			}
		}

		// ── [5] 라이팅 합성 파이프라인 (Lighting.hlsl, 2026-10-27) ──
		//   b0 = LightingCB(루트 CBV), 테이블 = targetHeap 슬롯 0 부터
		//   (SRV t0~t5 G-buffer GB0~GB4 · 깊이 = 슬롯 0~5, UAV u0 HDR = 슬롯 6,
		//    SRV t6 그림자 마스크 · t7 반사(노이즈 제거 후) · t8 반사(제거 전, 디버그) = 슬롯 9~11)
		//   5단계 후속 — GB4 를 G-buffer 묶음에 넣으면서 범위가 6개 → 3개로 줄었다.
		{
			D3D12_DESCRIPTOR_RANGE ranges[3] = {};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].NumDescriptors = Impl::GbCount + 1;   // G-buffer 5 + 깊이
			ranges[0].BaseShaderRegister = 0;
			ranges[0].OffsetInDescriptorsFromTableStart = Impl::SlotGBuffer;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 1;
			ranges[1].BaseShaderRegister = 0;
			ranges[1].OffsetInDescriptorsFromTableStart = Impl::SlotHdrUav;
			ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // 그림자 마스크 · 반사 · 제거 전 반사 · (6단계) 하늘 조도
			ranges[2].NumDescriptors = Impl::kLightingTailSrvCount;
			ranges[2].BaseShaderRegister = 6;   // t6 ~ t9
			ranges[2].OffsetInDescriptorsFromTableStart = Impl::SlotShadowSrv;

			D3D12_ROOT_PARAMETER params[2] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable.NumDescriptorRanges = _countof(ranges);
			params[1].DescriptorTable.pDescriptorRanges = ranges;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// 6단계 — s1 = 하늘(등장방형) 샘플러: 선형 · U 반복 · V 고정 / 7단계 — s2 = 선형 · 고정 (프로브 아틀라스)
			D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
			samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[0].MaxAnisotropy = 1;
			samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[0].ShaderRegister = 1;
			samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			samplers[1] = samplers[0];
			samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[1].ShaderRegister = 2;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = 2;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 2;
			rsDesc.pStaticSamplers = samplers;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->lightingRootSig))))
			{
				impl->status = L"라이팅 루트 시그니처 생성 실패.";
				return false;
			}

			const std::wstring path = ShaderPath(L"Lighting.hlsl");
			std::string log;
			std::vector<uint8_t> cs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"CSMain", L"cs_6_0", nullptr, 0, log);
			if (cs.empty())
			{
				OutputDebugStringA(("[Lighting CS] " + log + "\n").c_str());
				impl->status = L"라이팅 컴퓨트 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->lightingRootSig.Get();
			psoDesc.CS = { cs.data(), cs.size() };
			if (FAILED(impl->device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&impl->lightingPso))))
			{
				impl->status = L"라이팅 PSO 생성 실패.";
				return false;
			}
		}

		// ── [4a] RT 그림자 파이프라인 ──
		//  2026-10-28 — 아래 «바인드리스 컴퓨트 공용 루트 시그니처 + 파이프라인» 블록으로 옮겼다(알파 마스크 판정에
		//  재질 텍스처가 필요해 cs_6_5 · 테이블 방식 → cs_6_6 · 공용 바인드리스 루트). 예전 전용 루트 시그니처는 없앴다.

		// ── [4b] RT 반사 파이프라인 (RtReflection.hlsl, DispatchRays, 2026-10-27 렌더 4단계) ──
		//  RT 지원 + SM 6.6 바인드리스 지원 장치에서만. 하나라도 실패하면 반사만 끄고 계속한다.
		if (impl->rtSupported && impl->bindlessSupported)
		{
			bool ok = true;

			// 전역 루트 시그니처 (버전 1.1)
			//   b0 = ReflectionCB, t0 = TLAS, t1 = 재질 표, t2 = 인스턴스 표 — 전부 루트 디스크립터(버퍼라 힙 없이 주소로)
			//   s0 = 정적 샘플러(선형 · WRAP, 래스터와 같은 값)
			//   플래그 CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED — 셰이더가 ResourceDescriptorHeap[번호] 를 쓴다
			//   (사양: 이 플래그는 전역 루트에만 둘 수 있고, SetDescriptorHeaps 를 루트 시그니처 설정보다 먼저 해야 한다)
			D3D12_ROOT_PARAMETER1 params[4] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			for (UINT i = 1; i < 4; ++i)
			{
				params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[i].Descriptor.ShaderRegister = i - 1;
			}
			for (D3D12_ROOT_PARAMETER1& param : params)
			{
				param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			D3D12_STATIC_SAMPLER_DESC sampler = {};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.MaxAnisotropy = 1;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			// 6단계 — s1 = 하늘(등장방형) 샘플러: 선형 · U 반복 · V 고정 (Miss 가 하늘 텍스처를 읽는다)
			// 7단계 — s2 = 선형 · 고정 (히트 셰이더가 GI 프로브 아틀라스를 읽는다)
			D3D12_STATIC_SAMPLER_DESC samplers[3] = { sampler, sampler, sampler };
			samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[1].ShaderRegister = 1;
			samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[2].ShaderRegister = 2;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			rsDesc.Desc_1_1.NumParameters = 4;
			rsDesc.Desc_1_1.pParameters = params;
			rsDesc.Desc_1_1.NumStaticSamplers = 3;
			rsDesc.Desc_1_1.pStaticSamplers = samplers;
			rsDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeVersionedRootSignature(&rsDesc, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->rtReflectionRootSig))))
			{
				if (err) OutputDebugStringA(("[RtReflection 루트] " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) + "\n").c_str());
				ok = false;
			}

			// 셰이더 라이브러리 — 진입점 없이 lib_6_6 (RayGen · Miss · ClosestHit 을 내보낸다)
			std::vector<uint8_t> library;
			if (ok)
			{
				std::string log;
				library = impl->shaderCompiler.CompileFromFile(ShaderPath(L"RtReflection.hlsl").c_str(), L"", L"lib_6_6", nullptr, 0, log);
				if (library.empty())
				{
					OutputDebugStringA(("[RtReflection lib] " + log + "\n").c_str());
					ok = false;
				}
			}

			// RT 파이프라인 상태 객체 — 하위 객체 5개
			//   DXIL 라이브러리 · 히트 그룹(삼각형, closest hit + any hit — 2026-10-28 알파 마스크) · 셰이더 설정(페이로드 16 B, 속성 8 B)
			//   · 전역 루트 시그니처 · 파이프라인 설정(재귀 1 — raygen 의 TraceRay 한 단계. 히트 셰이더 안 그림자는 RayQuery 라 안 셈)
			static const wchar_t* const kRayGen = L"RayGen";
			static const wchar_t* const kMiss = L"Miss";
			static const wchar_t* const kClosestHit = L"ClosestHit";
			static const wchar_t* const kAnyHit = L"AnyHit";   // 2026-10-28 알파 마스크 — 비불투명 인스턴스의 빈 곳을 거른다
			static const wchar_t* const kHitGroup = L"HitGroup";
			if (ok)
			{
				D3D12_EXPORT_DESC exports[4] = {
					{ kRayGen, nullptr, D3D12_EXPORT_FLAG_NONE },
					{ kMiss, nullptr, D3D12_EXPORT_FLAG_NONE },
					{ kClosestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
					{ kAnyHit, nullptr, D3D12_EXPORT_FLAG_NONE } };
				D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
				libraryDesc.DXILLibrary = { library.data(), library.size() };
				libraryDesc.NumExports = 4;
				libraryDesc.pExports = exports;

				// 히트 그룹 = closest hit + any hit. any hit 은 비불투명 인스턴스(알파 마스크 · 투과)에서만 불린다
				//  — 불투명 BLAS(GEOMETRY_FLAG_OPAQUE)는 시스템이 바로 받아들여 any hit 을 건너뛴다.
				D3D12_HIT_GROUP_DESC hitGroup = {};
				hitGroup.HitGroupExport = kHitGroup;
				hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
				hitGroup.ClosestHitShaderImport = kClosestHit;
				hitGroup.AnyHitShaderImport = kAnyHit;

				D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
				shaderConfig.MaxPayloadSizeInBytes = 16;     // ReflectionPayload: float3 color + float hitT
				shaderConfig.MaxAttributeSizeInBytes = 8;    // BuiltInTriangleIntersectionAttributes: float2

				D3D12_GLOBAL_ROOT_SIGNATURE globalRoot = { impl->rtReflectionRootSig.Get() };
				D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = { 1 };

				D3D12_STATE_SUBOBJECT subobjects[5] = {
					{ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libraryDesc },
					{ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup },
					{ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig },
					{ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot },
					{ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig } };
				D3D12_STATE_OBJECT_DESC stateDesc = {};
				stateDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
				stateDesc.NumSubobjects = 5;
				stateDesc.pSubobjects = subobjects;
				ok = SUCCEEDED(impl->device->CreateStateObject(&stateDesc, IID_PPV_ARGS(&impl->rtReflectionPso)));
			}

			// 셰이더 테이블 — 레코드 = 셰이더 식별자 32 B(로컬 루트 인자 없음).
			//   각 표의 시작은 64 B 정렬(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT): raygen 0 · miss 64 · hit group 128.
			if (ok)
			{
				ComPtr<ID3D12StateObjectProperties> properties;
				ok = SUCCEEDED(impl->rtReflectionPso.As(&properties));
				const void* identifiers[3] = {
					ok ? properties->GetShaderIdentifier(kRayGen) : nullptr,
					ok ? properties->GetShaderIdentifier(kMiss) : nullptr,
					ok ? properties->GetShaderIdentifier(kHitGroup) : nullptr };
				ok = ok && identifiers[0] && identifiers[1] && identifiers[2];

				D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
				D3D12_RESOURCE_DESC desc = BufferDesc(3 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
				uint8_t* mapped = nullptr;
				D3D12_RANGE noRead = { 0, 0 };
				ok = ok && SUCCEEDED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->rtShaderTable))) &&
					SUCCEEDED(impl->rtShaderTable->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
				if (ok)
				{
					for (UINT i = 0; i < 3; ++i)
						memcpy(mapped + i * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, identifiers[i], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					impl->rtShaderTable->Unmap(0, nullptr);
				}
			}

			// ── [4d] 반사 노이즈 제거 파이프라인 (RtReflectionDenoise.hlsl, 2026-10-27 렌더 5단계) ──
			//  B 방식 반사는 레이 1개라 노이즈 제거 없이는 점박이다 → 여기서 실패하면 반사째로 끈다.
			//  루트 시그니처: b0 상수 하나 + CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED (텍스처는 전부 바인드리스 번호로 읽고 쓴다).
			//  두 진입점(시간 누적 · 공간 필터)이 같은 루트 시그니처 · 같은 상수를 쓴다.
			if (ok)
			{
				D3D12_ROOT_PARAMETER1 param = {};
				param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				param.Descriptor.ShaderRegister = 0;
				param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				D3D12_VERSIONED_ROOT_SIGNATURE_DESC denoiseRs = {};
				denoiseRs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
				denoiseRs.Desc_1_1.NumParameters = 1;
				denoiseRs.Desc_1_1.pParameters = &param;
				denoiseRs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
				ComPtr<ID3DBlob> denoiseSig, denoiseErr;
				if (FAILED(D3D12SerializeVersionedRootSignature(&denoiseRs, &denoiseSig, &denoiseErr)) ||
					FAILED(impl->device->CreateRootSignature(0, denoiseSig->GetBufferPointer(), denoiseSig->GetBufferSize(),
						IID_PPV_ARGS(&impl->denoiseRootSig))))
				{
					if (denoiseErr) OutputDebugStringA(("[RtReflectionDenoise 루트] " + std::string(static_cast<const char*>(denoiseErr->GetBufferPointer()), denoiseErr->GetBufferSize()) + "\n").c_str());
					ok = false;
				}

				auto createDenoisePso = [&](const wchar_t* entry, ComPtr<ID3D12PipelineState>& out) -> bool
				{
					std::string log;
					const std::vector<uint8_t> cs = impl->shaderCompiler.CompileFromFile(
						ShaderPath(L"RtReflectionDenoise.hlsl").c_str(), entry, L"cs_6_6", nullptr, 0, log);
					if (cs.empty())
					{
						OutputDebugStringA(("[RtReflectionDenoise] " + log + "\n").c_str());
						return false;
					}
					D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
					desc.pRootSignature = impl->denoiseRootSig.Get();
					desc.CS = { cs.data(), cs.size() };
					return SUCCEEDED(impl->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out)));
				};
				ok = ok && createDenoisePso(L"CSTemporal", impl->denoiseTemporalPso) &&
					createDenoisePso(L"CSSpatial", impl->denoiseSpatialPso);
			}

			if (!ok)
			{
				impl->rtReflectionPso.Reset();
				impl->status += L"  (RT 반사 파이프라인 생성 실패 — 반사 없이 진행)";
				OutputDebugStringW(L"[GRenderer] [4b] RT 반사 파이프라인 생성 실패 — 반사 없이 진행\n");
			}
			else
			{
				OutputDebugStringW(L"[GRenderer] [4b] RT 반사 파이프라인 준비 (DispatchRays · SM 6.6 바인드리스, B 방식 + [4d] 임시 노이즈 제거)\n");
			}
		}

		// ── 바인드리스 컴퓨트 공용 루트 시그니처 + 파이프라인 (2026-10-27 렌더 6단계~) ──
		//  6단계부터 새로 붙는 컴퓨트 패스는 전부 이 루트 시그니처 하나를 쓴다(Impl::computeRootSig 주석).
		//  SM 6.6 바인드리스 장치에서만 만든다. 실패하면 그 패스만 꺼지고 예전 화면으로 돈다.
		if (impl->bindlessHeap)
		{
			bool ok = true;
			D3D12_ROOT_PARAMETER1 params[4] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			for (UINT i = 1; i < 4; ++i)
			{
				params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[i].Descriptor.ShaderRegister = i - 1;   // t0 TLAS · t1 재질 표 · t2 인스턴스 표
			}
			for (D3D12_ROOT_PARAMETER1& param : params)
			{
				param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
			for (UINT i = 0; i < 3; ++i)
			{
				samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplers[i].MaxAnisotropy = 1;
				samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
				samplers[i].ShaderRegister = i;
				samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;   // s1 등장방형: U 반복 · V 고정
			samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;   // s2 고정

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			rsDesc.Desc_1_1.NumParameters = 4;
			rsDesc.Desc_1_1.pParameters = params;
			rsDesc.Desc_1_1.NumStaticSamplers = 3;
			rsDesc.Desc_1_1.pStaticSamplers = samplers;
			rsDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeVersionedRootSignature(&rsDesc, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->computeRootSig))))
			{
				if (err) OutputDebugStringA(("[컴퓨트 공용 루트] " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) + "\n").c_str());
				ok = false;
			}

			// 셰이더 파일 · 진입점 → cs_6_6 컴퓨트 PSO (공용 루트 시그니처). define 은 L"이름=값" 하나(선택).
			//  (8단계 — 컴파일 · PSO 생성에 걸린 시간을 출력한다. Debug(-Od) 에서 드라이버 PSO 생성이 아주 느린 셰이더를 찾으려고)
			auto createCompute = [&](const wchar_t* file, const wchar_t* entry, ComPtr<ID3D12PipelineState>& out,
				const wchar_t* define = nullptr) -> bool
			{
				LARGE_INTEGER frequency, t0, t1, t2;
				QueryPerformanceFrequency(&frequency);
				QueryPerformanceCounter(&t0);
				std::string log;
				const std::vector<uint8_t> cs = impl->shaderCompiler.CompileFromFile(ShaderPath(file).c_str(), entry, L"cs_6_6",
					define ? &define : nullptr, define ? 1 : 0, log);
				if (cs.empty())
				{
					OutputDebugStringW((std::wstring(L"[") + file + L" " + entry + L"] 컴파일 실패\n").c_str());
					OutputDebugStringA((log + "\n").c_str());
					return false;
				}
				QueryPerformanceCounter(&t1);
				D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
				desc.pRootSignature = impl->computeRootSig.Get();
				desc.CS = { cs.data(), cs.size() };
				const bool created = SUCCEEDED(impl->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(out.ReleaseAndGetAddressOf())));
				QueryPerformanceCounter(&t2);
				wchar_t line[192];
				swprintf_s(line, L"[GRenderer]   %s %s — 컴파일 %.0f ms · PSO %.0f ms (DXIL %zu B)\n", file, entry,
					double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(frequency.QuadPart),
					double(t2.QuadPart - t1.QuadPart) * 1000.0 / double(frequency.QuadPart), cs.size());
				OutputDebugStringW(line);
				return created;
			};

			// [6] 하늘 — 세 진입점 (Sky.hlsl)
			const bool skyOk = ok &&
				createCompute(L"Sky.hlsl", L"CSMultiScattering", impl->skyMultiScatteringPso) &&
				createCompute(L"Sky.hlsl", L"CSGenerateSky", impl->skyGeneratePso) &&
				createCompute(L"Sky.hlsl", L"CSSkyIrradiance", impl->skyIrradiancePso) &&
				createCompute(L"Sky.hlsl", L"CSDrawSky", impl->skyDrawPso);
			if (!skyOk)
			{
				impl->skyMultiScatteringPso.Reset();
				impl->skyGeneratePso.Reset();
				impl->skyIrradiancePso.Reset();
				impl->skyDrawPso.Reset();
				impl->status += L"  (하늘 파이프라인 생성 실패 — 지우기 색 하늘)";
				OutputDebugStringW(L"[GRenderer] [6] 하늘 파이프라인 생성 실패 — 지우기 색 하늘 · 상수 주변광\n");
			}
			else
			{
				OutputDebugStringW(L"[GRenderer] [6] 하늘 파이프라인 준비 (하늘 텍스처 256x128 · 대기 산란 + 다중 산란 LUT, 임시)\n");
			}

			// [4c] GI 프로브 — 세 진입점 (ProbeGI.hlsl, RayQuery). RT 장치에서만. 하늘이 있어야 미스 · 대체 주변광을 읽는다.
			if (impl->rtSupported && skyOk)
			{
				const bool giOk =
					createCompute(L"ProbeGI.hlsl", L"CSProbeTrace", impl->giTracePso) &&
					createCompute(L"ProbeGI.hlsl", L"CSProbeBlendDistance", impl->giBlendDistancePso) &&
					createCompute(L"ProbeGI.hlsl", L"CSProbeBlendIrradiance", impl->giBlendIrradiancePso);
				if (!giOk)
				{
					impl->giTracePso.Reset();
					impl->giBlendDistancePso.Reset();
					impl->giBlendIrradiancePso.Reset();
					impl->status += L"  (GI 프로브 파이프라인 생성 실패 — 하늘 조도로 주변광)";
					OutputDebugStringW(L"[GRenderer] [4c] GI 프로브 파이프라인 생성 실패 — 하늘 조도로 주변광\n");
				}
				else
				{
					OutputDebugStringW(L"[GRenderer] [4c] GI 프로브 파이프라인 준비 (DDGI 두 겹: 32x8x32 · 4 m + 32x8x32 · 16 m, 지면에 맞춘 격자, 겹마다 프레임당 2048개 x 64 레이)\n");
				}
			}

			// [7] 화면 색 피라미드 — 두 진입점 (ColorPyramid.hlsl). 반투명 파이프라인과 같이 쓴다(아래 블록).
			if (ok && impl->rtSupported)
			{
				const bool pyramidOk =
					createCompute(L"ColorPyramid.hlsl", L"CSCopy", impl->pyramidCopyPso) &&
					createCompute(L"ColorPyramid.hlsl", L"CSDownsample", impl->pyramidDownsamplePso);
				if (!pyramidOk)
				{
					impl->pyramidCopyPso.Reset();
					impl->pyramidDownsamplePso.Reset();
				}
			}

			// [8] 후처리 — 네 진입점 (PostProcess.hlsl). RT 와 상관없이 바인드리스만 있으면 된다.
			if (ok)
			{
				const bool postOk =
					createCompute(L"PostProcess.hlsl", L"CSHistogram", impl->histogramPso) &&
					createCompute(L"PostProcess.hlsl", L"CSExposure", impl->exposurePso) &&
					createCompute(L"PostProcess.hlsl", L"CSBloomDownsample", impl->bloomDownPso) &&
					createCompute(L"PostProcess.hlsl", L"CSBloomUpsample", impl->bloomUpPso);
				if (!postOk)
				{
					impl->histogramPso.Reset();
					impl->exposurePso.Reset();
					impl->bloomDownPso.Reset();
					impl->bloomUpPso.Reset();
					OutputDebugStringW(L"[GRenderer] [8] 후처리 파이프라인 생성 실패 — 예전 톤맵(0~1 자르기)\n");
				}
			}

			// [4a] RT 그림자 (RtShadow.hlsl) — 2026-10-28 부터 이 공용 루트(b0 ShadowCB · t0 TLAS · t1 재질 표 · t2 인스턴스 표 · s0).
			//  법선 · 깊이 · 그림자 마스크 UAV 는 바인드리스 번호로 읽고 쓴다. RT 지원 장치에서만. 실패하면 그림자 없이 계속한다.
			if (ok && impl->rtSupported)
			{
				if (!createCompute(L"RtShadow.hlsl", L"CSMain", impl->rtShadowPso))
				{
					impl->rtShadowPso.Reset();
					impl->status += L"  (RT 그림자 파이프라인 생성 실패 — 그림자 없이 진행)";
				}
			}

			// [2] AO — 세 진입점 (AO.hlsl). RT 가 없는 장치는 RayQuery 를 빼고 컴파일한다(순수 SSAO).
			if (ok)
			{
				const wchar_t* aoDefine = impl->rtSupported ? L"AO_RAYQUERY=1" : L"AO_RAYQUERY=0";
				const bool aoOk =
					createCompute(L"AO.hlsl", L"CSAoTrace", impl->aoTracePso, aoDefine) &&
					createCompute(L"AO.hlsl", L"CSAoTemporal", impl->aoTemporalPso, aoDefine) &&
					createCompute(L"AO.hlsl", L"CSAoSpatial", impl->aoSpatialPso, aoDefine);
				if (!aoOk)
				{
					impl->aoTracePso.Reset();
					impl->aoTemporalPso.Reset();
					impl->aoSpatialPso.Reset();
					impl->status += L"  (AO 파이프라인 생성 실패 — AO 없이)";
					OutputDebugStringW(L"[GRenderer] [2] AO 파이프라인 생성 실패 — AO 없이\n");
				}
				else
				{
					OutputDebugStringW(impl->rtSupported
						? L"[GRenderer] [2] AO 파이프라인 준비 (화면 공간 추적 + 모르는 레이만 RayQuery, 반경 1.5 m)\n"
						: L"[GRenderer] [2] AO 파이프라인 준비 (RT 없음 — 화면 공간만)\n");
				}
			}

			// [3] 분류 — 한 진입점 (ReflectionClassify.hlsl) + 타일 목록 · 인자 버퍼 · 인자 원본 · 명령 시그니처 (2026-10-27 렌더 11단계)
			//  [4b] 반사 파이프라인 · 셰이더 테이블이 있어야 만든다(인자 원본에 셰이더 테이블 주소가 들어간다).
			//  하나라도 실패하면 분류만 끄고 예전 DispatchRays(화면 크기)로 돈다 — 화면은 같다.
			//  (RT 장치 = 레이트레이싱 티어 1.1 이상(장치 고를 때 확인) — ExecuteIndirect 의 DispatchRays 도 1.1 부터라 따로 볼 것이 없다)
			if (ok && impl->rtReflectionPso)
			{
				impl->reflectionTilesX = (impl->width + Impl::kReflectionTileSize - 1) / Impl::kReflectionTileSize;
				impl->reflectionTilesY = (impl->height + Impl::kReflectionTileSize - 1) / Impl::kReflectionTileSize;
				const UINT tileCount = impl->reflectionTilesX * impl->reflectionTilesY;

				// 버퍼 둘은 COMMON 으로 만든다(버퍼는 처음 상태를 무시한다 — #1328). 인자 원본 · 상수는 UPLOAD.
				const D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
				const D3D12_HEAP_PROPERTIES upload = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
				D3D12_RESOURCE_DESC tilesDesc = BufferDesc(UINT64(tileCount) * sizeof(uint32_t));
				tilesDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				D3D12_RESOURCE_DESC argsDesc = BufferDesc(sizeof(D3D12_DISPATCH_RAYS_DESC));
				argsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				const D3D12_RESOURCE_DESC templateDesc = BufferDesc(sizeof(D3D12_DISPATCH_RAYS_DESC));
				const D3D12_RESOURCE_DESC cbDesc = BufferDesc(256);
				D3D12_RANGE noRead = { 0, 0 };
				bool classifyOk =
					createCompute(L"ReflectionClassify.hlsl", L"CSClassifyTiles", impl->classifyPso) &&
					SUCCEEDED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tilesDesc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&impl->reflectionTiles.resource))) &&
					SUCCEEDED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &argsDesc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&impl->reflectionArgs.resource))) &&
					SUCCEEDED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &templateDesc,
						D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->reflectionArgsTemplate))) &&
					SUCCEEDED(impl->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cbDesc,
						D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&impl->classifyCB))) &&
					SUCCEEDED(impl->classifyCB->Map(0, &noRead, reinterpret_cast<void**>(&impl->classifyCBData)));
				impl->reflectionTiles.state = impl->reflectionArgs.state = D3D12_RESOURCE_STATE_COMMON;

				// 인자 원본 — 셰이더 테이블 주소 + Width 0 · Height 1 · Depth 1. 프레임마다 인자 버퍼로 복사한 뒤 분류가 Width 를 채운다.
				//  (사양: ExecuteIndirect 가 쓰는 셰이더 테이블을 그 호출이 바꾸면 안 된다 — 표는 초기화 때 한 번 쓰고 끝이라 해당 없음)
				if (classifyOk)
				{
					const D3D12_DISPATCH_RAYS_DESC rays = impl->ReflectionDispatchDesc(0, 1);
					void* mapped = nullptr;
					classifyOk = SUCCEEDED(impl->reflectionArgsTemplate->Map(0, &noRead, &mapped));
					if (classifyOk)
					{
						memcpy(mapped, &rays, sizeof(rays));
						impl->reflectionArgsTemplate->Unmap(0, nullptr);
					}
				}

				// 명령 시그니처 — DISPATCH_RAYS 인자 하나. 루트 인자를 바꾸지 않으므로 루트 시그니처는 nullptr
				//  (CreateCommandSignature 문서: 바인딩을 바꾸는 명령이 있을 때만 루트 시그니처가 필요하다).
				if (classifyOk)
				{
					D3D12_INDIRECT_ARGUMENT_DESC argument = {};
					argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
					D3D12_COMMAND_SIGNATURE_DESC signature = {};
					signature.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
					signature.NumArgumentDescs = 1;
					signature.pArgumentDescs = &argument;
					classifyOk = SUCCEEDED(impl->device->CreateCommandSignature(&signature, nullptr,
						IID_PPV_ARGS(&impl->dispatchRaysSignature)));
				}

				// 바인드리스 79 ~ 81 — 타일 목록 SRV · UAV (uint 구조 버퍼), 인자 UAV (raw — 바이트 위치로 Width 에 더한다)
				if (classifyOk)
				{
					D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
					srv.Format = DXGI_FORMAT_UNKNOWN;
					srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
					srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srv.Buffer.NumElements = tileCount;
					srv.Buffer.StructureByteStride = sizeof(uint32_t);
					impl->device->CreateShaderResourceView(impl->reflectionTiles.resource.Get(), &srv,
						impl->BindlessCpu(Impl::BindlessReflectionTileSrv));
					D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
					uav.Format = DXGI_FORMAT_UNKNOWN;
					uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
					uav.Buffer.NumElements = tileCount;
					uav.Buffer.StructureByteStride = sizeof(uint32_t);
					impl->device->CreateUnorderedAccessView(impl->reflectionTiles.resource.Get(), nullptr, &uav,
						impl->BindlessCpu(Impl::BindlessReflectionTileUav));
					D3D12_UNORDERED_ACCESS_VIEW_DESC rawUav = {};
					rawUav.Format = DXGI_FORMAT_R32_TYPELESS;
					rawUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
					rawUav.Buffer.NumElements = sizeof(D3D12_DISPATCH_RAYS_DESC) / 4;
					rawUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
					impl->device->CreateUnorderedAccessView(impl->reflectionArgs.resource.Get(), nullptr, &rawUav,
						impl->BindlessCpu(Impl::BindlessReflectionArgsUav));
				}

				if (!classifyOk)
				{
					impl->classifyPso.Reset();
					impl->dispatchRaysSignature.Reset();
					impl->status += L"  ([3] 분류 생성 실패 — 반사를 화면 전체로)";
					OutputDebugStringW(L"[GRenderer] [3] 분류 생성 실패 — 반사 DispatchRays 를 화면 전체로\n");
				}
				else
				{
					wchar_t line[160];
					swprintf_s(line, L"[GRenderer] [3] 분류 준비 (반사 8x8 타일 %u x %u → ExecuteIndirect DispatchRays)\n",
						impl->reflectionTilesX, impl->reflectionTilesY);
					OutputDebugStringW(line);
				}
			}
		}

		// ── [7] 반투명 파이프라인 (Transparent.hlsl, 2026-10-27 렌더 9단계) ──
		//  루트 시그니처(1.1): b0 = 패스 상수, b1 = 그리기마다 루트 상수 4개(인스턴스 · 재질),
		//    t0 = TLAS · t1 = 재질 표 · t2 = RT 인스턴스 표 · t3 = 인스턴스 행렬(패스 업로드 버퍼) — 루트 SRV,
		//    s0 반복 · s1 등장방형 · s2 고정, 입력 레이아웃 + CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED (재질 텍스처도 바인드리스 번호로 읽는다).
		//  PSO: 앞면만, 깊이 테스트 LESS_EQUAL · 쓰기 없음(읽기 전용 DSV), 섞기 없음(굴절 · 반사 · 투과를 셰이더가 직접 합친다).
		//  색 피라미드 PSO 와 하늘이 있어야 만든다. 없으면 투과 물체를 예전처럼 G-buffer 에 불투명으로 그린다.
		if (impl->rtSupported && impl->bindlessHeap && impl->pyramidCopyPso && impl->pyramidDownsamplePso && impl->skyDrawPso)
		{
			bool ok = true;
			D3D12_ROOT_PARAMETER1 params[6] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.ShaderRegister = 1;
			params[1].Constants.Num32BitValues = 4;
			for (UINT i = 2; i < 6; ++i)
			{
				params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[i].Descriptor.ShaderRegister = i - 2;   // t0 ~ t3
			}
			for (D3D12_ROOT_PARAMETER1& param : params)
			{
				if (param.ParameterType != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
					param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
			for (UINT i = 0; i < 3; ++i)
			{
				samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samplers[i].MaxAnisotropy = 1;
				samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
				samplers[i].ShaderRegister = i;
				samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			rsDesc.Desc_1_1.NumParameters = 6;
			rsDesc.Desc_1_1.pParameters = params;
			rsDesc.Desc_1_1.NumStaticSamplers = 3;
			rsDesc.Desc_1_1.pStaticSamplers = samplers;
			rsDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeVersionedRootSignature(&rsDesc, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->transparentRootSig))))
			{
				if (err) OutputDebugStringA(("[Transparent 루트] " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) + "\n").c_str());
				ok = false;
			}

			std::string log;
			const std::wstring path = ShaderPath(L"Transparent.hlsl");
			std::vector<uint8_t> vs, ps;
			if (ok)
			{
				vs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", L"vs_6_6", nullptr, 0, log);
				if (!vs.empty())
					ps = impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSIce", L"ps_6_6", nullptr, 0, log);
				if (vs.empty() || ps.empty())
				{
					OutputDebugStringA(("[Transparent] " + log + "\n").c_str());
					ok = false;
				}
			}
			if (ok)
			{
				D3D12_INPUT_ELEMENT_DESC layout[] = {
					{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, normal)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, color)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, uv)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, tangent)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				};
				D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
				psoDesc.pRootSignature = impl->transparentRootSig.Get();
				psoDesc.InputLayout = { layout, 5 };
				psoDesc.VS = { vs.data(), vs.size() };
				psoDesc.PS = { ps.data(), ps.size() };
				psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
				psoDesc.RasterizerState.DepthClipEnable = TRUE;
				psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				psoDesc.DepthStencilState.DepthEnable = TRUE;
				psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
				psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
				psoDesc.SampleMask = UINT_MAX;
				psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				psoDesc.NumRenderTargets = 1;
				psoDesc.RTVFormats[0] = Impl::kHdrFormat;
				psoDesc.SampleDesc.Count = 1;
				ok = SUCCEEDED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->transparentPso)));
			}
			if (!ok)
			{
				impl->transparentPso.Reset();
				impl->status += L"  (반투명 파이프라인 생성 실패 — 투과 물체를 불투명으로)";
				OutputDebugStringW(L"[GRenderer] [7] 반투명 파이프라인 생성 실패 — 투과 물체를 G-buffer 에 불투명으로\n");
			}
			else
			{
				OutputDebugStringW(L"[GRenderer] [7] 반투명 파이프라인 준비 (화면 색 피라미드 굴절 · 픽셀 셰이더 RayQuery 반사 · 그림자)\n");
			}
		}

		// ── [8] HDR → 백버퍼 파이프라인 (Tonemap.hlsl, 2026-10-27) ──
		//   테이블 = targetHeap 슬롯 6 (SRV t0 HDR). 정점 버퍼 · 깊이 없음.
		{
			D3D12_DESCRIPTOR_RANGE range = {};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = 0;

			D3D12_ROOT_PARAMETER param = {};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &range;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			ComPtr<ID3DBlob> sig, err;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
				FAILED(impl->device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&impl->tonemapRootSig))))
			{
				impl->status = L"톤맵 루트 시그니처 생성 실패.";
				return false;
			}

			const std::wstring path = ShaderPath(L"Tonemap.hlsl");
			std::string log;
			std::vector<uint8_t> vs = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", L"vs_6_0", nullptr, 0, log);
			std::vector<uint8_t> ps = vs.empty() ? std::vector<uint8_t>()
				: impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSMain", L"ps_6_0", nullptr, 0, log);
			if (vs.empty() || ps.empty())
			{
				OutputDebugStringA(("[Tonemap] " + log + "\n").c_str());
				impl->status = L"톤맵 셰이더 컴파일 실패 (출력창 참고).";
				return false;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = impl->tonemapRootSig.Get();
			psoDesc.VS = { vs.data(), vs.size() };
			psoDesc.PS = { ps.data(), ps.size() };
			psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			psoDesc.RasterizerState.DepthClipEnable = TRUE;
			psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			psoDesc.DepthStencilState.DepthEnable = FALSE;
			psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Impl::kBackBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			if (FAILED(impl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&impl->tonemapPso))))
			{
				impl->status = L"톤맵 PSO 생성 실패.";
				return false;
			}

			// ── 10단계 — 최종 톤맵(PSTonemap, 바인드리스) ──
			//  루트 시그니처: 루트 상수 8개(b0) + 정적 샘플러 s0(선형 고정) + CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED.
			//  후처리 컴퓨트 PSO 가 있어야 만든다 — 없으면 예전 PSO(자르기)만 쓴다.
			if (impl->bindlessHeap && impl->histogramPso && impl->exposurePso && impl->bloomDownPso && impl->bloomUpPso)
			{
				D3D12_ROOT_PARAMETER1 constants = {};
				constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				constants.Constants.ShaderRegister = 0;
				constants.Constants.Num32BitValues = sizeof(TonemapConstants) / sizeof(uint32_t);
				constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				D3D12_STATIC_SAMPLER_DESC clampSampler = {};
				clampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				clampSampler.AddressU = clampSampler.AddressV = clampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				clampSampler.MaxAnisotropy = 1;
				clampSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				clampSampler.MaxLOD = D3D12_FLOAT32_MAX;
				clampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				D3D12_VERSIONED_ROOT_SIGNATURE_DESC bindlessRs = {};
				bindlessRs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
				bindlessRs.Desc_1_1.NumParameters = 1;
				bindlessRs.Desc_1_1.pParameters = &constants;
				bindlessRs.Desc_1_1.NumStaticSamplers = 1;
				bindlessRs.Desc_1_1.pStaticSamplers = &clampSampler;
				bindlessRs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
				ComPtr<ID3DBlob> bindlessSig, bindlessErr;
				std::vector<uint8_t> vs66 = impl->shaderCompiler.CompileFromFile(path.c_str(), L"VSMain", L"vs_6_6", nullptr, 0, log);
				std::vector<uint8_t> psTonemap = vs66.empty() ? std::vector<uint8_t>()
					: impl->shaderCompiler.CompileFromFile(path.c_str(), L"PSTonemap", L"ps_6_6", nullptr, 0, log);
				bool tonemapOk = !vs66.empty() && !psTonemap.empty() &&
					SUCCEEDED(D3D12SerializeVersionedRootSignature(&bindlessRs, &bindlessSig, &bindlessErr)) &&
					SUCCEEDED(impl->device->CreateRootSignature(0, bindlessSig->GetBufferPointer(), bindlessSig->GetBufferSize(),
						IID_PPV_ARGS(&impl->tonemapBindlessRootSig)));
				if (tonemapOk)
				{
					D3D12_GRAPHICS_PIPELINE_STATE_DESC bindlessPso = psoDesc;
					bindlessPso.pRootSignature = impl->tonemapBindlessRootSig.Get();
					bindlessPso.VS = { vs66.data(), vs66.size() };
					bindlessPso.PS = { psTonemap.data(), psTonemap.size() };
					tonemapOk = SUCCEEDED(impl->device->CreateGraphicsPipelineState(&bindlessPso, IID_PPV_ARGS(&impl->tonemapBindlessPso)));
				}
				if (!tonemapOk)
				{
					OutputDebugStringA(("[Tonemap PSTonemap] " + log + "\n").c_str());
					impl->tonemapBindlessPso.Reset();
					OutputDebugStringW(L"[GRenderer] [8] 최종 톤맵 파이프라인 생성 실패 — 예전 톤맵(0~1 자르기)\n");
				}
				else
				{
					OutputDebugStringW(L"[GRenderer] [8] 후처리 준비 (블룸 Jimenez 2014 · 자동 노출 히스토그램 UE 기본값 · ACES 근사 · sRGB)\n");
				}
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

			// 깊이는 쓰지도 읽지도 않는다.
			//  ★ 2026-10-27 — UI 는 [8] 에서 HDR 을 옮긴 백버퍼 위에 그린다. 그때 DSV 를 묶지 않으므로 형식도 UNKNOWN.
			psoDesc.DepthStencilState.DepthEnable = FALSE;
			psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Impl::kBackBufferFormat;
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
		// ★ 2026-10-27 렌더 4단계 — 인덱스 시작을 4 B → 16 B 정렬로 바꿨다.
		//   반사 히트 셰이더용 raw SRV(ByteAddressBuffer)는 첫 원소 위치가 16 B 배수여야 한다(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT,
		//   어기면 디버그 레이어 오류 #31 + 장치 제거 — 실제로 났다). 인덱스 버퍼 뷰 · BLAS 는 4 B 만 요구하므로 그대로 된다.
		const UINT64 indexOffset = (UINT64(vbSize) + D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1) & ~UINT64(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1);
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

		// ── 바인드리스 raw SRV 두 칸 (2026-10-27 렌더 4단계) ──
		//  [4b] 반사 히트 셰이더가 맞은 삼각형의 정점 세 개를 ByteAddressBuffer 로 읽는다(RtReflection.hlsl ClosestHit).
		//  정점 · 인덱스는 한 버퍼를 나눠 쓴다: 정점 [0, vbSize), 인덱스 [indexOffset, indexOffset + ibSize).
		//  raw 뷰는 4바이트 단위(R32_TYPELESS)라 시작 · 개수를 4 로 나눈다(정점 60 B · indexOffset 16 B 정렬이라 나누어떨어진다).
		//  시작 위치는 16 B 배수여야 한다 — 정점은 0, 인덱스는 위에서 indexOffset 을 16 B 로 맞췄다.
		//  버퍼는 위에서 RT 장치면 NON_PIXEL_SHADER_RESOURCE 까지 켠 상태로 둔다.
		if (impl->bindlessHeap && impl->rtSupported)
		{
			auto rawSrv = [&](UINT64 offsetBytes, UINT sizeBytes) -> uint32_t
			{
				const uint32_t index = impl->AllocateBindless();
				if (index == Impl::kNoDescriptor) return index;
				D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
				srv.Format = DXGI_FORMAT_R32_TYPELESS;
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Buffer.FirstElement = offsetBytes / 4;
				srv.Buffer.NumElements = sizeBytes / 4;
				srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				impl->device->CreateShaderResourceView(m.buffer.Get(), &srv, impl->BindlessCpu(index));
				return index;
			};
			m.vertexDescriptor = rawSrv(0, vbSize);
			m.indexDescriptor = rawSrv(indexOffset, ibSize);
		}

		impl->meshes.push_back(std::move(m));
		return MeshHandle(impl->meshes.size() - 1);
	}

	TextureHandle GRenderer::CreateTexture(const TextureData& data)
	{
		// ── 2026-10-28 지형 레이어 — 밉 여러 단 · 배열 · R8 을 받는다 (기본값이면 예전 RGBA8 한 장과 같다) ──
		//  pixels 배치 = 배열 원소마다 밉 0 ~ mipLevels−1 차례로, 행은 빈틈 없이(ModelData.h TextureData).
		//  D3D12 서브리소스 번호(밉 + 원소 × 밉 수)도 같은 순서라 그대로 차례로 옮긴다.
		const uint32_t bytesPerPixel = data.format == TextureFormat::R8 ? 1u : 4u;
		uint32_t fullMips = 0;
		for (uint32_t size = (std::max)(data.width, data.height); size > 0; size >>= 1) ++fullMips;
		auto mipWidth = [&](uint32_t mip) { return (std::max)(data.width >> mip, 1u); };
		auto mipHeight = [&](uint32_t mip) { return (std::max)(data.height >> mip, 1u); };
		uint64_t imageBytes = 0;
		const bool shapeValid = data.width > 0 && data.height > 0 &&
			data.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && data.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
			data.mipLevels >= 1 && data.mipLevels <= fullMips &&
			data.arraySize >= 1 && data.arraySize <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
		if (shapeValid)
			for (uint32_t slice = 0; slice < data.arraySize; ++slice)
				for (uint32_t mip = 0; mip < data.mipLevels; ++mip)
					imageBytes += uint64_t(mipWidth(mip)) * mipHeight(mip) * bytesPerPixel;
		if (!impl->device || !impl->fenceEvent || impl->frameOpen || !shapeValid ||
			imageBytes != data.pixels.size() || impl->textures.size() >= kInvalidTexture)
		{
			impl->status = L"텍스처 데이터(크기 · 밉 · 배열 · 바이트 수) 또는 호출 시점이 올바르지 않습니다.";
			return kInvalidTexture;
		}

		Impl::TextureGpu texture;
		texture.format = data.format == TextureFormat::R8 ? DXGI_FORMAT_R8_UNORM
			: (data.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
		texture.mipLevels = data.mipLevels;
		texture.arraySize = data.arraySize;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = data.width;
		desc.Height = data.height;
		desc.DepthOrArraySize = static_cast<UINT16>(data.arraySize);
		desc.MipLevels = static_cast<UINT16>(data.mipLevels);
		desc.Format = texture.format;
		desc.SampleDesc.Count = 1;
		D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		if (FAILED(impl->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource))))
		{
			impl->status = L"GPU 텍스처 생성 실패.";
			return kInvalidTexture;
		}

		const UINT subresources = data.mipLevels * data.arraySize;
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
		std::vector<UINT> rowCounts(subresources);
		std::vector<UINT64> rowSizes(subresources);
		UINT64 uploadBytes = 0;
		impl->device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rowCounts.data(), rowSizes.data(), &uploadBytes);
		bool footprintsMatch = uploadBytes > 0 && uploadBytes <= (std::numeric_limits<size_t>::max)();
		for (UINT s = 0; footprintsMatch && s < subresources; ++s)
		{
			const uint32_t mip = s % data.mipLevels;
			footprintsMatch = rowCounts[s] == mipHeight(mip) && rowSizes[s] == uint64_t(mipWidth(mip)) * bytesPerPixel;
		}
		if (!footprintsMatch)
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
		// GPU 행 간격은 256바이트 정렬이므로 원본 행을 하나씩 복사한다(서브리소스마다).
		size_t source = 0;
		for (UINT s = 0; s < subresources; ++s)
		{
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[s];
			const size_t rowBytes = static_cast<size_t>(rowSizes[s]);
			for (UINT row = 0; row < rowCounts[s]; ++row)
				memcpy(mapped + static_cast<size_t>(footprint.Offset) + size_t(row) * footprint.Footprint.RowPitch,
					data.pixels.data() + source + size_t(row) * rowBytes, rowBytes);
			source += rowBytes * rowCounts[s];
		}
		upload->Unmap(0, nullptr);

		if (!impl->WaitForGpu()) return kInvalidTexture;
		if (FAILED(impl->commandAllocator->Reset()) ||
			FAILED(impl->commandList->Reset(impl->commandAllocator.Get(), nullptr)))
		{
			impl->status = L"텍스처 업로드 명령 초기화 실패.";
			return kInvalidTexture;
		}
		for (UINT s = 0; s < subresources; ++s)
		{
			D3D12_TEXTURE_COPY_LOCATION copySource = {};
			copySource.pResource = upload.Get();
			copySource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			copySource.PlacedFootprint = footprints[s];
			D3D12_TEXTURE_COPY_LOCATION destination = {};
			destination.pResource = texture.resource.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = s;
			impl->commandList->CopyTextureRegion(&destination, 0, 0, 0, &copySource, nullptr);
		}
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.resource.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		// ★ 4단계 — [4b] 반사 히트 셰이더(픽셀 셰이더가 아님)도 이 텍스처를 읽는다 → 읽기 상태 둘을 같이 켠다.
		if (impl->bindlessHeap)
			barrier.Transition.StateAfter |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		impl->commandList->ResourceBarrier(1, &barrier);
		// 복사가 끝날 때까지 upload를 유지한다. 디스크립터는 CreateMaterial에서 만든다.
		if (!impl->FlushCommands(upload, texture.resource)) return kInvalidTexture;

		// UI 로도 그릴 수 있게 UI SRV 힙의 «핸들 번호» 슬롯에 SRV 를 하나 더 만든다(DrawSprites).
		//  UI 셰이더는 Texture2D 라 배열 텍스처(지형 레이어 묶음)는 그 칸을 비워 둔다.
		const UINT slot = UINT(impl->textures.size());
		if (impl->srvHeap && slot < Impl::MaxUiTextures && texture.arraySize == 1)
		{
			const D3D12_SHADER_RESOURCE_VIEW_DESC srv = Impl::TextureSrvDesc(texture);
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = impl->srvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += SIZE_T(slot) * impl->srvDescriptorSize;
			impl->device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
		}

		// ── 바인드리스 SRV 한 칸 (2026-10-27 렌더 4단계) ──
		//  [4b] 반사 히트 셰이더가 ResourceDescriptorHeap[번호] 로 이 텍스처를 읽는다.
		//  번호는 CreateMaterial 이 RT 재질 표에 적는다. 바인드리스를 못 쓰는 장치면 kNoDescriptor 로 남는다.
		if (impl->bindlessHeap)
		{
			texture.bindless = impl->AllocateBindless();
			if (texture.bindless != Impl::kNoDescriptor)
			{
				const D3D12_SHADER_RESOURCE_VIEW_DESC srv = Impl::TextureSrvDesc(texture);
				impl->device->CreateShaderResourceView(texture.resource.Get(), &srv, impl->BindlessCpu(texture.bindless));
			}
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
			!std::isfinite(data.roughness) || !std::isfinite(data.metallic) ||
			// 투과 값 (2026-10-27): attenuationDistance 만 무한대(기본값 = 흡수 없음)를 허용한다.
			!std::isfinite(data.transmission) || !std::isfinite(data.thickness) ||
			std::isnan(data.attenuationDistance) || !(data.attenuationDistance > 0.0f) ||
			!std::isfinite(data.attenuationColor.x) || !std::isfinite(data.attenuationColor.y) ||
			!std::isfinite(data.attenuationColor.z))
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
			// 2026-10-28 — 일반 재질 셰이더는 Texture2D 다. 배열 텍스처(지형 레이어 묶음)는 CreateTerrainMaterial 로만.
			if (impl->textures[resolved[slot]].arraySize != 1)
			{
				impl->status = L"일반 재질에 배열 텍스처를 쓸 수 없습니다(지형 레이어는 CreateTerrainMaterial).";
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
		// ── 알파 마스크 (2026-10-28) — glTF MASK: 알파 < alphaCutoff 면 없는 것. 기준이 0 이하면 다 남으므로 마스크로 다루지 않는다 ──
		material.alphaMask = data.alphaMask && std::isfinite(data.alphaCutoff) && data.alphaCutoff > 0.0f;
		material.constants.alphaCutoff = material.alphaMask ? data.alphaCutoff : 0.0f;
		material.textures = impl->WriteMaterialTextures(impl->materials.size(), resolved);

		// ── 투과 → 그림자 레이가 이 물체 하나를 지나고 남는 빛 (2026-10-27 렌더 2단계) ──
		//  glTF 정의 그대로: 표면 투과(transmission) × 투과광을 물들이는 baseColor
		//                   × 부피 흡수 attenuationColor ^ (thickness / attenuationDistance)  (비어-람베르트)
		//  · baseColor 텍스처는 보지 않는다 — 그림자 레이 히트에서 UV 를 구하지 않는다(재질 상수만).
		//  · 두께는 메시 좌표 단위 그대로 쓴다 — 인스턴스 배율은 반영하지 않는다(재질마다 한 번 계산).
		//  · 표면 반사로 잃는 빛(얼음 정면 약 3.6 %)은 넣지 않았다.
		const float transmission = std::clamp(data.transmission, 0.0f, 1.0f);
		material.transmissive = transmission > 0.0f;
		if (material.transmissive)
		{
			const float pathRatio = (data.thickness > 0.0f && std::isfinite(data.attenuationDistance))
				? data.thickness / data.attenuationDistance : 0.0f;
			auto channel = [&](float base, float attenuation)
			{
				const float volume = pathRatio > 0.0f ? std::pow(std::clamp(attenuation, 0.0f, 1.0f), pathRatio) : 1.0f;
				return transmission * std::clamp(base, 0.0f, 1.0f) * volume;
			};
			material.transmittance = XMFLOAT3(
				channel(data.baseColor.x, data.attenuationColor.x),
				channel(data.baseColor.y, data.attenuationColor.y),
				channel(data.baseColor.z, data.attenuationColor.z));
		}
		// ── RT 재질 표 한 줄 (2026-10-27 렌더 4단계 — 2단계의 float4 «남는 빛» 을 RtMaterialGpu 96 B 로 넓힘) ──
		//  [4a] 그림자 루프(RtCommon.hlsli TraceSunVisibility)는 transmittance 만,
		//  [4b] 반사 히트 셰이더(RtReflection.hlsl ClosestHit)는 상수 · 텍스처 번호까지 전부 읽는다.
		//  텍스처 번호 = 폴백까지 고른(resolved) 텍스처의 바인드리스 힙 번호 → 래스터 재질 테이블과 같은 텍스처를 가리킨다.
		if (impl->rtMaterialData)
		{
			RtMaterialGpu row = {};
			row.baseColor = data.baseColor;
			row.emissive = data.emissive;
			row.roughness = data.roughness;
			row.metallic = data.metallic;
			row.hasNormalMap = material.constants.hasNormalMap;
			row.doubleSided = data.doubleSided ? 1u : 0u;   // 7단계 — GI 프로브 레이의 «벽 속» 판정에서 뺀다
			for (size_t slot = 0; slot < 4; ++slot)   // BaseColor · Normal · Roughness · Metallic (TextureSlot 순서)
				row.textures[slot] = impl->textures[resolved[slot]].bindless;
			row.emissiveTexture = impl->textures[resolved[static_cast<size_t>(TextureSlot::Emissive)]].bindless;
			row.transmittance = material.transmittance;
			row.transmissive = material.transmissive ? 1u : 0u;
			// 9단계 — [7] 반투명 패스용. 흡수 거리가 무한대(흡수 없음)면 0 으로 적어 셰이더가 «흡수 없음» 으로 읽게 한다.
			row.transmission = std::clamp(data.transmission, 0.0f, 1.0f);
			row.ior = std::isfinite(data.ior) && data.ior >= 1.0f ? data.ior : 1.5f;
			row.thickness = (std::max)(data.thickness, 0.0f);
			row.attenuationDistance = std::isfinite(data.attenuationDistance) ? data.attenuationDistance : 0.0f;
			row.attenuationColor = data.attenuationColor;
			row.alphaCutoff = material.constants.alphaCutoff;   // 2026-10-28 — 레이 후보의 알파 판정(RtCommon.hlsli AlphaMaskedOut)
			memcpy(impl->rtMaterialData + impl->materials.size() * sizeof(RtMaterialGpu), &row, sizeof(row));
		}

		impl->materials.push_back(material);
		return static_cast<MaterialHandle>(impl->materials.size() - 1);
	}

	// ── 지형 레이어 재질 (2026-10-28) ──
	//  래스터: 묶음 키 kTerrainPsoKey 의 PSO(Forward.hlsl TERRAIN_LAYERS=1)가 텍스처 5칸을 «배열 4개 + 마스크» 로,
	//          루트 상수 20 DWORD 를 TerrainConstants 로 읽는다(4단계에서 12 → 20).
	//  RT   : 재질 표 한 줄에 terrain = 1 을 적고 칸 몇 개를 다른 뜻으로 쓴다(RtMaterialGpu 주석) — RtSurface.hlsli FetchHitSurface.
	//  그림자 · AO 레이는 지형을 보통 불투명 물체로 본다(투과 · 알파 없음).
	MaterialHandle GRenderer::CreateTerrainMaterial(const TerrainMaterialData& data, TextureHandle baseColor,
		TextureHandle normal, TextureHandle orm, TextureHandle height, TextureHandle layerMask)
	{
		const std::array<TextureHandle, kMaterialTextureCount> slots{ baseColor, normal, orm, height, layerMask };
		auto fail = [&](const wchar_t* why)
		{
			impl->status = std::wstring(L"지형 레이어 재질: ") + why;
			return kInvalidMaterial;
		};
		if (!impl->materialHeap || impl->frameOpen || impl->materials.size() >= Impl::MaxMaterials)
			return fail(L"호출 시점 또는 재질 개수가 올바르지 않습니다.");
		for (float value : data.heightBlend)
			if (!std::isfinite(value)) return fail(L"높이 계수가 유한하지 않습니다.");
		if (!std::isfinite(data.blendDepth) || !(data.blendDepth > 0.0f) || !std::isfinite(data.normalStrength))
			return fail(L"경계 폭(0 보다 커야 함) 또는 노멀 세기가 올바르지 않습니다.");
		if (!(data.triplanarTileA > 0.0f) || !(data.triplanarTileB > 0.0f) || !std::isfinite(data.triplanarTileA) ||
			!std::isfinite(data.triplanarTileB) || !(data.warpScale >= 0.0f) || !std::isfinite(data.warpScale) ||
			!std::isfinite(data.warpAmount) || data.triplanarLayers > 0xFu)
			return fail(L"세 방향 투영(타일 크기 > 0 · 레이어 비트 4개) 또는 좌표 비틀기 값이 올바르지 않습니다.");
		if (!std::isfinite(data.macroAmount) || !std::isfinite(data.macroBias) || !std::isfinite(data.bumpDistance) ||
			!std::isfinite(data.reliefDistance) || !(data.reliefStrength >= 0.0f) || !(data.reliefStrength <= 1.0f))
			return fail(L"4단계 값(색 흔들림 · 범프 거리 · 범프 세기 0 ~ 1)이 올바르지 않습니다.");
		for (TextureHandle handle : slots)
			if (handle >= impl->textures.size()) return fail(L"등록되지 않은 GPU 텍스처를 참조합니다.");

		// 텍스처 모양 — 레이어 묶음 넷은 원소 4개 · 같은 크기 · 같은 밉 수, 형식은 슬롯마다 정해져 있다(셰이더가 그렇게 읽는다).
		const Impl::TextureGpu& first = impl->textures[baseColor];
		const DXGI_FORMAT expected[4] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM };
		for (size_t slot = 0; slot < 4; ++slot)
		{
			const Impl::TextureGpu& texture = impl->textures[slots[slot]];
			const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
			const D3D12_RESOURCE_DESC firstDesc = first.resource->GetDesc();
			if (texture.arraySize != 4 || texture.format != expected[slot] || texture.mipLevels != first.mipLevels ||
				desc.Width != firstDesc.Width || desc.Height != firstDesc.Height)
				return fail(L"레이어 텍스처는 원소 4개 배열이고 크기 · 밉 수가 같아야 합니다(색 sRGB · 노멀 · ORM RGBA · 높이 R8).");
		}
		const Impl::TextureGpu& mask = impl->textures[layerMask];
		if (mask.arraySize != 1 || mask.format != DXGI_FORMAT_R8G8B8A8_UNORM)
			return fail(L"레이어 마스크는 RGBA8(UNORM) 한 장이어야 합니다.");

		Impl::MaterialGpu material;
		material.terrain = true;
		TerrainConstants constants{};
		constants.heightBlend = XMFLOAT4(data.heightBlend[0], data.heightBlend[1], data.heightBlend[2], data.heightBlend[3]);
		constants.blendDepth = data.blendDepth;
		constants.normalStrength = data.normalStrength;
		constants.triplanarLayers = data.triplanarLayers;
		constants.triplanarTileA = data.triplanarTileA;
		constants.triplanarTileB = data.triplanarTileB;
		constants.triplanarOffsetB = data.triplanarOffsetB ? 1.0f : 0.0f;
		constants.warpScale = data.warpScale;
		constants.warpAmount = data.warpAmount;
		constants.macroAmount = data.macroAmount;         // 4단계
		constants.macroBias = data.macroBias;
		constants.bumpDistance = data.bumpDistance;
		constants.reliefDistance = data.reliefDistance;
		constants.reliefStrength = data.reliefStrength;
		material.terrainConstants = constants;
		material.textures = impl->WriteMaterialTextures(impl->materials.size(), slots);

		if (impl->rtMaterialData)
		{
			RtMaterialGpu row = {};
			row.terrain = 1;
			row.baseColor = constants.heightBlend;          // 지형: 레이어별 높이 계수
			row.roughness = data.blendDepth;                // 지형: 경계 폭
			row.metallic = data.normalStrength;             // 지형: 노멀 세기 (RT 히트는 노멀맵을 안 쓰지만 같이 적는다)
			row.emissive = XMFLOAT3(data.macroAmount, data.macroBias, 0.0f);   // 지형: 4단계 색 흔들림 세기 · 바탕(발광 아님)
			for (size_t slot = 0; slot < 4; ++slot)         // 배열 4개: 색 · 노멀 · ORM · 높이
				row.textures[slot] = impl->textures[slots[slot]].bindless;
			row.emissiveTexture = mask.bindless;            // 지형: 레이어 마스크
			row.alphaCutoff = 0.0f;                         // 알파 마스크 아님 — 레이 후보 판정에 오지 않는다
			memcpy(impl->rtMaterialData + impl->materials.size() * sizeof(RtMaterialGpu), &row, sizeof(row));
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
		++impl->frameCounter;   // 프레임 번호 — 시간 누적 · 노이즈 시드용 자리(2026-10-27 룰렛이 빠져 지금은 읽는 곳 없음)
		impl->commandAllocator->Reset();
		impl->commandList->Reset(impl->commandAllocator.Get(), nullptr);
		impl->timestampCount = 0;
		impl->MarkGpuTime(L"프레임 시작(지우기)");
		impl->resolved = false;
		impl->skyDoneThisFrame = false;   // 6단계 — 이번 프레임의 첫 Render 가 해 · 하늘을 처리한다
		// 10단계 — 프레임 시간(자동 노출 적응 속도가 «스톱/초» 라서). 첫 프레임 · 멈췄다 돌아온 프레임은 0.1 초로 자른다.
		{
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			if (impl->lastFrameCounter.QuadPart != 0)
				impl->frameDeltaSeconds = (std::min)(0.1f, float(double(now.QuadPart - impl->lastFrameCounter.QuadPart) / double(frequency.QuadPart)));
			impl->lastFrameCounter = now;
		}

		// ── 장면은 HDR 컬러에 그린다 (2026-10-27 파이프라인 1단계) ──
		//  백버퍼는 여기서 건드리지 않는다. [8](ResolveToBackBuffer)에서 HDR 을 옮길 때 처음 쓴다.
		impl->TransitionTargets({
			{ &impl->hdr,   D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_WRITE } });
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = impl->Rtv(Impl::kRtvHdr);
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = impl->Dsv();
		impl->commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

		impl->commandList->ClearRenderTargetView(rtv, Impl::kClearColor, 0, nullptr);
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

		// ── [6] 해 · 하늘 (2026-10-27 렌더 6단계) — 프레임의 첫 Render 에서 한 번(배경이 먼저면 배경에서) ──
		//  땅에 닿는 태양 조도를 구하고(아래 모든 상수가 쓴다), HDR 컬러 전체에 하늘을 그린다.
		//  2026-10-28 — 메인 화면의 위쪽(엔진이 넣는 행성 기준 위쪽)을 기억해 둔다. 배경 패스가 먼저 오는 프레임은
		//  배경 view 에 위쪽이 없어(SkyPlanet 이 RenderView{} 로 만듦 → 기본값 (0, 1, 0)) 하늘이 월드 +Y 기준 «낮» 으로
		//  계산됐다. 이제 하늘 · 해 높이는 늘 이 행성 기준 위쪽으로 계산한다(UpdateSunAndSky).
		if (!view.background)
		{
			impl->mainViewUp = view.up;
			impl->hasMainViewUp = true;
		}
		impl->MarkGpuTime(L"[6] 하늘");
		impl->UpdateSunAndSky(view);

		// ── TLAS 재빌드 (패킷 인스턴스 → TLAS 인스턴스) ──
		if (rtActive)
		{
			// 프레임 슬롯을 넘긴다 — 비행 중 프레임(M1)에서 인스턴스 버퍼 덮어쓰기를 막는다.
			impl->accel.ResetInstances(impl->frameIndex);
			// ★ 4단계 — RT 인스턴스 표도 같은 프레임 슬롯에 쓴다(AccelStructure::ResetInstances 와 같은 규칙).
			uint8_t* const instanceRows = impl->rtInstanceData
				? impl->rtInstanceData + size_t(impl->frameIndex % AccelStructure::kFrameSlots) * Impl::MaxInstances * sizeof(RtInstanceGpu)
				: nullptr;
			for (const InstanceData& it : items)
			{
				// 행렬식 ≈ 0 인 인스턴스는 래스터처럼 RT 에서도 뺀다.
				if (it.mesh >= impl->meshes.size() || (it.flags & kInstanceDegenerate)) continue;
				const Impl::MeshGpu& mesh = impl->meshes[it.mesh];
				const uint32_t blasIndex = mesh.blasIndex;
				if (blasIndex == AccelStructure::kInvalidBlas) continue;
				// ★ 2026-10-27 렌더 2단계 — InstanceID = 재질 번호(그림자 셰이더가 재질 표를 찾는다),
				//   투과 재질이면 FORCE_NON_OPAQUE 로 넣어 그림자 레이가 통과 후보로 본다.
				const MaterialHandle material = it.material < impl->materials.size() ? it.material : 0;
				const uint32_t index = impl->accel.InstanceCount();
				// 투과 · 알파 마스크(2026-10-28) 재질은 FORCE_NON_OPAQUE — 레이가 후보로 받아 남는 빛 · 빈 곳을 처리한다
				impl->accel.AddInstance(blasIndex, it.world, material,
					impl->materials[material].transmissive || impl->materials[material].alphaMask);
				// ★ 2026-10-27 렌더 4단계 — 들어간 인스턴스와 같은 순번으로 인스턴스 표 한 줄.
				//   반사 히트 셰이더가 InstanceIndex() 로 이 줄을 찾아 정점 · 인덱스 버퍼(바인드리스 번호)와 재질을 안다.
				//   AddInstance 가 상한으로 버렸으면(개수가 안 늘었으면) 쓰지 않는다.
				if (instanceRows && impl->accel.InstanceCount() > index)
				{
					const RtInstanceGpu row{ mesh.vertexDescriptor, mesh.indexDescriptor, material, 0 };
					memcpy(instanceRows + size_t(index) * sizeof(RtInstanceGpu), &row, sizeof(row));
				}
			}
			// ★ 2026-10-27 렌더 3단계 — [0] 빌드는 비동기 컴퓨트 큐로. 직접 큐는 G-buffer 뒤(JoinAsyncTlasBuild)에서 기다린다.
			//   컴퓨트 큐가 없거나 제출에 실패하면 예전처럼 이 직접 리스트에서 빌드한다.
			if (!impl->asyncAs || !impl->SubmitTlasBuildAsync())
				impl->accel.BuildTlas(commandList);
		}

		// ── 프레임 상수 ──
		FrameConstants fc = {};
		XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.viewProj)));
		fc.eyePos = view.eyePosition;
		fc.sunDir = impl->sunDir;
		fc.debugMode = impl->debugMode;
		// ★ 5단계 — 이전 프레임 viewProj (모션 벡터). 메인 패스만 지난 프레임 값을 쓰고,
		//   첫 프레임 · 배경 패스(G-buffer 를 안 씀)는 이번 값과 같게 둔다 → 카메라 몫 모션 벡터 0.
		const XMFLOAT4X4 prevViewProj = (!view.background && impl->hasPrevMainView) ? impl->prevMainViewProj : view.viewProj;
		XMStoreFloat4x4(&fc.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&prevViewProj)));
		fc.sunIlluminance = impl->sunIlluminance;   // 6단계 — 배경 패스 PSMain 의 태양
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
				const Impl::MaterialGpu& material = impl->materials[materialOf(it)];
				// 2026-10-28 지형 레이어 판 — 단면 · 알파 없음, 반전만 나눈다(키 8 · 9)
				if (material.terrain)
					return Impl::kTerrainPsoKey + ((it.flags & kInstanceMirrored) ? 1u : 0u);
				return (material.doubleSided ? 1u : 0u) |
					((it.flags & kInstanceMirrored) ? 2u : 0u) |
					(material.alphaMask ? 4u : 0u);   // 2026-10-28 — 알파 마스크 판 PSO
			};

			std::vector<uint32_t>& order = impl->batchOrder;
			order.clear();
			for (size_t i = 0; i < usable; ++i)
			{
				const InstanceData& it = items[i];
				if (it.mesh >= impl->meshes.size() || (it.flags & kInstanceDegenerate)) continue;
				// ★ 9단계 — 투과 재질(얼음)은 메인 G-buffer 에서 뺀다. 라이팅 뒤 [7] 반투명 패스가 그린다.
				//   (반투명 파이프라인이 없는 장치는 예전처럼 여기서 불투명으로 그린다)
				if (!view.background && impl->transparentPso && impl->materials[materialOf(it)].transmissive) continue;
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
		impl->MarkGpuTime(L"GPU 컬링");
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

		// ── 래스터 공통 바인딩 — 배경 forward 와 메인 G-buffer 가 같은 루트 시그니처를 쓴다 ──
		//  (렌더 타깃을 묶은 뒤 부른다. PSO 는 DrawBatches 가 묶음마다 고른다)
		auto bindRasterRoot = [&]()
		{
			commandList->SetGraphicsRootSignature(impl->rootSig.Get());
			ID3D12DescriptorHeap* heaps[] = { impl->materialHeap.Get() };
			commandList->SetDescriptorHeaps(1, heaps);
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			commandList->SetGraphicsRootConstantBufferView(1, impl->frameCB->GetGPUVirtualAddress() + frameOffset);
			commandList->SetGraphicsRootShaderResourceView(4, pass.instances->GetGPUVirtualAddress());
			commandList->SetGraphicsRootShaderResourceView(5, pass.visible->GetGPUVirtualAddress());
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = impl->Dsv();
		const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = impl->Rtv(Impl::kRtvHdr);

		if (view.background)
		{
			impl->MarkGpuTime(L"배경 패스");
			// ── 배경 패스 — forward 로 HDR 컬러에 바로 그린다 ──
			//  자기 투영(넓은 near/far)을 쓰고 끝나면 깊이를 지우므로 메인 G-buffer 와 섞지 않는다.
			impl->TransitionTargets({
				{ &impl->hdr,   D3D12_RESOURCE_STATE_RENDER_TARGET },
				{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_WRITE } });
			commandList->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);
			bindRasterRoot();
			// ④ 묶음마다 ExecuteIndirect 한 번 — 인스턴스 수는 컬링 셰이더가 채웠다
			ID3D12PipelineState* const forwardPsos[Impl::kPsoKeyCount] = {
				impl->pso.Get(), impl->doubleSidedPso.Get(), impl->mirroredPso.Get(), impl->mirroredDoubleSidedPso.Get(),
				impl->forwardMaskedPsos[0].Get(), impl->forwardMaskedPsos[1].Get(),
				impl->forwardMaskedPsos[2].Get(), impl->forwardMaskedPsos[3].Get(),
				impl->forwardTerrainPsos[0].Get(), impl->forwardTerrainPsos[1].Get() };   // 8 · 9 지형 레이어(2026-10-28)
			impl->DrawBatches(pass, forwardPsos);

			// 먼 행성 내부의 깊이 비교는 유지하고 이후 맵/캐릭터가 항상 앞에 놓이게 한다.
			// 배경 패스의 넓은 near/far 범위를 메인 맵의 깊이 범위와 섞지 않는다.
			commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			return;
		}

		// ── [1] G-buffer — 메인 패스는 조명 없이 표면 정보만 쓴다 (Forward.hlsl PSGBuffer) ──
		impl->MarkGpuTime(L"[1] G-buffer");
		//  G-buffer 는 지우지 않는다. 깊이가 1 인 픽셀(물체 없음)은 뒤 단계가 읽지 않기 때문이다.
		//  ★ 5단계 — GB4 모션 벡터까지 5장(RTV 가 연속이라 범위 한 번으로 묶인다).
		impl->TransitionTargets({
			{ &impl->gbuffer[Impl::GbAlbedo],   D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->gbuffer[Impl::GbNormal],   D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->gbuffer[Impl::GbMaterial], D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->gbuffer[Impl::GbEmissive], D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->gbuffer[Impl::GbMotion],   D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->depth,                     D3D12_RESOURCE_STATE_DEPTH_WRITE } });
		const D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtv = impl->Rtv(Impl::kRtvGBuffer);
		commandList->OMSetRenderTargets(Impl::GbCount, &gbufferRtv, TRUE, &dsv);   // 연속 5장(GB0~GB4)을 한 번에
		bindRasterRoot();
		ID3D12PipelineState* gbufferPsos[Impl::kPsoKeyCount] = {};
		for (uint32_t key = 0; key < Impl::kPsoKeyCount; ++key)
			gbufferPsos[key] = impl->gbufferPsos[key].Get();   // 0 ~ 7 일반 · 알파 마스크, 8 · 9 지형 레이어(2026-10-28)
		impl->DrawBatches(pass, gbufferPsos);

		// ── [0] 의 짝 — 여기까지(프레임 시작 · 컬링 · G-buffer)를 먼저 GPU 에 보내 컴퓨트 큐의 TLAS 빌드와 겹치게 하고,
		//    여기부터(TLAS 를 읽는 [4a])는 그 빌드가 끝난 뒤 돌게 한다. 비동기로 안 보냈으면 아무것도 안 한다.
		impl->MarkGpuTime(L"[0] TLAS 대기");
		impl->JoinAsyncTlasBuild();

		// G-buffer · 깊이를 컴퓨트가 읽는 상태로. 깊이는 DEPTH_READ 와 셰이더 읽기를 같이 켠다(둘 다 읽기 상태라 함께 둘 수 있다).
		impl->TransitionTargets({
			{ &impl->gbuffer[Impl::GbAlbedo],   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->gbuffer[Impl::GbNormal],   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->gbuffer[Impl::GbMaterial], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->gbuffer[Impl::GbEmissive], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->gbuffer[Impl::GbMotion],   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } });

		const XMMATRIX invViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&view.viewProj)));
		ID3D12DescriptorHeap* targetHeaps[] = { impl->targetHeap.Get() };
		const UINT groupsX = (impl->width + Impl::LightingGroupSize - 1) / Impl::LightingGroupSize;
		const UINT groupsY = (impl->height + Impl::LightingGroupSize - 1) / Impl::LightingGroupSize;

		// ── [4c] GI 프로브 갱신 (ProbeGI.hlsl, 2026-10-27 렌더 7단계) ──
		//  G-buffer 와 상관없이 프로브 자리에서 레이를 쏜다 — TLAS 만 있으면 된다(위에서 빌드 대기를 끝냈다).
		//  반사([4b])보다 먼저 돌려 반사 히트도 이번 프레임 프로브를 읽게 한다.
		//  돌지 않으면(RT 끔 · 미지원 · 인스턴스 0) 라이팅 · 반사가 하늘 조도로 주변광을 대신한다(probeVolume.enabled = 0).
		impl->MarkGpuTime(L"[4c] GI");
		const bool giPass = rtActive && impl->giTracePso && impl->bindlessHeap && !impl->bindlessOverflow &&
			impl->skyValid && impl->accel.InstanceCount() > 0;
		if (giPass)
		{
			impl->UpdateProbes(view);
		}
		else
		{
			impl->probeVolume.enabled = 0;
			impl->probeVolumeFar.enabled = 0;   // 2026-10-28 두 겹 (처음엔 중괄호 없이 붙여 매 프레임 먼 겹이 꺼졌다 — 고침)
		}

		// ── [4a] RT 그림자 — 픽셀마다 태양으로 그림자 레이 1개 (RtShadow.hlsl, 2026-10-27 렌더 2단계) ──
		//  TLAS 는 이 함수 맨 앞에서 이번 프레임 인스턴스로 다시 빌드했다(UAV 배리어까지 끝남).
		//  돌지 않으면(RT 미지원 · R 키로 끔 · 인스턴스 0) 라이팅이 그림자 마스크를 안 읽는다(shadowEnabled = 0).
		impl->MarkGpuTime(L"[4a] 그림자");
		//  2026-10-28 — 바인드리스(컴퓨트 공용 루트)로 옮겼다: 알파 마스크 후보 판정이 재질 텍스처 · 정점을 읽는다.
		const bool shadowPass = rtActive && impl->rtShadowPso && impl->bindlessHeap && !impl->bindlessOverflow &&
			impl->accel.InstanceCount() > 0;
		if (shadowPass)
		{
			impl->TransitionTargets({ { &impl->shadowMask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });

			ShadowConstants sc = {};
			XMStoreFloat4x4(&sc.invViewProj, invViewProj);
			sc.eyePos = view.eyePosition;
			sc.maxDistance = Impl::kShadowMaxDistance;
			sc.sunDir = impl->sunDir;
			sc.width = impl->width;
			sc.height = impl->height;
			sc.normalIndex = Impl::BindlessGbNormal;
			sc.depthIndex = Impl::BindlessDepth;
			sc.shadowUav = Impl::BindlessShadowMaskUav;
			memcpy(impl->shadowCBData, &sc, sizeof(sc));

			// 힙을 먼저, 루트 시그니처를 나중에(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED 사양 — [4b] 와 같은 순서)
			ID3D12DescriptorHeap* shadowHeaps[] = { impl->bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, shadowHeaps);
			commandList->SetComputeRootSignature(impl->computeRootSig.Get());
			commandList->SetPipelineState(impl->rtShadowPso.Get());
			commandList->SetComputeRootConstantBufferView(0, impl->shadowCB->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(1, impl->accel.TlasAddress());
			commandList->SetComputeRootShaderResourceView(2, impl->rtMaterials->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(3, impl->rtInstances->GetGPUVirtualAddress() +
				UINT64(impl->frameIndex % AccelStructure::kFrameSlots) * Impl::MaxInstances * sizeof(RtInstanceGpu));
			commandList->Dispatch(groupsX, groupsY, 1);
		}

		// ── [4b] RT 반사 — 픽셀마다 반사 레이 1개 (RtReflection.hlsl, DispatchRays, 2026-10-27 렌더 4단계) ──
		//  RayGen 이 G-buffer 법선 · 거칠기 · 깊이를 바인드리스로 읽고, 맞은 물체는 ClosestHit 이 그 자리에서 셰이딩한다.
		//  돌지 않으면(RT 미지원 · R 키로 끔 · SM 6.6 아님 · 힙 넘침 · 인스턴스 0) 라이팅이 반사 텍스처를 안 읽는다(reflectionEnabled = 0).
		//  [4a] 와 쓰는 텍스처가 달라서(그림자 마스크 / 반사 텍스처) 둘 사이에 배리어가 필요 없다.
		impl->MarkGpuTime(L"[3][4b][4d] 반사");
		const bool reflectionPass = rtActive && impl->rtReflectionPso && impl->bindlessHeap && !impl->bindlessOverflow &&
			impl->accel.InstanceCount() > 0;
		if (reflectionPass)
		{
			impl->TransitionTargets({ { &impl->reflection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });

			// ── [3] 분류 — 반사 레이가 필요한 8×8 타일 목록 + DispatchRays 간접 인자 (ReflectionClassify.hlsl, 2026-10-27 렌더 11단계) ──
			//  ① 인자 원본(Width 0)을 인자 버퍼로 복사 → ② 타일마다 판정: 고르면 Width += 64 · 목록에 적고, 안 고르면 그 64 픽셀 반사에 0
			//  → ③ 아래 [4b] 가 ExecuteIndirect 로 «고른 타일 수 × 64» 만큼만 RayGen 을 띄운다.
			//  분류가 쓰는 0 과 RayGen 이 쓰는 값은 서로 다른 픽셀이라 둘 사이에 반사 텍스처 UAV 배리어가 필요 없다.
			const bool classifyPass = impl->classifyPso && impl->dispatchRaysSignature;
			if (classifyPass)
			{
				impl->TransitionTargets({ { &impl->reflectionArgs, D3D12_RESOURCE_STATE_COPY_DEST } });
				commandList->CopyBufferRegion(impl->reflectionArgs.resource.Get(), 0, impl->reflectionArgsTemplate.Get(), 0,
					sizeof(D3D12_DISPATCH_RAYS_DESC));
				impl->TransitionTargets({
					{ &impl->reflectionArgs,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
					{ &impl->reflectionTiles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });

				ClassifyConstants cc = {};
				cc.width = impl->width;
				cc.height = impl->height;
				cc.maxRoughness = Impl::kReflectionMaxRoughness;   // RayGen 과 같은 문턱
				cc.depthIndex = Impl::BindlessDepth;
				cc.materialIndex = Impl::BindlessGbMaterial;
				cc.reflectionUav = Impl::BindlessReflectionUav;
				cc.tileListUav = Impl::BindlessReflectionTileUav;
				cc.argsUav = Impl::BindlessReflectionArgsUav;
				cc.argsWidthOffset = uint32_t(offsetof(D3D12_DISPATCH_RAYS_DESC, Width));
				memcpy(impl->classifyCBData, &cc, sizeof(cc));

				ID3D12DescriptorHeap* classifyHeaps[] = { impl->bindlessHeap.Get() };
				commandList->SetDescriptorHeaps(1, classifyHeaps);
				commandList->SetComputeRootSignature(impl->computeRootSig.Get());
				commandList->SetComputeRootConstantBufferView(0, impl->classifyCB->GetGPUVirtualAddress());
				commandList->SetPipelineState(impl->classifyPso.Get());
				commandList->Dispatch(impl->reflectionTilesX, impl->reflectionTilesY, 1);   // 그룹 하나 = 타일 하나

				impl->TransitionTargets({
					{ &impl->reflectionArgs,  D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT },
					{ &impl->reflectionTiles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } });
			}

			ReflectionConstants rc = {};
			XMStoreFloat4x4(&rc.invViewProj, invViewProj);
			rc.eyePos = view.eyePosition;
			rc.maxDistance = Impl::kShadowMaxDistance;   // 반사 레이 길이도 그림자와 같은 상한(행성 지름보다 길다)
			rc.sunDir = impl->sunDir;
			rc.maxRoughness = Impl::kReflectionMaxRoughness;
			rc.width = impl->width;
			rc.height = impl->height;
			rc.normalIndex = Impl::BindlessGbNormal;
			rc.materialIndex = Impl::BindlessGbMaterial;
			rc.depthIndex = Impl::BindlessDepth;
			rc.outputIndex = Impl::BindlessReflectionUav;
			rc.frameIndex = impl->frameCounter;   // 5단계 — B 방식 난수 씨앗 (프레임마다 다른 방향)
			rc.skyIndex = Impl::BindlessSkySrv;                       // 6단계 — 미스 = [6] 하늘 텍스처
			rc.sunIlluminance = impl->sunIlluminance;                 // 6단계 — 맞은 점의 태양
			rc.skyIrradianceIndex = Impl::BindlessSkyIrradianceSrv;   // 6단계 — 맞은 점의 주변광(프로브를 못 쓸 때)
			rc.probeVolume = impl->probeVolume;                       // 7단계 — 맞은 점의 주변광 = GI 프로브
			rc.probeVolumeFar = impl->probeVolumeFar;                 // 2026-10-28 — 먼 겹
			rc.probeIrradianceIndex = Impl::BindlessProbeIrradianceSrv;
			rc.probeDistanceIndex = Impl::BindlessProbeDistanceSrv;
			rc.probeStateIndex = Impl::BindlessProbeStateSrv;
			rc.tileListIndex = classifyPass ? uint32_t(Impl::BindlessReflectionTileSrv) : Impl::kNoDescriptor;   // 11단계
			memcpy(impl->reflectionCBData, &rc, sizeof(rc));

			// ★ 순서: 힙을 먼저 묶고 루트 시그니처를 묶는다 — CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED 루트 시그니처는
			//   SetDescriptorHeaps 를 루트 시그니처 설정보다 먼저 해야 한다(SM 6.6 Dynamic Resources 사양).
			ID3D12DescriptorHeap* bindlessHeaps[] = { impl->bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, bindlessHeaps);
			commandList->SetComputeRootSignature(impl->rtReflectionRootSig.Get());
			commandList->SetPipelineState1(impl->rtReflectionPso.Get());
			commandList->SetComputeRootConstantBufferView(0, impl->reflectionCB->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(1, impl->accel.TlasAddress());
			commandList->SetComputeRootShaderResourceView(2, impl->rtMaterials->GetGPUVirtualAddress());
			commandList->SetComputeRootShaderResourceView(3, impl->rtInstances->GetGPUVirtualAddress() +
				UINT64(impl->frameIndex % AccelStructure::kFrameSlots) * Impl::MaxInstances * sizeof(RtInstanceGpu));

			// 11단계 — 분류가 돌았으면 GPU 가 채운 인자로(레이트레이싱 PSO 를 위에서 묶어 둔 상태여야 한다 — 사양),
			//  아니면 예전처럼 화면 크기 그대로. 셰이더 테이블 배치는 ReflectionDispatchDesc 한 곳.
			if (classifyPass)
			{
				commandList->ExecuteIndirect(impl->dispatchRaysSignature.Get(), 1, impl->reflectionArgs.resource.Get(), 0, nullptr, 0);
			}
			else
			{
				const D3D12_DISPATCH_RAYS_DESC rays = impl->ReflectionDispatchDesc(impl->width, impl->height);
				commandList->DispatchRays(&rays);
			}

			// ── [4d] 반사 노이즈 제거 — 임시 구현 (RtReflectionDenoise.hlsl, 2026-10-27 렌더 5단계) ──
			//  ① 시간 누적: 반사(노이즈 제거 전) + 지난 이력 → 이번 이력   ② 공간 필터: 이번 이력 → 결과(라이팅 t7)
			//  이력 두 장은 프레임마다 번갈아 쓴다: 이번에 쓰는 것 = historyIndex, 지난 프레임 것 = 1 - historyIndex.
			const uint32_t cur  = impl->historyIndex;
			const uint32_t prev = 1u - cur;
			impl->TransitionTargets({
				{ &impl->reflection,              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },   // 레이 결과 → 읽기
				{ &impl->reflectionHistory[cur],  D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
				{ &impl->reflectionHistory[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } });

			DenoiseConstants dc = {};
			XMStoreFloat4x4(&dc.invViewProj, invViewProj);
			XMStoreFloat4x4(&dc.prevInvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&prevViewProj))));
			dc.eyePos = view.eyePosition;
			dc.maxRoughness = Impl::kReflectionMaxRoughness;
			dc.width = impl->width;
			dc.height = impl->height;
			dc.historyValid = impl->historyValid ? 1u : 0u;
			dc.rawIndex = Impl::BindlessReflectionSrv;
			dc.motionIndex = Impl::BindlessMotion;
			dc.depthIndex = Impl::BindlessDepth;
			dc.normalIndex = Impl::BindlessGbNormal;
			dc.materialIndex = Impl::BindlessGbMaterial;
			dc.prevDepthIndex = Impl::BindlessPrevDepth;
			dc.prevNormalIndex = Impl::BindlessPrevNormal;
			dc.historyInIndex = prev == 0 ? Impl::BindlessHistorySrv0 : Impl::BindlessHistorySrv1;
			dc.historyOutUav  = cur == 0 ? Impl::BindlessHistoryUav0 : Impl::BindlessHistoryUav1;
			dc.historyOutSrv  = cur == 0 ? Impl::BindlessHistorySrv0 : Impl::BindlessHistorySrv1;
			dc.outputUav = Impl::BindlessDenoisedUav;
			memcpy(impl->denoiseCBData, &dc, sizeof(dc));

			// 바인드리스 힙은 위에서 묶은 그대로 — 루트 시그니처만 바꾼다(힙이 먼저 묶여 있어야 한다는 순서는 지켜진다).
			commandList->SetComputeRootSignature(impl->denoiseRootSig.Get());
			commandList->SetComputeRootConstantBufferView(0, impl->denoiseCB->GetGPUVirtualAddress());
			commandList->SetPipelineState(impl->denoiseTemporalPso.Get());
			commandList->Dispatch(groupsX, groupsY, 1);

			impl->TransitionTargets({
				{ &impl->reflectionHistory[cur], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
				{ &impl->reflectionDenoised,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			commandList->SetPipelineState(impl->denoiseSpatialPso.Get());
			commandList->Dispatch(groupsX, groupsY, 1);

			impl->historyIndex = prev;   // 다음 프레임은 반대쪽에 쓴다
			// (8단계 — 이전 깊이 · 법선 사본 뜨기는 아래 [2] AO 뒤로 옮겼다. AO 시간 누적도 같은 사본을 읽는다)
		}
		// 반사가 안 돈 프레임이 끼면 이력 · 사본이 끊긴다 → 다음에 돌 때 새로 시작한다.
		impl->historyValid = reflectionPass;

		// ── [2] AO — 화면 공간 추적 + 모르는 레이만 RayQuery (AO.hlsl, 2026-10-27 렌더 8단계) ──
		//  ① 레이(픽셀마다 1개) → ② 시간 누적(이력 두 장 번갈아) → ③ 공간 필터 → AO 텍스처(라이팅 t13).
		//  RT 가 꺼져 있으면(R 키 · 미지원 · 인스턴스 0) «모름» 레이를 안 가려짐으로 본다 = 순수 SSAO. 바인드리스만 있으면 돈다.
		impl->MarkGpuTime(L"[2] AO");
		const bool aoPass = impl->aoTracePso && impl->bindlessHeap && !impl->bindlessOverflow;
		if (aoPass)
		{
			const uint32_t aoCur  = impl->aoHistoryIndex;
			const uint32_t aoPrev = 1u - aoCur;
			const bool aoRt = rtActive && impl->accel.InstanceCount() > 0;

			AoConstants ac = {};
			XMStoreFloat4x4(&ac.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.viewProj)));
			XMStoreFloat4x4(&ac.invViewProj, invViewProj);
			XMStoreFloat4x4(&ac.prevInvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&prevViewProj))));
			ac.eyePos = view.eyePosition;
			ac.radius = Impl::kAoRadius;
			ac.width = impl->width;
			ac.height = impl->height;
			ac.frameIndex = impl->frameCounter;
			ac.rtEnabled = aoRt ? 1u : 0u;
			ac.depthIndex = Impl::BindlessDepth;
			ac.normalIndex = Impl::BindlessGbNormal;
			ac.motionIndex = Impl::BindlessMotion;
			ac.prevDepthIndex = Impl::BindlessPrevDepth;
			ac.prevNormalIndex = Impl::BindlessPrevNormal;
			ac.rawUav = Impl::BindlessAoRawUav;
			ac.rawSrv = Impl::BindlessAoRawSrv;
			ac.historyInSrv  = aoPrev == 0 ? Impl::BindlessAoHistorySrv0 : Impl::BindlessAoHistorySrv1;
			ac.historyOutUav = aoCur == 0 ? Impl::BindlessAoHistoryUav0 : Impl::BindlessAoHistoryUav1;
			ac.historyOutSrv = aoCur == 0 ? Impl::BindlessAoHistorySrv0 : Impl::BindlessAoHistorySrv1;
			ac.outputUav = Impl::BindlessAoOutputUav;
			ac.historyValid = impl->aoHistoryValid ? 1u : 0u;
			ac.thickness = Impl::kAoThickness;
			memcpy(impl->aoCBData, &ac, sizeof(ac));

			ID3D12DescriptorHeap* bindlessHeaps[] = { impl->bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, bindlessHeaps);
			commandList->SetComputeRootSignature(impl->computeRootSig.Get());
			commandList->SetComputeRootConstantBufferView(0, impl->aoCB->GetGPUVirtualAddress());
			// TLAS 루트 SRV — RT 장치면 늘 묶는다(셰이더에 RayQuery 코드가 있으면 안 돌아도 묶여 있어야 한다). 실제로 쏘는지는 rtEnabled.
			if (impl->rtSupported)
			{
				commandList->SetComputeRootShaderResourceView(1, impl->accel.TlasAddress());
				// 2026-10-28 알파 마스크 — «모름» 레이의 후보 판정이 재질 표 · 인스턴스 표를 읽는다
				commandList->SetComputeRootShaderResourceView(2, impl->rtMaterials->GetGPUVirtualAddress());
				commandList->SetComputeRootShaderResourceView(3, impl->rtInstances->GetGPUVirtualAddress() +
					UINT64(impl->frameIndex % AccelStructure::kFrameSlots) * Impl::MaxInstances * sizeof(RtInstanceGpu));
			}

			impl->TransitionTargets({ { &impl->aoRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			commandList->SetPipelineState(impl->aoTracePso.Get());
			commandList->Dispatch(groupsX, groupsY, 1);

			impl->TransitionTargets({
				{ &impl->aoRaw,               Impl::kSkyReadState },
				{ &impl->aoHistory[aoCur],    D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
				{ &impl->aoHistory[aoPrev],   Impl::kSkyReadState } });
			commandList->SetPipelineState(impl->aoTemporalPso.Get());
			commandList->Dispatch(groupsX, groupsY, 1);

			impl->TransitionTargets({
				{ &impl->aoHistory[aoCur], Impl::kSkyReadState },
				{ &impl->aoOutput,         D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			commandList->SetPipelineState(impl->aoSpatialPso.Get());
			commandList->Dispatch(groupsX, groupsY, 1);
			impl->TransitionTargets({ { &impl->aoOutput, Impl::kSkyReadState } });

			impl->aoHistoryIndex = aoPrev;
		}
		impl->aoHistoryValid = aoPass;

		// ── 이번 프레임 깊이 · 법선을 «이전 프레임 사본» 으로 떠 둔다 (5단계 [4d] · 8단계 [2] 의 시간 누적이 같이 쓴다) ──
		//  다음 프레임 시간 누적이 «같은 표면인지» 볼 때 쓴다. 이번 시간 누적들은 위에서 이미 지난 프레임 사본을 읽었다 — 순서가 중요하다.
		impl->MarkGpuTime(L"깊이 · 법선 사본");
		if (reflectionPass || aoPass)
		{
			impl->TransitionTargets({
				{ &impl->depth,                     D3D12_RESOURCE_STATE_COPY_SOURCE },
				{ &impl->gbuffer[Impl::GbNormal],   D3D12_RESOURCE_STATE_COPY_SOURCE },
				{ &impl->prevDepth,                 D3D12_RESOURCE_STATE_COPY_DEST },
				{ &impl->prevNormal,                D3D12_RESOURCE_STATE_COPY_DEST } });
			commandList->CopyResource(impl->prevDepth.resource.Get(), impl->depth.resource.Get());
			commandList->CopyResource(impl->prevNormal.resource.Get(), impl->gbuffer[Impl::GbNormal].resource.Get());
			impl->TransitionTargets({
				{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
				{ &impl->gbuffer[Impl::GbNormal],   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
				{ &impl->prevDepth,                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
				{ &impl->prevNormal,                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } });
		}

		// ── [5] 라이팅 합성 — 컴퓨트가 G-buffer · 깊이 · 그림자 마스크 · 반사 텍스처를 읽어 HDR 컬러에 쓴다 (Lighting.hlsl) ──
		//  (5단계 — t4 = GB4 모션 벡터(디버그), t7 = [4d] 결과, t8 = 노이즈 제거 전 반사(디버그))
		impl->MarkGpuTime(L"[5] 라이팅");
		impl->TransitionTargets({
			{ &impl->shadowMask,         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->reflection,         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->reflectionDenoised, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
			{ &impl->hdr,                D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });

		LightingConstants lc = {};
		XMStoreFloat4x4(&lc.invViewProj, invViewProj);
		lc.eyePos = view.eyePosition;
		lc.sunDir = impl->sunDir;
		lc.width = impl->width;
		lc.height = impl->height;
		lc.debugMode = impl->debugMode;
		lc.shadowEnabled = shadowPass ? 1u : 0u;
		lc.reflectionEnabled = reflectionPass ? 1u : 0u;
		lc.aoEnabled = aoPass ? 1u : 0u;            // 8단계
		lc.sunIlluminance = impl->sunIlluminance;   // 6단계
		lc.skyEnabled = impl->skyValid ? 1u : 0u;   // 6단계 — 하늘 조도 텍스처가 만들어져 있으면 주변광으로 읽는다
		lc.probeVolume = impl->probeVolume;         // 7단계 — [4c] GI 프로브 (enabled 0 이면 하늘 조도)
		lc.probeVolumeFar = impl->probeVolumeFar;   // 2026-10-28 — 먼 겹
		memcpy(impl->lightingCBData, &lc, sizeof(lc));

		commandList->SetDescriptorHeaps(1, targetHeaps);
		commandList->SetComputeRootSignature(impl->lightingRootSig.Get());
		commandList->SetPipelineState(impl->lightingPso.Get());
		commandList->SetComputeRootConstantBufferView(0, impl->lightingCB->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(1, impl->TargetGpu(Impl::SlotGBuffer));
		commandList->Dispatch(groupsX, groupsY, 1);

		// ── [7] 반투명 — 투과 재질(얼음) 물체를 forward 로, 뒤에서 앞으로 (Transparent.hlsl, 2026-10-27 렌더 9단계) ──
		//  ① 이번 프레임 투과 물체 목록(패킷 순번) → 눈에서 먼 순으로 정렬
		//  ② 화면 색 피라미드: HDR → 밉 0, 밉 i−1 → 밉 i (ColorPyramid.hlsl). 밉마다 상태가 달라 하위 자원 배리어를 직접 건다.
		//  ③ HDR 에 그리기 — 읽기 전용 DSV(깊이 테스트만), 셰이더가 같은 깊이를 SRV 로 읽는다(굴절 자리 앞 가림 검사)
		//  디버그 뷰(0 이 아닌 것)에서는 그리지 않는다 — 디버그 뷰는 G-buffer 값을 보여 주는데 얼음은 G-buffer 에 없다.
		impl->MarkGpuTime(L"[7] 반투명");
		impl->transparentDraws.clear();
		if (impl->transparentPso && impl->debugMode == 0)
		{
			const XMVECTOR eye = XMLoadFloat3(&view.eyePosition);
			const size_t drawable = (std::min)(items.size(), size_t(Impl::MaxDrawInstances));
			for (size_t i = 0; i < drawable; ++i)
			{
				const InstanceData& it = items[i];
				if (it.mesh >= impl->meshes.size() || (it.flags & kInstanceDegenerate)) continue;
				const MaterialHandle material = it.material < impl->materials.size() ? it.material : 0;
				if (!impl->materials[material].transmissive) continue;
				const XMVECTOR position = XMVectorSet(it.world._41, it.world._42, it.world._43, 1.0f);
				impl->transparentDraws.push_back({ uint32_t(i), material, it.mesh,
					XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(position, eye))) });
			}
			std::sort(impl->transparentDraws.begin(), impl->transparentDraws.end(),
				[](const Impl::TransparentDraw& a, const Impl::TransparentDraw& b) { return a.distanceSq > b.distanceSq; });
		}
		if (!impl->transparentDraws.empty())
		{
			// ② 색 피라미드
			ID3D12DescriptorHeap* bindlessHeaps[] = { impl->bindlessHeap.Get() };
			commandList->SetDescriptorHeaps(1, bindlessHeaps);
			commandList->SetComputeRootSignature(impl->computeRootSig.Get());
			impl->TransitionTargets({
				{ &impl->hdr,        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
				{ &impl->sceneColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS } });
			UINT mipWidth = impl->width, mipHeight = impl->height;
			for (UINT mip = 0; mip < Impl::kSceneColorMips; ++mip)
			{
				PyramidConstants pc = {};
				pc.srcIndex = mip == 0 ? Impl::BindlessHdrSrv : Impl::BindlessSceneColorMipSrv0 + mip - 1;
				pc.dstIndex = Impl::BindlessSceneColorMipUav0 + mip;
				pc.dstWidth = mipWidth;
				pc.dstHeight = mipHeight;
				pc.srcTexelX = 1.0f / float(mipWidth * 2);
				pc.srcTexelY = 1.0f / float(mipHeight * 2);
				memcpy(impl->pyramidCBData + 256 * mip, &pc, sizeof(pc));
				if (mip > 0)
				{
					// 원본 밉(i−1)만 읽기로 — 나머지 밉은 UAV 그대로
					const D3D12_RESOURCE_BARRIER toRead = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
						{ { impl->sceneColor.resource.Get(), mip - 1,
							D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } } };
					commandList->ResourceBarrier(1, &toRead);
				}
				commandList->SetComputeRootConstantBufferView(0, impl->pyramidCB->GetGPUVirtualAddress() + 256 * mip);
				commandList->SetPipelineState(mip == 0 ? impl->pyramidCopyPso.Get() : impl->pyramidDownsamplePso.Get());
				commandList->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
				mipWidth = (std::max)(1u, mipWidth / 2);
				mipHeight = (std::max)(1u, mipHeight / 2);
			}
			{
				// 마지막 밉도 읽기로 → 이제 모든 밉이 NON_PIXEL 이라 한 번에 «둘 다 읽기» 로
				const D3D12_RESOURCE_BARRIER lastToRead = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
					{ { impl->sceneColor.resource.Get(), Impl::kSceneColorMips - 1,
						D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } } };
				commandList->ResourceBarrier(1, &lastToRead);
				impl->sceneColor.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				impl->TransitionTargets({ { &impl->sceneColor, Impl::kSkyReadState } });
			}

			// ③ 그리기
			const bool transparentRt = rtActive && impl->accel.InstanceCount() > 0;
			TransparentConstants tc = {};
			XMStoreFloat4x4(&tc.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.viewProj)));
			XMStoreFloat4x4(&tc.invViewProj, invViewProj);
			tc.eyePos = view.eyePosition;
			tc.maxDistance = Impl::kShadowMaxDistance;
			tc.sunDir = impl->sunDir;
			tc.sceneColorSrv = Impl::BindlessSceneColorSrv;
			tc.sunIlluminance = impl->sunIlluminance;
			tc.depthSrv = Impl::BindlessDepth;
			tc.width = impl->width;
			tc.height = impl->height;
			tc.mipCount = Impl::kSceneColorMips;
			tc.rtEnabled = transparentRt ? 1u : 0u;
			tc.skySrv = Impl::BindlessSkySrv;
			tc.skyIrradianceSrv = Impl::BindlessSkyIrradianceSrv;
			tc.probeIrradianceSrv = Impl::BindlessProbeIrradianceSrv;
			tc.probeDistanceSrv = Impl::BindlessProbeDistanceSrv;
			tc.probeStateSrv = Impl::BindlessProbeStateSrv;
			tc.probeVolume = impl->probeVolume;
			tc.probeVolumeFar = impl->probeVolumeFar;   // 2026-10-28 — 먼 겹
			memcpy(impl->transparentCBData, &tc, sizeof(tc));

			impl->TransitionTargets({
				{ &impl->hdr,   D3D12_RESOURCE_STATE_RENDER_TARGET },
				{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
				                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE } });
			const D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv = impl->ReadOnlyDsv();
			commandList->OMSetRenderTargets(1, &hdrRtv, FALSE, &readOnlyDsv);
			commandList->SetGraphicsRootSignature(impl->transparentRootSig.Get());
			commandList->SetPipelineState(impl->transparentPso.Get());
			commandList->SetGraphicsRootConstantBufferView(0, impl->transparentCB->GetGPUVirtualAddress());
			commandList->SetGraphicsRootShaderResourceView(2, impl->accel.TlasAddress());
			commandList->SetGraphicsRootShaderResourceView(3, impl->rtMaterials->GetGPUVirtualAddress());
			commandList->SetGraphicsRootShaderResourceView(4, impl->rtInstances->GetGPUVirtualAddress() +
				UINT64(impl->frameIndex % AccelStructure::kFrameSlots) * Impl::MaxInstances * sizeof(RtInstanceGpu));
			commandList->SetGraphicsRootShaderResourceView(5, pass.instances->GetGPUVirtualAddress());
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			for (const Impl::TransparentDraw& draw : impl->transparentDraws)
			{
				const uint32_t drawConstants[4] = { draw.instance, draw.material, 0, 0 };
				commandList->SetGraphicsRoot32BitConstants(1, 4, drawConstants, 0);
				const Impl::MeshGpu& m = impl->meshes[draw.mesh];
				commandList->IASetVertexBuffers(0, 1, &m.vbv);
				commandList->IASetIndexBuffer(&m.ibv);
				commandList->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
				++impl->stats.drawCalls;
			}
		}

		// 뒤에 붙을 래스터 패스(반투명 등)가 이어 그릴 수 있게 HDR · 깊이를 다시 그리기 상태로 묶어 둔다.
		impl->TransitionTargets({
			{ &impl->hdr,   D3D12_RESOURCE_STATE_RENDER_TARGET },
			{ &impl->depth, D3D12_RESOURCE_STATE_DEPTH_WRITE } });
		commandList->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);

		// ★ 5단계 — 다음 프레임의 «이전 프레임 viewProj» (모션 벡터 · 이력 위치 되살리기)
		impl->prevMainViewProj = view.viewProj;
		impl->hasPrevMainView = true;
	}

	void GRenderer::DrawSprites(const std::vector<UISprite>& sprites)
	{
		if (sprites.empty() || !impl->uiPso) return;

		// UI 는 백버퍼 위에 덮어 그린다 — 그 전에 [8] HDR → 백버퍼를 끝내 둔다(프레임에 한 번).
		impl->ResolveToBackBuffer();

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
		// UI 가 없던 프레임이면 여기서 [8] HDR → 백버퍼를 한다(있었으면 이미 끝나 있다).
		impl->ResolveToBackBuffer();

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = impl->renderTargets[impl->frameIndex].Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		impl->commandList->ResourceBarrier(1, &barrier);

		// GPU 시간 계측 (2026-10-28) — 마지막 표시를 찍고 이번 프레임 타임스탬프를 리드백 버퍼로 옮긴다
		impl->MarkGpuTime(L"끝");
		const UINT timestampsThisFrame = impl->timestampCount;
		if (impl->timestampHeap && timestampsThisFrame > 0)
			impl->commandList->ResolveQueryData(impl->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
				timestampsThisFrame, impl->timestampReadback.Get(), 0);

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
		impl->CollectGpuTimes(timestampsThisFrame);
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
