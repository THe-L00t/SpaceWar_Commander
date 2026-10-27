// ============================================================
//  Probes.hlsli — [4c] GI 프로브 공용 (2026-10-27 렌더 7단계)
//
//  설계 문서 3.2 · 6.4 의 DDGI(Majercik 외, JCGT 2019) 프로브. 쓰는 곳:
//    · ProbeGI.hlsl       — 프로브 레이 추적 · 조도/거리 섞기 (다중 반사: 맞은 점에서 지난 프로브를 읽는다)
//    · Lighting.hlsl      — 픽셀의 간접 확산(주변광)
//    · RtReflection.hlsl  — 반사 레이가 맞은 점의 간접 확산
//
//  ★ 격자: 카메라를 따라 칸 단위로 스크롤한다. 프로브는 «월드 칸»(위치 / 간격, 정수)에 놓이고,
//    저장 번호 = 월드 칸을 격자 크기로 나눈 나머지(링 주소) — 격자가 움직여도 아틀라스를 옮겨 쓰지 않는다.
//    새로 격자에 들어온 칸은 그 칸을 다시 추적할 때까지 «데이터 없음» 으로 보고 쓰지 않는다(상태 버퍼).
//  ★ 아틀라스: 프로브마다 조도 8×8(팔면체 6×6 + 테두리 1), 거리 16×16(14×14 + 테두리 1).
//    테두리는 팔면체 가장자리 맞은편 텍셀을 복사한 값 — 쌍선형 샘플이 프로브 칸을 넘지 않게(RTXGI 와 같은 배치).
//  ★ 조도는 «코사인 가중 평균 휘도 = 조도 / π» 로 저장한다(설계 문서 6.4). 알베도만 곱하면 확산 휘도다.
//    (RTXGI 는 1/2π 로 저장하고 샘플에서 2π 를 곱한다 — 같은 값의 다른 표현)
// ============================================================
#ifndef PROBES_HLSLI
#define PROBES_HLSLI

#include "GBuffer.hlsli"   // EncodeNormalOct · DecodeNormalOct

// GRenderer.cpp 의 ProbeVolumeGpu 와 같은 배치 (64바이트). 여러 상수 버퍼에 그대로 들어간다.
struct ProbeVolume
{
	int3   baseCell;     uint  generation;       // 격자 맨 앞 칸의 월드 칸 번호 · 세대(격자 축이 바뀌면 +1 → 옛 데이터 무시)
	int3   dims;         float spacing;          // 축별 프로브 수 · 간격(m)
	float  normalBias;   float viewBias;         // 샘플 위치를 법선 · 시선 쪽으로 띄우는 거리(간격 배수)
	uint   probesPerRow; uint  enabled;          // 아틀라스 한 줄의 프로브 수 · 1 = 이번 프레임에 쓸 수 있다
	float2 irradianceAtlasSize;  float2 distanceAtlasSize;
};

static const uint kProbeIrradianceTexels = 8;    // 6×6 + 테두리
static const uint kProbeDistanceTexels   = 16;   // 14×14 + 테두리
static const uint kProbeRays             = 64;   // 프로브당 레이 (설계 문서 3.2)
static const uint kProbeStateNoData   = 0;
static const uint kProbeStateActive   = 1;
static const uint kProbeStateInactive = 2;       // 레이 25 % 이상이 뒷면 — 벽 · 땅 속 프로브

int3 PositiveMod(int3 a, int3 m)
{
	return ((a % m) + m) % m;
}

// 월드 칸 → 저장 번호 (링 주소)
uint ProbeStorageIndex(int3 cell, ProbeVolume v)
{
	const int3 s = PositiveMod(cell, v.dims);
	return uint(s.x + v.dims.x * (s.y + v.dims.y * s.z));
}

// 저장 번호 → 지금 격자 안에서 그 번호를 쓰는 월드 칸
int3 ProbeCellFromStorage(uint index, ProbeVolume v)
{
	const int3 s = int3(index % uint(v.dims.x), (index / uint(v.dims.x)) % uint(v.dims.y), index / uint(v.dims.x * v.dims.y));
	return v.baseCell + PositiveMod(s - PositiveMod(v.baseCell, v.dims), v.dims);
}

float3 ProbeWorldPosition(int3 cell, ProbeVolume v)
{
	return float3(cell) * v.spacing;
}

// 상태 한 칸 = int4(월드 칸 xyz, 세대 << 2 | 상태). 이 칸 · 이 세대 · 활성이어야 쓴다.
bool ProbeUsable(int4 state, int3 cell, ProbeVolume v)
{
	return all(state.xyz == cell) && (uint(state.w) >> 2) == v.generation && (uint(state.w) & 3u) == kProbeStateActive;
}

// 프로브 번호 · 방향 → 아틀라스 UV. 팔면체 [0,1]² 을 테두리 1 텍셀 안쪽 interior 영역에 놓는다.
//  (섞기 쪽 텍셀 i 의 중심 = (i + 0.5) / interior 와 같은 규약)
float2 ProbeAtlasUv(uint index, float3 dir, uint texels, uint probesPerRow, float2 atlasSize)
{
	const float2 oct   = EncodeNormalOct(dir) * 0.5 + 0.5;
	const float2 tile  = float2(index % probesPerRow, index / probesPerRow) * texels;
	const float2 texel = tile + 1.0 + oct * (texels - 2);
	return texel / atlasSize;
}

// P(법선 N, 눈 쪽 V) 주변 프로브 8개를 섞어 «조도 / π» 를 돌려준다. RTXGI DDGIGetVolumeIrradiance 와 같은 가중치:
//   삼선형 × 감싸기 음영((cos+1)/2)² + 0.2 × 체비쇼프 가시성³(최소 0.05) × 작은 가중치 눌러 줄이기(0.2 아래)
//  쓸 수 있는 프로브가 하나도 없으면(격자 밖 · 아직 안 채워짐 · 다 벽 속) weightSum = 0 → 부른 쪽이 하늘 조도로 대신한다.
float3 SampleProbes(Texture2D<float4> irradiance, Texture2D<float2> distance, StructuredBuffer<int4> states,
	SamplerState clampSampler, ProbeVolume v, float3 P, float3 N, float3 V, out float weightSum)
{
	const float3 biasedP = P + (N * v.normalBias + V * v.viewBias) * v.spacing;
	const float3 cellF   = biasedP / v.spacing;
	const int3   base    = int3(floor(cellF));
	const float3 alpha   = cellF - float3(base);

	float3 sum = float3(0.0, 0.0, 0.0);
	weightSum = 0.0;
	[unroll]
	for (uint i = 0; i < 8; ++i)
	{
		const int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		const int3 cell   = base + offset;
		if (any(cell < v.baseCell) || any(cell >= v.baseCell + v.dims))
			continue;   // 격자 밖
		const uint index = ProbeStorageIndex(cell, v);
		if (!ProbeUsable(states[index], cell, v))
			continue;

		const float3 probeP        = ProbeWorldPosition(cell, v);
		const float3 toProbe       = normalize(probeP - P);
		const float3 biasedToProbe = probeP - biasedP;
		const float  dist          = max(length(biasedToProbe), 1.0e-4);

		const float3 trilinear = max(0.001, lerp(1.0 - alpha, alpha, float3(offset)));
		float weight = 1.0;
		const float wrap = (dot(toProbe, N) + 1.0) * 0.5;      // 뒤쪽 프로브는 약하게(0 으로 자르지 않음)
		weight *= wrap * wrap + 0.2;

		// 체비쇼프 가시성 — 프로브에서 이쪽 방향으로 잰 평균 거리 · 거리² 로 «가려졌을 확률» 을 추정
		const float2 moments  = distance.SampleLevel(clampSampler,
			ProbeAtlasUv(index, -biasedToProbe / dist, kProbeDistanceTexels, v.probesPerRow, v.distanceAtlasSize), 0);
		const float  variance = abs(moments.x * moments.x - moments.y);
		float chebyshev = 1.0;
		if (dist > moments.x)
		{
			const float d = dist - moments.x;
			chebyshev = variance / (variance + d * d);
			chebyshev = chebyshev * chebyshev * chebyshev;
		}
		weight *= max(0.05, chebyshev);
		weight = max(1.0e-6, weight);
		const float crush = 0.2;
		if (weight < crush)
			weight *= weight * weight / (crush * crush);
		weight *= trilinear.x * trilinear.y * trilinear.z;

		sum += weight * irradiance.SampleLevel(clampSampler,
			ProbeAtlasUv(index, N, kProbeIrradianceTexels, v.probesPerRow, v.irradianceAtlasSize), 0).rgb;
		weightSum += weight;
	}
	return weightSum > 0.0 ? sum / weightSum : float3(0.0, 0.0, 0.0);
}

#endif
