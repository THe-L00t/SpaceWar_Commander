// ============================================================
//  TerrainLayers.hlsli — 지형 레이어 섞기 공용 (2026-10-28)
//
//  예시 행성의 지면은 «레이어 4종 + LayerMask(RGBA)» 다(리소스 규격 — 노션 «예시 행성 제작 기술» 2.6).
//  glb 의 지형 재질에는 대체 레이어 한 장만 연결돼 있어서, 네 레이어 섞기는 렌더러가 한다.
//  [1] G-buffer 지형 판(Forward.hlsl TERRAIN_LAYERS=1)과 RT 히트(RtSurface.hlsli FetchHitSurface)가 같이 쓴다.
//
//  · 마스크 UV — 메시에 없다. 행성 중심에서 본 방향으로 계산한다(리소스 Placement.json terrain_layers.mask_uv).
//  · 높이 섞기 — 블렌더 시안(Resource_Work/<행성>/scripts/build_map.py draft_terrain_material)의 점수식 그대로.
//  텍스처 객체는 부르는 쪽이 넘긴다(래스터는 레지스터, RT 는 바인드리스) — 여기는 계산만 둔다.
// ============================================================
#ifndef TERRAIN_LAYERS_HLSLI
#define TERRAIN_LAYERS_HLSLI

static const float kTerrainPi = 3.14159265;

// sign 의 부호를 magnitude 에 붙인다(C copysign — HLSL 에 없다).
float TerrainCopySign(float magnitude, float sign)
{
	return sign < 0.0 ? -abs(magnitude) : abs(magnitude);
}

// 등면적 팔면체 매핑 — 단위 방향 → [0,1]² (Clarberg 2008 «Fast Equal-Area Mapping of the (Hemi)Sphere using SIMD»,
//  pbrt-v4 src/pbrt/util/math.cpp EqualAreaSphereToSquare). pbrt 는 atan 을 다항식으로 근사하지만
//  여기는 atan 그대로 쓴다 — 마스크를 구운 리소스 쪽 pl_common.py sphere_to_square 와 같은 식이다.
float2 EqualAreaSphereToSquare(float3 d)
{
	const float x = abs(d.x);
	const float y = abs(d.y);
	const float z = abs(d.z);
	const float r = sqrt(max(1.0 - z, 0.0));
	const float a = max(x, y);
	const float b = a > 0.0 ? min(x, y) / a : 0.0;
	float phi = atan(b) * (2.0 / kTerrainPi);
	if (x < y)
		phi = 1.0 - phi;
	float v = phi * r;
	float u = r - v;
	if (d.z < 0.0)   // 아래 반구는 팔면체 바깥 삼각형으로 접는다
	{
		const float t = u;
		u = 1.0 - v;
		v = 1.0 - t;
	}
	u = TerrainCopySign(u, d.x);
	v = TerrainCopySign(v, d.y);
	return float2(0.5 * (u + 1.0), 0.5 * (v + 1.0));
}

// 레이어 마스크 UV. meshPosition = 메시 좌표(지형 노드에 변환이 없어 원점 = 행성 중심).
//  리소스 규약: 방향 d(glTF) 를 (d.x, −d.z, d.y) 로 바꾼 뒤 등면적 팔면체 매핑.
//  PNG 가로 = u, 세로 = v, 맨 위 행 v = 0(Resource_Work mask_gen.py 의 전치) → D3D 텍스처 좌표로 그대로 쓴다.
float2 TerrainMaskUv(float3 meshPosition)
{
	const float3 d = normalize(meshPosition);
	return EqualAreaSphereToSquare(float3(d.x, -d.z, d.y));
}

// 높이 섞기 가중치(합 1). 블렌더 시안 그대로:
//   점수 s = Height × heightBlend + 마스크,  문턱 th = max(s) − blendDepth,  w = max(s − th, 0) / Σ
//  (Mishkinis 2013 «Advanced Terrain Texture Splatting» 의 depth = 0.2 식과 같은 구조 — 시안은 높이에 0.5 를 곱한다.)
//  가장 큰 점수의 레이어는 늘 blendDepth 만큼 남으므로 합이 0 이 되지 않는다.
float4 TerrainLayerWeights(float4 mask, float4 height, float4 heightBlend, float blendDepth)
{
	const float4 score = height * heightBlend + mask;
	const float  th    = max(max(score.x, score.y), max(score.z, score.w)) - blendDepth;
	const float4 w     = max(score - th, 0.0);
	return w / max(dot(w, float4(1.0, 1.0, 1.0, 1.0)), 1e-5);
}

// ============================================================
//  3단계 반복 숨김 (2026-10-28) — 시안의 «두 크기 UV 섞기» · «바위 레이어 세 방향 투영»
// ============================================================

// 두 크기 UV — 시안(다섯 행성 공통): UV_B = UV0 × 0.41 + (0.37, 0.71), 섞는 비율 = clamp(노이즈 × 3 − 1, 0, 1)
//  노이즈 = 블렌더 Noise Texture(물체 좌표, Scale 0.045, Detail 2) — 아래 TerrainFbm 이 블렌더와 같은 값을 낸다(4단계 때 바꿈).
static const float  kTerrainUvBScale    = 0.41;
static const float2 kTerrainUvBOffset   = float2(0.37, 0.71);
static const float  kTerrainAntiScale   = 0.045;   // 1/m — 약 22 m 마다 바뀐다
static const uint   kTerrainNoiseOctave = 3;       // Detail 2 = 옥타브 3
static const float  kTerrainBoxBlend    = 0.35;    // 시안 Image Texture projection_blend

// ── 블렌더 Noise Texture (2026-10-28 4단계 — 3단계의 «같은 성질의 대신»(PCG 해시 · 12 방향)을 바꿈) ──
//  Blender Cycles 원문을 한 줄씩 옮겼다: intern/cycles/util/hash.h hash_uint3(Jenkins lookup3 final),
//  kernel/svm/noise.h grad3 · fade · tri_mix · perlin_3d · noise_scale3, kernel/svm/fractal_noise.h noise_fbm.
//  시안 지형 물체는 변환이 없어(build_map.py) 블렌더 물체 좌표 = 메시 좌표 (x, −z, y) — 시안과 같은 자리에 같은 무늬가 나온다.
//  원문의 fmod(p, 100000) 은 뺐다: 여기 좌표는 행성 반지름(650 m) × 노이즈 배율(최대 1.4 × 2³) 이라 그 범위에 닿지 않는다.
uint TerrainRot(uint x, uint k)
{
	return (x << k) | (x >> (32u - k));
}

uint TerrainHashUint3(uint kx, uint ky, uint kz)
{
	uint a = 0xdeadbeefu + (3u << 2) + 13u;
	uint b = a;
	uint c = a;
	c += kz;
	b += ky;
	a += kx;
	c ^= b; c -= TerrainRot(b, 14u);   // final(a, b, c)
	a ^= c; a -= TerrainRot(c, 11u);
	b ^= a; b -= TerrainRot(a, 25u);
	c ^= b; c -= TerrainRot(b, 16u);
	a ^= c; a -= TerrainRot(c, 4u);
	b ^= a; b -= TerrainRot(a, 14u);
	c ^= b; c -= TerrainRot(b, 24u);
	return c;
}

// grad3 — 해시 아래 4비트로 기울기(12 방향 + 4 중복)를 고른 내적
float TerrainGrad3(uint hash, float3 f)
{
	const uint  h  = hash & 15u;
	const float u  = h < 8u ? f.x : f.y;
	const float vt = (h == 12u || h == 14u) ? f.x : f.z;
	const float v  = h < 4u ? f.y : vt;
	return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v);
}

// snoise_3d = noise_scale3(perlin_3d(p)) — 대략 −1 ~ 1
float TerrainSnoise(float3 p)
{
	const float3 cellF = floor(p);
	const int3   cell  = int3(cellF);
	const float3 f     = p - cellF;
	const float3 s     = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);   // fade
	float corners[8];
	[unroll] for (uint k = 0; k < 8; ++k)   // tri_mix 의 v0 ~ v7 순서(x 가 가장 빨리 바뀐다)
	{
		const int3 o = int3(k & 1u, (k >> 1) & 1u, (k >> 2) & 1u);
		corners[k] = TerrainGrad3(TerrainHashUint3(uint(cell.x + o.x), uint(cell.y + o.y), uint(cell.z + o.z)), f - float3(o));
	}
	const float x1 = 1.0 - s.x, y1 = 1.0 - s.y, z1 = 1.0 - s.z;
	const float r = z1 * (y1 * (corners[0] * x1 + corners[1] * s.x) + s.y * (corners[2] * x1 + corners[3] * s.x)) +
	                s.z * (y1 * (corners[4] * x1 + corners[5] * s.x) + s.y * (corners[6] * x1 + corners[7] * s.x));
	return 0.9820 * r;
}

// noise_fbm(p, Detail = octaves − 1, Roughness 0.5, Lacunarity 2, Normalize) → 0.5 가운데의 Fac.
//  시안의 Detail 은 모두 정수라 원문의 «소수 Detail 덧 옥타브» 는 없다.
//  footprint = 화면 픽셀 하나가 덮는 길이를 p 단위(배율을 곱한 값)로 잰 것. 옥타브 격자 한 칸이 화면 4 픽셀보다 작아지면
//  그 옥타브를 줄여 2 픽셀에서 0 으로 만든다 — 나이퀴스트를 넘는 주파수를 빼는 «clamping»(Ebert 외, Texturing & Modeling:
//  A Procedural Approach, 2장 안티에일리어싱). 블렌더는 픽셀마다 여러 번 쏴 평균을 내므로 같은 자리에서 같은 일이 일어난다.
//  0 을 넘기면 원문 그대로다(RT 히트처럼 미분이 없는 곳). 나눗셈(maxamp)은 원문대로 전체 옥타브 합이다 — 뺀 옥타브는 평균 0.
float TerrainFbm(float3 p, uint octaves, float footprint)
{
	float sum = 0.0, amp = 1.0, fscale = 1.0;
	for (uint o = 0; o < octaves; ++o)
	{
		const float keep = 1.0 - smoothstep(0.25, 0.5, footprint * fscale);
		if (keep <= 0.0)
			break;   // 다음 옥타브는 더 촘촘하다 — 모두 0
		sum += amp * keep * TerrainSnoise(fscale * p);
		amp *= 0.5;
		fscale *= 2.0;
	}
	const float maxamp = 2.0 - 2.0 * exp2(-float(octaves));   // Σ 0.5^o (o = 0 … octaves−1)
	return 0.5 * sum / maxamp + 0.5;
}

// Noise Texture 의 Color 출력 — (Fac, p + 오프셋(3) 의 Fac, p + 오프셋(4) 의 Fac).
//  오프셋 = noisetex.h random_float3_offset(3) · (4) — 원문 hash_float2_to_float(hash_uint2 · uint_to_float_incl)로 미리 계산한 값.
float3 TerrainFbm3(float3 p, uint octaves, float footprint)
{
	return float3(TerrainFbm(p, octaves, footprint),
	              TerrainFbm(p + float3(165.445282, 162.768219, 193.408417), octaves, footprint),
	              TerrainFbm(p + float3(155.580917, 177.154633, 138.14003), octaves, footprint));
}

// 블렌더 BOX 투영의 면 가중치 — Blender Cycles intern/cycles/kernel/svm/image.h svm_node_tex_image_box 그대로.
//  N = 물체 공간(블렌더 축) 법선. 한 축이 두드러지면 그 면만, 두 축 경계는 둘을, 꼭짓점 근처는 셋을 섞는다.
float3 TerrainBoxWeights(float3 N, float blend)
{
	N = abs(N);
	N /= max(N.x + N.y + N.z, 1e-6);
	const float limit = 0.5 * (1.0 + blend);
	float3 weight = float3(0.0, 0.0, 0.0);
	if (N.x > limit * (N.x + N.y) && N.x > limit * (N.x + N.z))
		weight.x = 1.0;
	else if (N.y > limit * (N.x + N.y) && N.y > limit * (N.y + N.z))
		weight.y = 1.0;
	else if (N.z > limit * (N.x + N.z) && N.z > limit * (N.y + N.z))
		weight.z = 1.0;
	else if (blend > 0.0)
	{
		if (N.z < (1.0 - limit) * (N.y + N.x))
		{
			weight.x = saturate((N.x / (N.x + N.y) - 0.5 * (1.0 - blend)) / blend);
			weight.y = 1.0 - weight.x;
		}
		else if (N.x < (1.0 - limit) * (N.y + N.z))
		{
			weight.y = saturate((N.y / (N.y + N.z) - 0.5 * (1.0 - blend)) / blend);
			weight.z = 1.0 - weight.y;
		}
		else if (N.y < (1.0 - limit) * (N.x + N.z))
		{
			weight.x = saturate((N.x / (N.x + N.z) - 0.5 * (1.0 - blend)) / blend);
			weight.z = 1.0 - weight.x;
		}
		else
		{
			weight = ((2.0 - limit) * N + (limit - 1.0)) / (2.0 * limit - 1.0);
		}
	}
	else
		weight.x = 1.0;
	return weight;
}

// ============================================================
//  4단계 색 흔들림 · 범프 (2026-10-28) — 시안 draft_terrain_material 의 macro · rockvar · aof · bump1 · bump2
//  행성마다 다른 값(색 흔들림 세기 · 범프 거리 · 세기)은 재질 상수, 다섯 행성 공통 값은 여기 상수.
// ============================================================
static const uint  kTerrainRockLayer      = 1;      // 마스크 G = 바위 레이어(다섯 행성 공통 — 시안 rockw = nws[1])
static const float kTerrainMacroScale     = 0.012;  // 1/m — 약 83 m 규모의 밝기 흔들림(Detail 3)
static const uint  kTerrainMacroOctaves   = 4;
static const float kTerrainRockVarScale   = 0.11;   // 1/m — 바위 면 안의 밝기 흔들림(Detail 4)
static const uint  kTerrainRockVarOctaves = 5;
static const float kTerrainRockVarAmount  = 0.45;   // 바위: 노이즈 × 0.45 + 0.78
static const float kTerrainRockVarBias    = 0.78;
static const float kTerrainAoAmount       = 0.55;   // AO 를 색에 곱한다: AO × 0.55 + 0.45
static const float kTerrainAoBias         = 0.45;
static const float kTerrainRockBumpGain   = 3.0;    // 범프 1 높이 = 섞은 높이 × (바위 가중치 × 3 + 1) — 바위는 4배 깊게
static const float kTerrainReliefScaleA   = 0.33;   // 범프 2 지면 요철 = noise(0.33, Detail 4) + 0.45 × noise(1.4, Detail 3)
static const uint  kTerrainReliefOctavesA = 5;
static const float kTerrainReliefScaleB   = 1.4;
static const uint  kTerrainReliefOctavesB = 4;
static const float kTerrainReliefWeightB  = 0.45;

// 색에 곱할 배율 = macro × aof × var (시안 «col × macro × aof × var»).
//  co = 블렌더 물체 좌표(m), footprint = 화면 픽셀 하나가 덮는 길이(m, 0 = 거르지 않음), ao = 섞은 ORM.r,
//  rockWeight = 바위 레이어 가중치, macroAmount · macroBias = 행성 값(빙하 0.22 · 0.89, 나머지 0.32 · 0.84).
float TerrainColorScale(float3 co, float footprint, float ao, float rockWeight, float macroAmount, float macroBias)
{
	const float macro = TerrainFbm(co * kTerrainMacroScale, kTerrainMacroOctaves, footprint * kTerrainMacroScale) *
		macroAmount + macroBias;
	float variation = 1.0;
	[branch] if (rockWeight > 0.0)
	{
		const float rockVar = TerrainFbm(co * kTerrainRockVarScale, kTerrainRockVarOctaves, footprint * kTerrainRockVarScale) *
			kTerrainRockVarAmount + kTerrainRockVarBias;
		variation = rockVar * rockWeight + (1.0 - rockWeight);
	}
	const float aoFactor = ao * kTerrainAoAmount + kTerrainAoBias;
	return macro * aoFactor * variation;
}

// 범프 2 의 높이 — 지면 요철(0.5 ~ 3 m 규모, 지형 메시 2 m 간격보다 작은 울퉁불퉁함)
float TerrainRelief(float3 co, float footprint)
{
	return TerrainFbm(co * kTerrainReliefScaleA, kTerrainReliefOctavesA, footprint * kTerrainReliefScaleA) +
		kTerrainReliefWeightB * TerrainFbm(co * kTerrainReliefScaleB, kTerrainReliefOctavesB, footprint * kTerrainReliefScaleB);
}

// 블렌더 Bump 노드 — Cycles kernel/svm/displace.h svm_node_set_bump 그대로(Mikkelsen 2010 «Bump Mapping Unparametrized
//  Surfaces on the GPU» 의 표면 기울기 식). dPdx · dPdy = 화면 x · y 로 한 픽셀 옮길 때의 위치 변화,
//  dHdx · dHdy = 그때의 높이 변화. 원문의 filter_width 는 높이 차이와 det 양쪽에 같이 걸려 지워진다(원문 주석의 식).
//  화면 y 가 아래로 늘어 손잡이가 바뀌어도 det 부호가 같이 바뀌어 결과는 같다.
float3 TerrainBump(float3 N, float3 dPdx, float3 dPdy, float dHdx, float dHdy, float distance, float strength)
{
	const float3 Rx      = cross(dPdy, N);
	const float3 Ry      = cross(N, dPdx);
	const float  det     = dot(dPdx, Rx);
	const float3 surfgrad = dHdx * Rx + dHdy * Ry;
	const float3 n       = abs(det) * N - distance * (det < 0.0 ? -1.0 : 1.0) * surfgrad;   // signf: 0 은 +1
	const float  len2    = dot(n, n);
	if (!(len2 > 1e-30))
		return N;   // 원문 safe_normalize 가 0 이면 입력 법선
	return normalize(strength * (n * rsqrt(len2)) + (1.0 - strength) * N);
}

#endif
