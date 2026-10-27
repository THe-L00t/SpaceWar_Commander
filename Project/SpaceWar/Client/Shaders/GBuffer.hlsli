// ============================================================
//  GBuffer.hlsli — G-buffer 배치 (2026-10-27 파이프라인 1단계)
//
//  [1] G-buffer 패스(Forward.hlsl PSGBuffer)가 쓰고,
//  [5] 라이팅 합성(Lighting.hlsl)이 읽는다. 앞으로 붙을 단계(AO · RT 그림자 · 반사)도 여기서 읽는다.
//
//  슬롯  형식                    내용
//  GB0   R8G8B8A8_UNORM_SRGB    rgb = 알베도(쓸 때 sRGB 로 저장, 읽을 때 선형으로 풀림)  a = 안 씀(1)
//  GB1   R16G16_SNORM           월드 법선 — 팔면체 인코딩(3성분을 2성분으로)
//  GB2   R8G8B8A8_UNORM         r = 거칠기  g = 금속성  ba = 안 씀(0)
//  GB3   R11G11B10_FLOAT        자체발광 (1 을 넘을 수 있다)
//  깊이  D32_FLOAT (자원은 R32_TYPELESS)  — 컴퓨트는 R32_FLOAT SRV 로 읽어 월드 위치를 되살린다
//
//  ★ GRenderer.cpp 의 Impl::kGBufferFormats 와 순서·형식이 같아야 한다.
//  ★ 깊이 1.0 = 이 픽셀에 메인 패스 물체가 없다. 뒤 단계는 이 픽셀을 건너뛴다
//    (그래서 G-buffer 는 매 프레임 지우지 않는다 — 깊이가 «유효한 픽셀» 표시를 겸한다).
// ============================================================
#ifndef GBUFFER_HLSLI
#define GBUFFER_HLSLI

struct GBufferOut
{
	float4 albedo   : SV_Target0;
	float2 normal   : SV_Target1;
	float4 material : SV_Target2;
	float3 emissive : SV_Target3;
};

// 팔면체 인코딩 — 단위 벡터를 팔면체에 펴서 [-1,1]² 에 담는다.
//  (Cigolle 외, «A Survey of Efficient Representations for Independent Unit Vectors», JCGT 2014)
//  ★ 벡터 삼항 연산자를 쓰지 않는다 — HLSL 2021 은 벡터 조건의 ?: 를 막는다. 성분마다 스칼라로 고른다.
float2 EncodeNormalOct(float3 n)
{
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	if (n.z < 0.0)
	{
		const float2 s = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * s;
	}
	return n.xy;
}

float3 DecodeNormalOct(float2 e)
{
	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	const float t = saturate(-n.z);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

GBufferOut PackGBuffer(float3 albedo, float3 N, float rough, float metallic, float3 emissive)
{
	GBufferOut o;
	o.albedo   = float4(albedo, 1.0);
	o.normal   = EncodeNormalOct(N);
	o.material = float4(rough, metallic, 0.0, 0.0);
	o.emissive = emissive;
	return o;
}

#endif
