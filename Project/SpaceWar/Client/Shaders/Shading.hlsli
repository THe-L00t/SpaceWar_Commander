// ============================================================
//  Shading.hlsli — 셰이딩 공용 함수 · 임시 광원 상수
//
//  ★ 2026-10-27 파이프라인 1단계에서 Forward.hlsl 에서 옮겨 왔다.
//    같은 식을 두 곳이 쓴다:
//      · Forward.hlsl  PSMain   — 배경 패스(먼 행성)를 forward 로 그릴 때
//      · Lighting.hlsl CSMain   — 메인 패스의 G-buffer 를 컴퓨트로 라이팅할 때 ([5] 라이팅 합성)
//    한쪽만 고치면 배경과 메인의 밝기가 어긋나므로 한 파일에 둔다.
// ============================================================
#ifndef SHADING_HLSLI
#define SHADING_HLSLI

// 기존 (albedo * (NdotL*0.85 + 0.15)) 를 그대로 재현하는 값.
// 광원 목록 · 하늘빛이 들어오면 이 상수 대신 그 값을 쓴다.
static const float3 kSunColor = float3(0.85, 0.85, 0.85);
static const float  kAmbient  = 0.15;

// GGX 는 rough 0 에서 분모가 터진다. 거울도 이 바닥값으로 둔다.
static const float kMinRoughness = 0.02;

static const float kPi = 3.14159265;

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

// 태양(방향광) 하나의 직접광과 앰비언트를 나눠 돌려준다.
//  forward 와 라이팅 컴퓨트가 같은 숫자를 내야 하므로 식을 여기 한 곳에만 둔다.
struct DirectLighting
{
	float3 diffuse;    // 태양 확산
	float3 specular;   // 태양 정반사 (GGX)
	float3 ambient;    // 앰비언트 확산 — 나중에 GI 가 들어갈 자리
	float3 F_env;      // 환경 프레넬 (N·V 기준) — 반사 레이 결과에 곱할 값
};

DirectLighting EvaluateSunAndAmbient(float3 albedo, float3 N, float3 V, float rough, float metallic, float3 sunDir)
{
	DirectLighting o;

	float  NdotV = saturate(dot(N, V));
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

	// ① 태양 (방향광) — 분석적. 델타 광원이라 적분이 닫힌 형태다.
	float3 L = normalize(-sunDir);
	float3 H = normalize(L + V);
	float  NdotL = saturate(dot(N, L));
	float  NdotH = saturate(dot(N, H));
	float  VdotH = saturate(dot(V, H));

	float3 F_dir = FresnelSchlick(F0, VdotH);          // ★ 반각 기준
	o.specular = SpecularGGX(NdotL, NdotV, NdotH, rough, F_dir) * NdotL * kSunColor;
	o.diffuse  = (1.0 - F_dir) * (1.0 - metallic) * albedo * NdotL * kSunColor;

	// ② 환경 프레넬 — 완전 거울이므로 반각 H = N, 즉 N·V 기준
	o.F_env = FresnelSchlick(F0, NdotV);

	// 앰비언트는 반구 전체에서 오는 빛이라 각도 의존 F 로 나누면 안 된다.
	// (1-F) 로 나누면 grazing 에서 확산이 0이 되어 먼 지형이 색을 잃는다.
	float3 F_avg = F0 + (1.0 - F0) * 0.05;
	o.ambient = (1.0 - F_avg) * (1.0 - metallic) * albedo * kAmbient;

	return o;
}

#endif
