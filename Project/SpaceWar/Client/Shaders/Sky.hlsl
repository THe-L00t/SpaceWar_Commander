// ============================================================
//  Sky.hlsl — [6] 하늘 (컴퓨트 cs_6_6 · 바인드리스, 2026-10-27 렌더 6단계)
//
//  진입점 네 개 (GRenderer.cpp UpdateSunAndSky 가 프레임의 첫 Render 에서 부른다):
//    CSMultiScattering 다중 산란 LUT(32×32). 대기 값만으로 정해지므로 처음 한 번만 돈다.
//    CSGenerateSky    하늘 텍스처(등장방형 256×128)를 대기 산란으로 계산해 채운다.
//                     해 방향 · 카메라 자리의 위가 바뀔 때만 다시 돈다.
//    CSSkyIrradiance  하늘 텍스처를 코사인으로 섞어 «법선 방향별 하늘 조도 / π» (32×16)를 만든다 — 그늘의 주변광.
//    CSDrawSky        화면 픽셀마다 하늘 텍스처 + 해 원반을 HDR 컬러에 쓴다. 그 위에 배경 행성 · 메인 라이팅이 덮어 그린다.
//
//  ★ 하늘 — 대기 산란 (레일리 + 미 + 오존 흡수) = 단일 산란 + Hillaire 2020 의 다중 산란 근사.
//    사용자 결정(10-27): 행성별 하늘 에셋은 없다. 행성에 내려오면 항성 빛을 지구 기준 대기 산란으로 계산한 푸른 하늘,
//    우주에서는 우주 배경 스카이박스(아직 없음)를 쓴다.
//    대기 값은 Hillaire 의 지구 대기 기본값 그대로다(UnrealEngineSkyAtmosphere, SkyAtmosphereCommon.cpp — 1/km 를 1/m 로 옮김).
//    다중 산란: 처음엔 단일 산란만 넣었는데, 땅이 받는 하늘 조도/π 가 0.03~0.09 로 맑은 하늘(직달광의 10~15 %)보다 많이 어두웠다.
//    → 원문(RenderSkyRayMarching.hlsl NewMultiScattCS)과 같은 방식: 점마다 «구 전체에서 들어오는 2차 산란(등방)» 을 구하고
//      더 높은 차수를 등비급수 1/(1 − f_ms) 로 합친 Ψms 를 LUT 에 둔다. 하늘을 계산할 때 산란 계수 × Ψms 를 더한다.
//  ★ 행성 크기: 대기 계산은 «지구 크기 행성 위 2 m» 로 한다. 게임 행성(반지름 650 m)은 대기에 비해 너무 작아서,
//    실제 행성 반지름을 넣으면 지평선 아래가 거의 다 대기가 된다. 위 방향만 카메라 자리에 맞춘다.
// ============================================================

#include "Sky.hlsli"
#include "GBuffer.hlsli"   // ReconstructWorldPosition

// GRenderer.cpp 의 SkyConstants 와 같은 배치 (176바이트)
cbuffer SkyCB : register(b0)
{
	float4x4 gInvViewProj;
	float3   gEyePos;          float gSunIlluminanceTop;   // 대기 위 태양 조도 (흰빛, π)
	float3   gToSun;           float gCosSunRadius;        // 해 쪽 단위 방향, 해 원반 반지름의 cos
	float3   gUp;              float gObserverHeight;      // 카메라 자리의 위(행성 중심 → 카메라), 관측 높이(m)
	float3   gSunDiskRadiance; uint  gHdrUav;              // 해 원반 휘도 = 지면에 닿는 태양 조도 / 원반 입체각
	uint2    gScreenSize;      uint2 gSkySize;
	uint2    gIrradianceSize;  uint  gSkyUav;   uint gSkySrv;   // ↓ 바인드리스 힙 번호
	uint     gIrradianceUav;   uint  gMsUav;    uint gMsSrv;    uint gMsSize;   // 다중 산란 LUT (정사각)
};
SamplerState gSkySampler   : register(s1);   // 선형 · U 반복 · V 고정
SamplerState gClampSampler : register(s2);   // 선형 · 고정 (다중 산란 LUT)

// ── 지구 대기 (Hillaire 기본값, 단위 m · 1/m) ──
static const float  kBottomRadius        = 6360.0e3;
static const float  kTopRadius           = 6460.0e3;
static const float3 kRayleighScattering  = float3(5.802e-6, 13.558e-6, 33.1e-6);
static const float  kRayleighScaleHeight = 8.0e3;
static const float  kMieScattering       = 3.996e-6;
static const float  kMieExtinction       = 4.440e-6;
static const float  kMieScaleHeight      = 1.2e3;
static const float  kMieG                = 0.8;
static const float3 kOzoneAbsorption     = float3(0.650e-6, 1.881e-6, 0.085e-6);   // 25 km 에서 1, 10 · 40 km 에서 0 인 텐트 분포
static const float3 kGroundAlbedo        = float3(0.3, 0.3, 0.3);   // 지평선 아래(대기 계산 속 지면) — 실제 지형이 대부분 가린다
static const uint   kViewSamples = 32;
static const uint   kSunSamples  = 16;

struct Medium
{
	float  rayleigh;     // 레일리 밀도 (지면 = 1)
	float  mie;          // 미 밀도 (지면 = 1)
	float3 extinction;   // 소멸 계수 (산란 + 흡수, 1/m)
};

Medium SampleMedium(float height)
{
	Medium m;
	m.rayleigh = exp(-height / kRayleighScaleHeight);
	m.mie      = exp(-height / kMieScaleHeight);
	const float km    = height * 1.0e-3;
	const float ozone = saturate(km < 25.0 ? km / 15.0 - 2.0 / 3.0 : -km / 15.0 + 8.0 / 3.0);
	m.extinction = kRayleighScattering * m.rayleigh + kMieExtinction * m.mie + kOzoneAbsorption * ozone;
	return m;
}

// 반지름 r 인 점에서 «위와 cos mu» 방향으로 대기 끝(바깥 구)까지의 거리.
//  r² 끼리 빼면 float 정밀도가 모자라서 (R − r)(R + r) 로 푼다.
float DistanceToTop(float r, float mu)
{
	const float disc = (kTopRadius - r) * (kTopRadius + r) + r * r * mu * mu;
	return -r * mu + sqrt(max(disc, 0.0));
}

// 같은 방향으로 지면(안쪽 구)에 닿으면 true 와 거리
bool DistanceToGround(float r, float mu, out float t)
{
	t = 0.0;
	if (mu >= 0.0) return false;
	const float disc = r * r * mu * mu - (r - kBottomRadius) * (r + kBottomRadius);
	if (disc < 0.0) return false;
	t = -r * mu - sqrt(disc);
	return t >= 0.0;
}

// 점(반지름 r)에서 해 쪽(위와 cos muS)으로 대기 끝까지의 투과율. 지면에 막히면 0.
//  GRenderer.cpp 의 SunTransmittance(CPU) 와 같은 식 — 그쪽은 태양 직접광 색을 구한다.
float3 SunTransmittance(float r, float muS)
{
	float tGround;
	if (DistanceToGround(r, muS, tGround)) return float3(0.0, 0.0, 0.0);
	const float dt = DistanceToTop(r, muS) / kSunSamples;
	float3 depth = float3(0.0, 0.0, 0.0);
	for (uint i = 0; i < kSunSamples; ++i)
	{
		const float t  = (i + 0.5) * dt;
		const float rr = sqrt(r * r + t * t + 2.0 * r * muS * t);
		depth += SampleMedium(rr - kBottomRadius).extinction * dt;
	}
	return exp(-depth);
}

// 위상 함수 — nu = 시선과 해 방향 사이 cos
float RayleighPhase(float nu)
{
	return 3.0 / (16.0 * kSkyPi) * (1.0 + nu * nu);
}
float MiePhase(float nu)   // Cornette-Shanks (Hillaire 코드와 같은 식)
{
	const float g2 = kMieG * kMieG;
	const float k  = 3.0 / (8.0 * kSkyPi) * (1.0 - g2) / (2.0 + g2);
	return k * (1.0 + nu * nu) / pow(max(1.0 + g2 - 2.0 * kMieG * nu, 1.0e-4), 1.5);
}

// 산란 계수(흡수 빼고, 1/m) — 다중 산란 항에 곱한다
float3 ScatteringOf(Medium m)
{
	return kRayleighScattering * m.rayleigh + kMieScattering * m.mie;
}

// 다중 산란 LUT 읽기 — 칸 (해 천정각 cos·0.5+0.5, 높이/대기 두께). 원문 GetMultipleScattering 과 같은 배치.
float3 MultipleScattering(float r, float muS)
{
	Texture2D<float4> lut = ResourceDescriptorHeap[gMsSrv];
	const float2 uv = saturate(float2(muS * 0.5 + 0.5, (r - kBottomRadius) / (kTopRadius - kBottomRadius)));
	// 텍셀 중심끼리만 보간하게 (원문 fromUnitToSubUvs)
	const float2 sub = (uv * (gMsSize - 1) + 0.5) / gMsSize;
	return lut.SampleLevel(gClampSampler, sub, 0).rgb;
}

// 방향 dir(월드)으로 본 하늘 휘도 — 단일 산란 + 다중 산란 근사.
//  시선을 kViewSamples 칸으로 나눠, 칸마다 «여기까지 시선 투과율 × (해에서 여기까지 투과율 × 산란 × 위상 + 산란 × Ψms)» 를 더한다.
//  ★ 7단계에서 고침 — 지평선 아래는 «지평선 색» 을 이어 쓴다.
//    대기 계산 속 땅(지구 크기)은 지평선 바로 아래부터 시선을 막지만, 실제 행성(반지름 1.6 km · 게임 650 m)은 지평선이
//    2~3° 더 내려간다. 그 틈으로 가짜 땅 색(갈색 띠)이 하늘에 보였다. 지평선 아래 진짜 땅은 장면의 지형 메시가 그린다.
float3 SkyRadiance(float3 dir)
{
	const float r0   = kBottomRadius + gObserverHeight;
	const float below = dot(dir, gUp);
	if (below < 1.0e-3)
		dir = normalize(dir + gUp * (1.0e-3 - below));   // 수평면 바로 위로 올린다(방위는 그대로)
	const float mu   = dot(dir, gUp);
	const float nu   = dot(dir, gToSun);
	const float muS0 = dot(gUp, gToSun);

	float tEnd;
	const bool ground = DistanceToGround(r0, mu, tEnd);
	if (!ground)
		tEnd = DistanceToTop(r0, mu);
	const float dt = tEnd / kViewSamples;

	float3 depth = float3(0.0, 0.0, 0.0);
	float3 sumRayleigh = float3(0.0, 0.0, 0.0);
	float3 sumMie = float3(0.0, 0.0, 0.0);
	float3 sumMultiple = float3(0.0, 0.0, 0.0);
	for (uint i = 0; i < kViewSamples; ++i)
	{
		const float t = (i + 0.5) * dt;
		const float r = sqrt(r0 * r0 + t * t + 2.0 * r0 * mu * t);
		const float muS = (r0 * muS0 + t * nu) / r;   // 이 점의 위(= 위치/반지름)와 해 방향의 cos
		const Medium m = SampleMedium(r - kBottomRadius);
		depth += m.extinction * (0.5 * dt);           // 칸 가운데까지
		const float3 view = exp(-depth);
		const float3 T = view * SunTransmittance(r, muS);
		sumRayleigh += m.rayleigh * T * dt;
		sumMie      += m.mie * T * dt;
		sumMultiple += view * ScatteringOf(m) * MultipleScattering(r, muS) * dt;
		depth += m.extinction * (0.5 * dt);
	}
	float3 L = gSunIlluminanceTop *
		(kRayleighScattering * sumRayleigh * RayleighPhase(nu) + kMieScattering * sumMie * MiePhase(nu) + sumMultiple);

	return L;   // (지평선 위로 올렸으므로 시선이 대기 속 땅에 닿는 경우는 없다)
}

// 지평선 아래에서 오는 빛 — 하늘 조도(주변광 대체값)를 계산할 때만 쓴다.
//  땅(알베도 0.3)이 받는 태양 직접광의 람베르트 반사. 진짜 지형 색은 GI 프로브([4c])가 가져온다 — 이건 GI 가 없을 때의 대체값.
float3 GroundRadiance()
{
	const float muS0 = dot(gUp, gToSun);
	return kGroundAlbedo / kSkyPi * gSunIlluminanceTop *
		SunTransmittance(kBottomRadius + gObserverHeight, muS0) * saturate(muS0);
}

// ── ⓪ 다중 산란 LUT (Hillaire 2020 §5.5 — 원문 NewMultiScattCS 와 같은 계산) ──
//  칸마다 점(높이) · 해 천정각이 정해진다. 구 위 고른 방향 64개로 레이를 쏘아
//    L₂   = 방향 평균 ∫ 시선 투과율 × 산란 × (1/4π) × 해 투과율 dt  (+ 지면에 닿으면 지면 반사)  — 등방 2차 산란
//    f_ms = 방향 평균 ∫ 시선 투과율 × 산란 dt                                                   — 한 번 더 산란될 비율
//  Ψms = L₂ / (1 − f_ms)  (3차 이상을 등비급수로 합침). 태양 조도 1 기준으로 저장한다.
static const uint kMsDirectionsSqrt = 8;   // 8×8 = 64 방향 (원문 SQRTSAMPLECOUNT)
static const uint kMsSteps = 20;           // 원문 SampleCountIni

[numthreads(8, 8, 1)]
void CSMultiScattering(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= uint2(gMsSize, gMsSize))) return;
	RWTexture2D<float4> lut = ResourceDescriptorHeap[gMsUav];

	// 칸 → (해 천정각 cos, 반지름). 텍셀 중심이 0 과 1 에 오게 (원문 fromSubUvsToUnit)
	const float2 uv = float2(id.xy) / float(gMsSize - 1);
	const float muS = uv.x * 2.0 - 1.0;
	const float r   = kBottomRadius + 1.0 + uv.y * (kTopRadius - kBottomRadius - 2.0);
	// 국소 좌표: 위 = +y, 해는 x-y 평면
	const float3 toSunLocal = float3(sqrt(saturate(1.0 - muS * muS)), muS, 0.0);

	float3 sumL = float3(0.0, 0.0, 0.0);
	float3 sumF = float3(0.0, 0.0, 0.0);
	for (uint k = 0; k < kMsDirectionsSqrt * kMsDirectionsSqrt; ++k)
	{
		// 구 위 고른 방향 (원문과 같은 격자: 경도 2π·a, 위도 acos(1 − 2b))
		const float a = (0.5 + (k / kMsDirectionsSqrt)) / kMsDirectionsSqrt;
		const float b = (0.5 + (k % kMsDirectionsSqrt)) / kMsDirectionsSqrt;
		const float theta = 2.0 * kSkyPi * a;
		const float cosPhi = 1.0 - 2.0 * b;
		const float sinPhi = sqrt(saturate(1.0 - cosPhi * cosPhi));
		const float3 dir = float3(cos(theta) * sinPhi, cosPhi, sin(theta) * sinPhi);
		const float mu = dir.y;
		const float nu = dot(dir, toSunLocal);

		float tEnd;
		const bool ground = DistanceToGround(r, mu, tEnd);
		if (!ground)
			tEnd = DistanceToTop(r, mu);
		const float dt = tEnd / kMsSteps;

		float3 depth = float3(0.0, 0.0, 0.0);
		float3 L = float3(0.0, 0.0, 0.0);
		float3 F = float3(0.0, 0.0, 0.0);
		for (uint i = 0; i < kMsSteps; ++i)
		{
			const float t  = (i + 0.5) * dt;
			const float rr = sqrt(r * r + t * t + 2.0 * r * mu * t);
			const float muSp = (r * muS + t * nu) / rr;
			const Medium m = SampleMedium(rr - kBottomRadius);
			depth += m.extinction * (0.5 * dt);
			const float3 view = exp(-depth);
			const float3 scattering = ScatteringOf(m);
			L += view * scattering * (1.0 / (4.0 * kSkyPi)) * SunTransmittance(rr, muSp) * dt;
			F += view * scattering * dt;
			depth += m.extinction * (0.5 * dt);
		}
		if (ground)
		{
			const float muSGround = (r * muS + tEnd * nu) / kBottomRadius;
			L += exp(-depth) * kGroundAlbedo / kSkyPi * SunTransmittance(kBottomRadius + 1.0, muSGround) * saturate(muSGround);
		}
		sumL += L;
		sumF += F;
	}
	const float invCount = 1.0 / (kMsDirectionsSqrt * kMsDirectionsSqrt);
	const float3 L2  = sumL * invCount;
	const float3 fms = sumF * invCount;
	lut[id.xy] = float4(L2 / max(1.0 - fms, 1.0e-3), 1.0);
}

// ── ① 하늘 텍스처 채우기 ──
[numthreads(8, 8, 1)]
void CSGenerateSky(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gSkySize)) return;
	RWTexture2D<float4> sky = ResourceDescriptorHeap[gSkyUav];
	const float2 uv = (float2(id.xy) + 0.5) / float2(gSkySize);
	sky[id.xy] = float4(SkyRadiance(SkyUvToDirection(uv)), 1.0);
}

// ── ② 하늘 조도 / π — 법선 N 에 대해 ∫ L(ω) max(0, N·ω) dω / π ──
//  하늘 텍스처를 64×32 칸으로 훑는다. 칸의 입체각 = Δφ · Δθ · sin θ.
static const uint kIrradianceStepsU = 64;
static const uint kIrradianceStepsV = 32;

[numthreads(8, 8, 1)]
void CSSkyIrradiance(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gIrradianceSize)) return;
	Texture2D<float4>   sky = ResourceDescriptorHeap[gSkySrv];
	RWTexture2D<float4> irradiance = ResourceDescriptorHeap[gIrradianceUav];

	const float3 N = SkyUvToDirection((float2(id.xy) + 0.5) / float2(gIrradianceSize));
	const float3 ground = GroundRadiance();   // 지평선 아래 방향은 하늘 텍스처(지평선 색) 대신 땅 반사광
	float3 sum = float3(0.0, 0.0, 0.0);
	for (uint v = 0; v < kIrradianceStepsV; ++v)
	{
		const float sinTheta = sin((v + 0.5) * kSkyPi / kIrradianceStepsV);
		for (uint u = 0; u < kIrradianceStepsU; ++u)
		{
			const float2 uv = float2((u + 0.5) / kIrradianceStepsU, (v + 0.5) / kIrradianceStepsV);
			const float3 d  = SkyUvToDirection(uv);
			const float  c  = dot(N, d);
			if (c > 0.0)
				sum += (dot(d, gUp) < 0.0 ? ground : sky.SampleLevel(gSkySampler, uv, 0).rgb) * (c * sinTheta);
		}
	}
	const float dOmega = (2.0 * kSkyPi / kIrradianceStepsU) * (kSkyPi / kIrradianceStepsV);
	irradiance[id.xy] = float4(sum * dOmega / kSkyPi, 1.0);
}

// ── ③ 화면에 하늘 그리기 ──
//  픽셀 방향 = 먼 평면 점 − 가까운 평면 점(투영이 달라도 맞다 — 배경 패스가 먼저면 그 투영으로 부른다).
[numthreads(8, 8, 1)]
void CSDrawSky(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= gScreenSize)) return;
	Texture2D<float4>   sky = ResourceDescriptorHeap[gSkySrv];
	RWTexture2D<float4> hdr = ResourceDescriptorHeap[gHdrUav];

	const float3 nearPoint = ReconstructWorldPosition(id.xy, 0.0, gScreenSize, gInvViewProj, gEyePos);
	const float3 farPoint  = ReconstructWorldPosition(id.xy, 1.0, gScreenSize, gInvViewProj, gEyePos);
	const float3 dir = normalize(farPoint - nearPoint);

	float3 L = SampleSkyRadiance(sky, gSkySampler, dir);
	if (dot(dir, gToSun) >= gCosSunRadius)
		L += gSunDiskRadiance;
	hdr[id.xy] = float4(L, 1.0);
}
