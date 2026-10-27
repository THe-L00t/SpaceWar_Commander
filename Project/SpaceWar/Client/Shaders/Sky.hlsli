// ============================================================
//  Sky.hlsli — [6] 하늘 공용 (2026-10-27 렌더 6단계)
//
//  하늘은 «방향 → 휘도» 텍스처 하나다(등장방형 = 위도 · 경도 배치). 쓰는 곳:
//    · Sky.hlsl CSDrawSky       — 물체가 없는 픽셀의 배경
//    · RtReflection.hlsl Miss   — 반사 레이가 아무것도 안 맞았을 때
//    · Lighting.hlsl            — 하늘 조도 텍스처(이 텍스처를 코사인으로 섞은 것)로 그늘의 주변광
//  지금 내용은 Sky.hlsl 이 대기 산란으로 계산해 채운다(임시). 행성별 HDR 하늘 에셋이 오면 같은 텍스처를 파일로 채운다.
//
//  ★ 텍스처는 «월드 방향» 으로 찾는다. 하늘 내용은 카메라 자리의 위(행성 위치마다 다름)로 계산하지만,
//    쓰는 쪽은 월드 방향만 넘기면 된다(위가 바뀌면 렌더러가 텍스처를 다시 만든다).
//  ★ 샘플러는 U 반복 · V 고정 이어야 한다(경도는 한 바퀴 돌아 이어지고, 위도는 극에서 끝난다) — 루트 시그니처의 s1.
//  ★ 단위: 태양이 대기 위에서 주는 조도를 π 로 둔다(흰 표면이 해를 정면으로 받으면 휘도 1).
//    조도 텍스처는 E/π(코사인 가중 평균 휘도)로 저장한다 — 알베도만 곱하면 확산 휘도다(설계 문서 6.4 와 같은 규약).
// ============================================================
#ifndef SKY_HLSLI
#define SKY_HLSLI

static const float kSkyPi = 3.14159265;

// 월드 방향 → 등장방형 UV.  u = 경도(월드 x·z 평면 각도), v = 위도(월드 +y 에서 잰 각도: 0 = +y, 1 = −y)
float2 SkyDirectionToUv(float3 d)
{
	const float u = atan2(d.x, d.z) * (0.5 / kSkyPi) + 0.5;
	const float v = acos(clamp(d.y, -1.0, 1.0)) / kSkyPi;
	return float2(u, v);
}

// 등장방형 UV → 월드 방향 (위의 반대)
float3 SkyUvToDirection(float2 uv)
{
	const float phi   = (uv.x - 0.5) * 2.0 * kSkyPi;
	const float theta = uv.y * kSkyPi;
	const float s     = sin(theta);
	return float3(s * sin(phi), cos(theta), s * cos(phi));
}

// 하늘 휘도 (해 원반 제외 — 해의 직접광은 라이팅이 공식으로 계산하므로 두 번 세지 않는다)
float3 SampleSkyRadiance(Texture2D<float4> sky, SamplerState skySampler, float3 dir)
{
	return sky.SampleLevel(skySampler, SkyDirectionToUv(dir), 0).rgb;
}

// 법선 N 이 하늘에서 받는 확산 조도 / π (= 알베도를 곱하면 확산 휘도)
float3 SampleSkyIrradiance(Texture2D<float4> irradiance, SamplerState skySampler, float3 N)
{
	return irradiance.SampleLevel(skySampler, SkyDirectionToUv(N), 0).rgb;
}

#endif
