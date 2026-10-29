// ============================================================
//  Shading.hlsli — 셰이딩 공용 함수 · 임시 광원 상수
//
//  ★ 2026-10-27 파이프라인 1단계에서 Forward.hlsl 에서 옮겨 왔다.
//    같은 식을 두 곳이 쓴다:
//      · Forward.hlsl  PSMain   — 배경 패스(먼 행성)를 forward 로 그릴 때
//      · Lighting.hlsl CSMain   — 메인 패스의 G-buffer 를 컴퓨트로 라이팅할 때 ([5] 라이팅 합성)
//    한쪽만 고치면 배경과 메인의 밝기가 어긋나므로 한 파일에 둔다.
//
//  ★ 2026-10-27 렌더 6단계 — 조명 단위 정리 (설계 문서 6.6 «밝기 맞추기»)
//    · 태양은 «조도» 로 받는다(sunIlluminance, RGB — 대기를 지나며 색이 붙은 값). 렌더러가 프레임마다 계산해 넘긴다.
//    · 확산은 람베르트 알베도/π × 조도. 예전 식(알베도 × NdotL × 0.85)은 1/π 를 빼고 태양을 0.85 로 둔 것이었다.
//      대기 위 태양 조도를 π 로 두면 확산 밝기는 거의 같고, 정반사 하이라이트만 π 배 — 예전 비율이 틀렸던 것을 바로잡는다.
//    · 주변광은 «조도 / π» (= 알베도만 곱하면 확산 휘도)로 받는다. 하늘 조도 텍스처([6]) · GI 프로브([4c])가 이 단위다.
// ============================================================
#ifndef SHADING_HLSLI
#define SHADING_HLSLI

// 하늘 · GI 가 없을 때(바인드리스 미지원 장치 · 배경 패스)의 주변광 — 조도 / π. 예전 앰비언트 0.15 와 같은 값이다.
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

// 태양(방향광) 하나의 직접광과 주변광을 나눠 돌려준다.
//  forward 와 라이팅 컴퓨트 · RT 히트가 같은 숫자를 내야 하므로 식을 여기 한 곳에만 둔다.
struct DirectLighting
{
	float3 diffuse;    // 태양 확산
	float3 specular;   // 태양 정반사 (GGX)
	float3 ambient;    // 주변광 확산 — 하늘 조도([6]) 또는 GI 프로브([4c])
	float3 F_env;      // 환경 프레넬 (N·V 기준) — 반사 레이 결과에 곱할 값
};

//  sunIlluminance  = 땅에 닿는 태양 조도(RGB). 해가 지평선 아래면 0.
//  ambientEOverPi  = 이 법선이 받는 주변 조도 / π (RGB).
DirectLighting EvaluateSunAndAmbient(float3 albedo, float3 N, float3 V, float rough, float metallic, float3 sunDir,
	float3 sunIlluminance, float3 ambientEOverPi)
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
	o.specular = SpecularGGX(NdotL, NdotV, NdotH, rough, F_dir) * NdotL * sunIlluminance;
	o.diffuse  = (1.0 - F_dir) * (1.0 - metallic) * albedo / kPi * NdotL * sunIlluminance;   // 6단계 — 람베르트 1/π

	// ② 환경 프레넬 — 완전 거울이므로 반각 H = N, 즉 N·V 기준
	o.F_env = FresnelSchlick(F0, NdotV);

	// 앰비언트는 반구 전체에서 오는 빛이라 각도 의존 F 로 나누면 안 된다.
	// (1-F) 로 나누면 grazing 에서 확산이 0이 되어 먼 지형이 색을 잃는다.
	float3 F_avg = F0 + (1.0 - F0) * 0.05;
	o.ambient = (1.0 - F_avg) * (1.0 - metallic) * albedo * ambientEOverPi;

	return o;
}

#endif
