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
//  ★ 2026-10-28 두 겹(캐스케이드, Majercik 외 2021 7.3) — 가까운 겹(4 m) + 먼 겹(16 m, 정육면체).
//    두 겹은 아틀라스 · 상태 버퍼를 같이 쓰고, 겹마다 «시작 번호»(storageOffset) 뒤에 이어 놓인다.
//    샘플은 가까운 겹 → 먼 겹 → (둘 다 못 쓰면) 하늘 조도 순서. 겹 사이를 섞는 구간은 아직 없다(사용자 «경계 섞기 보류»).
//  ★ 2026-10-28 지면에 맞춘 격자 — 격자 축이 월드 축이 아니라 axisX · axisY · axisZ(행성 중심 기준 회전)다.
//    axisY = 짧은 축 = 지면 위쪽. 그래서 얇은 격자가 땅과 나란히 놓여 플레이어 둘레 수평으로 고르게 덮는다
//    (전에는 위쪽에 가장 가까운 월드 축이라 행성 위에서 36 ~ 47° 비스듬 → 앞쪽 약 30 m 에서 끊겼다).
//    «칸» 은 회전된 격자 좌표의 정수 칸이다: 칸 = floor(dot(P, 축) / 간격), 프로브 위치 = Σ 칸 × 축 × 간격.
// ============================================================
#ifndef PROBES_HLSLI
#define PROBES_HLSLI

#include "GBuffer.hlsli"   // EncodeNormalOct · DecodeNormalOct

// GRenderer.cpp 의 ProbeVolumeGpu 와 같은 배치 (128바이트 — 2026-10-28 두 겹 64 → 80, 지면에 맞춘 축 80 → 128).
//  여러 상수 버퍼에 그대로 들어간다.
struct ProbeVolume
{
	int3   baseCell;     uint  generation;       // 격자 맨 앞 칸(격자 좌표) · 세대(격자 축이 바뀌면 +1 → 옛 데이터 무시)
	int3   dims;         float spacing;          // 축별 프로브 수 · 간격(m)
	float  normalBias;   float viewBias;         // 샘플 위치를 법선 · 시선 쪽으로 띄우는 거리(간격 배수)
	uint   probesPerRow; uint  enabled;          // 아틀라스 한 줄의 프로브 수 · 1 = 이번 프레임에 쓸 수 있다
	float2 irradianceAtlasSize;  float2 distanceAtlasSize;   // 두 겹이 같이 쓰는 아틀라스 전체 크기
	uint   storageOffset;  uint3 _pad;           // 2026-10-28 — 이 겹 프로브가 아틀라스 · 상태 버퍼에서 시작하는 번호
	float4 axisX;  float4 axisY;  float4 axisZ;  // 2026-10-28 — 격자 축(월드 단위 벡터, w 안 씀). axisY = 짧은 축 = 지면 위쪽
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

// 월드 칸 → 겹 안의 저장 번호 (링 주소, 0 ~ 겹의 프로브 수 − 1). 아틀라스 · 상태 버퍼 번호는 여기에 storageOffset 을 더한다.
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

// 칸(격자 좌표) → 월드 위치. 2026-10-28 — 격자 축이 돌아가 있다(행성 중심 기준).
float3 ProbeWorldPosition(int3 cell, ProbeVolume v)
{
	return (float(cell.x) * v.axisX.xyz + float(cell.y) * v.axisY.xyz + float(cell.z) * v.axisZ.xyz) * v.spacing;
}

// 월드 위치 → 격자 좌표(칸 단위, 소수). floor 하면 칸 번호.
float3 ProbeGridCoord(float3 P, ProbeVolume v)
{
	return float3(dot(P, v.axisX.xyz), dot(P, v.axisY.xyz), dot(P, v.axisZ.xyz)) / v.spacing;
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
	const float3 cellF   = ProbeGridCoord(biasedP, v);   // 2026-10-28 — 지면에 맞춘 격자 좌표
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
		const uint index = v.storageOffset + ProbeStorageIndex(cell, v);   // 2026-10-28 두 겹 — 공용 아틀라스 · 상태 번호
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

// 2026-10-28 두 겹 — 가까운 겹을 먼저, 거기서 쓸 프로브가 없으면 먼 겹. 둘 다 없으면 weightSum = 0(부른 쪽이 하늘 조도).
//  layer = 0 가까운 겹 · 1 먼 겹 · 2 둘 다 못 씀 (디버그 뷰 12 색).
float3 SampleProbeLayers(Texture2D<float4> irradiance, Texture2D<float2> distance, StructuredBuffer<int4> states,
	SamplerState clampSampler, ProbeVolume nearV, ProbeVolume farV, float3 P, float3 N, float3 V,
	out float weightSum, out uint layer)
{
	weightSum = 0.0;
	layer = 2;
	float3 e = float3(0.0, 0.0, 0.0);
	if (nearV.enabled != 0)
	{
		e = SampleProbes(irradiance, distance, states, clampSampler, nearV, P, N, V, weightSum);
		if (weightSum > 0.0) { layer = 0; return e; }
	}
	if (farV.enabled != 0)
	{
		e = SampleProbes(irradiance, distance, states, clampSampler, farV, P, N, V, weightSum);
		if (weightSum > 0.0) { layer = 1; return e; }
	}
	return float3(0.0, 0.0, 0.0);
}

#endif
