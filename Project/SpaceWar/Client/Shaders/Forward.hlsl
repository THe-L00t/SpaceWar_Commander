// ============================================================
//  Forward.hlsl — 하이브리드 렌더링 골격
//    래스터 : 1차 가시성 + 직접광 (전체 화면)
//    RT     : 스페큘러 반사만 (인라인 RayQuery, 반사 레이 1개)
//
//  두 개의 프레넬을 구분해서 쓴다:
//    · 물리 Schlick F  = 확산/반사의 에너지 배분 비율 (그려지는 값)
//    · FGPS 의 F(p)    = 샘플을 어디에 몰아줄지 정하는 예산 신호 (확률만 조정)
//
//  레이 발사는 임계값이 아니라 확률(러시안 룰렛)로 한다 → 화면에 경계선이 없다.
//
//  ※ 머티리얼 시스템이 아직 없다. roughness/metallic 은 아래 자리표시자 상수다.
// ============================================================

#ifndef RT_SUPPORTED
#define RT_SUPPORTED 0
#endif

cbuffer FrameCB : register(b0)
{
	float4x4 gViewProj;
	float3   gEyePos;        float _pad0;
	float3   gSunDir;        float _pad1;
	uint     gRtEnabled;
	float    gRouletteKnee;    // 이 가중치 이상은 확정 발사
	float    gFresnelBoost;    // F(p) 가 확률을 밀어올리는 정도
	uint     gDebugMode;
	uint     gFrameIndex;
	float3   _pad2;
};

cbuffer ObjectCB : register(b1)
{
	float4x4 gWorld;
};

#if RT_SUPPORTED
RaytracingAccelerationStructure gScene : register(t0);
#endif

// ── 머티리얼 자리표시자 (머티리얼 시스템 도입 시 제거) ──────────
//   metallic 0 = 유전체. 정면은 확산, grazing 에서 거울 — 젖은 바닥 효과.
//   1.0 으로 바꾸면 금속(확산 없음)이 되고 F(p) 예산 신호도 활성화된다.
static const float kRoughness = 0.10f;
static const float kMetallic  = 0.00f;

// 기존 (albedo * (NdotL*0.85 + 0.15)) 를 그대로 재현하는 값.
// 한 번에 하나만 바꾼다 — 새로 생기는 건 태양 하이라이트뿐이어야 검증이 쉽다.
static const float3 kSunColor = float3(0.85, 0.85, 0.85);
static const float  kAmbient  = 0.15;

static const float kPi = 3.14159265;

struct VIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float3 col : COLOR;
};

struct VOut
{
	float4 pos   : SV_POSITION;
	float3 world : WORLDPOS;
	float3 nrm   : NORMAL;
	float3 col   : COLOR;
};

VOut VSMain(VIn i)
{
	VOut o;
	float4 wp = mul(float4(i.pos, 1.0), gWorld);
	o.world = wp.xyz;
	o.pos   = mul(wp, gViewProj);
	o.nrm   = mul(float4(i.nrm, 0.0), gWorld).xyz;
	o.col   = i.col;
	return o;
}

// Interleaved Gradient Noise — 1spp 디더링에서 백색잡음보다 훨씬 균일하다.
float InterleavedGradientNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// ★ 여기에 태양 원반을 넣으면 안 된다.
//   반사 레이가 태양을 맞히면 그게 곧 태양 정반사인데,
//   아래 분석적 GGX 항이 이미 같은 걸 계산한다 → 이중 계산.
//   상용 렌더러가 환경맵에서 태양을 분리해두는 이유가 이것이다.
float3 SkyColor(float3 dir)
{
	float t = saturate(dir.y * 0.5 + 0.5);
	return lerp(float3(0.30, 0.34, 0.42), float3(0.06, 0.14, 0.38), t);
}

float3 FresnelSchlick(float3 F0, float cosTheta)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// GGX 정반사 BRDF — 직접광(델타 광원)용.
// 방향광은 입체각이 0이라 적분이 닫힌 형태로 풀린다. 레이로는 명중 확률이 0이다.
// 패스 트레이서도 NEE 로 이 계산을 한다.
float3 SpecularGGX(float NdotL, float NdotV, float NdotH, float rough, float3 F)
{
	float a  = rough * rough;
	float a2 = a * a;

	// GGX 법선 분포
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	float D = a2 / (kPi * d * d);

	// Smith 가시성 (Schlick-GGX, 직접광용 k)
	float k  = a * 0.5;
	float gv = NdotV / (NdotV * (1.0 - k) + k);
	float gl = NdotL / (NdotL * (1.0 - k) + k);
	float G  = gv * gl;

	return F * D * G / max(4.0 * NdotV * NdotL, 1e-4);
}

#if RT_SUPPORTED
// 반사 레이 1개. 재귀 없음, 히트 셰이딩 없음 — 구조 검증용.
float3 TraceReflection(float3 P, float3 N, float3 V)
{
	RayDesc ray;
	ray.Origin    = P + N * 0.02;      // self-intersection 회피
	ray.Direction = reflect(-V, N);
	ray.TMin      = 0.001;
	ray.TMax      = 500.0;

	RayQuery<RAY_FLAG_NONE> q;
	q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
	while (q.Proceed()) { }

	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		// 머티리얼이 없으므로 거리 감쇠만 돌려준다.
		float d = saturate(q.CommittedRayT() / 60.0);
		return lerp(float3(0.32, 0.32, 0.36), float3(0.02, 0.02, 0.03), d);
	}
	return SkyColor(ray.Direction);
}
#endif

float4 PSMain(VOut i) : SV_TARGET
{
	float3 N = normalize(i.nrm);
	float3 V = normalize(gEyePos - i.world);
	float  NdotV = saturate(dot(N, V));
	float  grazing = pow(1.0 - NdotV, 5.0);

	float3 F0 = lerp(float3(0.04, 0.04, 0.04), i.col, kMetallic);

	// ① 태양 (방향광) — 분석적. 델타 광원이라 적분이 닫힌 형태다.
	float3 L = normalize(-gSunDir);
	float3 H = normalize(L + V);
	float  NdotL = saturate(dot(N, L));
	float  NdotH = saturate(dot(N, H));
	float  VdotH = saturate(dot(V, H));

	float3 F_dir   = FresnelSchlick(F0, VdotH);          // ★ 반각 기준
	float3 specSun = SpecularGGX(NdotL, NdotV, NdotH, kRoughness, F_dir) * NdotL * kSunColor;
	float3 diffSun = (1.0 - F_dir) * (1.0 - kMetallic) * i.col * NdotL * kSunColor;

	// ② 환경 프레넬 — 완전 거울이므로 반각 H = N, 즉 N·V 기준
	float3 F  = FresnelSchlick(F0, NdotV);
	float  w  = max(F.r, max(F.g, F.b));

	// 앰비언트는 반구 전체에서 오는 빛이라 각도 의존 F 로 나누면 안 된다.
	// (1-F) 로 나누면 grazing 에서 확산이 0이 되어 먼 지형이 색을 잃는다.
	float3 F_avg   = F0 + (1.0 - F0) * 0.05;
	float3 diffAmb = (1.0 - F_avg) * (1.0 - kMetallic) * i.col * kAmbient;

	// ③ FGPS F(p) — 예산 신호. 그려지는 값은 건드리지 않고 확률만 밀어올린다.
	float fp = grazing * saturate(1.0 - kRoughness) * saturate(kMetallic);

	// ④ 러시안 룰렛 — 임계값 대신 확률로 발사하므로 화면에 경계선이 생기지 않는다.
	//    w >= knee 구간은 p=1 (확정 발사, 노이즈 0).
	//    그 아래 꼬리만 확률 발사하고 1/p 로 보정 → 기댓값은 정확(비편향).
	//    꼬리에서의 기여는 knee 로 상한이 걸려 노이즈 진폭이 제한된다.
	float p = saturate((w / max(gRouletteKnee, 1e-4)) * (1.0 + gFresnelBoost * fp));

	float3 reflection = float3(0.0, 0.0, 0.0);
	float  fired = 0.0;

#if RT_SUPPORTED
	if (gRtEnabled != 0 && p > 0.0)
	{
		float xi = InterleavedGradientNoise(i.pos.xy + 5.588238 * float(gFrameIndex % 64u));
		if (xi < p)
		{
			reflection = TraceReflection(i.world, N, V) / p;
			fired = 1.0;
		}
	}
#endif

	// ⑤ 합성 — 광원별로 갈라서 더한다.
	//    태양과 환경은 같은 정반사 로브로 들어오는 다른 빛이므로 서로를 약화시키지 않는다.
	//    ★ 확산 몫(kd)이 정반사에 곱해지지 않는다. 곱하면 프레넬 분배를 두 번 적용하는 셈이다.
	float3 specEnv = F * reflection;
	float3 color = diffSun + specSun + diffAmb + specEnv;
	//             └─ 확산 ─┘  └──── 정반사 (둘 다 F 를 품음) ────┘

	switch (gDebugMode)
	{
	case 1: color = i.col;               break;   // albedo
	case 2: color = N * 0.5 + 0.5;       break;   // world normal
	case 3: color = fp.xxx;              break;   // F(p) 예산 신호
	case 4: color = F;                   break;   // 환경 프레넬 F_env (반사 배분 비율)
	case 5: color = reflection;          break;   // 반사 원본 (F 곱하기 전)
	case 6: color = p.xxx;               break;   // 발사 확률
	case 7: color = fired > 0.5 ? float3(1.0, 0.25, 0.0)   // 실제 발사 패턴
	                            : float3(0.0, 0.08, 0.12);
		break;
	case 8: color = specSun;             break;   // 태양 하이라이트만
	case 9: color = specEnv;             break;   // 환경 반사 몫만 (F 곱한 뒤)
	}

	return float4(color, 1.0);
}
