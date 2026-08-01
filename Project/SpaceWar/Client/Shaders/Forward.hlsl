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

float3 SkyColor(float3 dir)
{
	float t = saturate(dir.y * 0.5 + 0.5);
	return lerp(float3(0.30, 0.34, 0.42), float3(0.06, 0.14, 0.38), t);
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

	// ① 래스터 직접광 — 방향광 1개 + 앰비언트
	float  NdotL  = saturate(dot(N, -gSunDir));
	float3 direct = i.col * (NdotL * 0.85 + 0.15);

	// ② 물리 프레넬 (Schlick) — 확산과 반사의 배분 비율
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), i.col, kMetallic);
	float3 F  = F0 + (float3(1.0, 1.0, 1.0) - F0) * grazing;
	float  w  = max(F.r, max(F.g, F.b));

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

	// ⑤ 래스터와 합성 — 더하지 않고 에너지 보존 블렌딩.
	//    반사가 강해지는 만큼 확산이 줄어든다 (금속은 확산이 아예 없다).
	float3 kd = (float3(1.0, 1.0, 1.0) - F) * (1.0 - kMetallic);
	float3 color = kd * direct + F * reflection;

	switch (gDebugMode)
	{
	case 1: color = i.col;               break;   // albedo
	case 2: color = N * 0.5 + 0.5;       break;   // world normal
	case 3: color = fp.xxx;              break;   // F(p) 예산 신호
	case 4: color = F;                   break;   // 물리 프레넬 (배분 비율)
	case 5: color = reflection;          break;   // 반사 성분만
	case 6: color = p.xxx;               break;   // 발사 확률
	case 7: color = fired > 0.5 ? float3(1.0, 0.25, 0.0)   // 실제 발사 패턴
	                            : float3(0.0, 0.08, 0.12);
		break;
	}

	return float4(color, 1.0);
}
