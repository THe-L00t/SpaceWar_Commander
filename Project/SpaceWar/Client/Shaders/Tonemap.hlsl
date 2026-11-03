// ============================================================
//  Tonemap.hlsl — [8] HDR 컬러 → 백버퍼 (2026-10-27 파이프라인 1단계, 10단계에서 톤맵 추가)
//
//  화면 전체 삼각형 하나로 HDR 컬러를 백버퍼(R8G8B8A8_UNORM)에 옮긴다. 진입점 둘:
//    PSMain     예전 그대로 — 톤 곡선 · 노출 · 감마 없이 0~1 로 자르기만 한다.
//               ★ 10단계부터는 디버그 뷰(0 이 아닌 것)와 바인드리스가 없는 장치에서만 쓴다.
//                 디버그 뷰는 G-buffer 값을 숫자 그대로 봐야 하므로(캡처 비교 기준) 노출 · 톤 곡선을 걸지 않는다.
//    PSTonemap  ★ 10단계 — 최종 화면. 바인드리스 번호는 루트 상수로 받는다.
//               색 = lerp(HDR, 블룸, 0.04) × 노출(PostProcess.hlsl 자동 노출) → ACES 근사 → sRGB 부호화.
//               · ACES 근사: Narkowicz 2015 «ACES Filmic Tone Mapping Curve» — 설계 문서 6.7 의 식 그대로.
//               · sRGB: 백버퍼 RTV 가 UNORM 이라 셰이더에서 sRGB 전달 함수(IEC 61966-2-1)를 직접 건다.
//                 예전엔 선형 값을 그대로 써서 어두워 보였다(설계 문서 11.2 1단계 설명). UI 는 그 뒤에 그대로 덮는다.
// ============================================================

Texture2D<float4> gHdr : register(t0);

struct VOut
{
	float4 pos : SV_Position;
};

// 정점 버퍼 없이 SV_VertexID 0·1·2 로 화면을 덮는 큰 삼각형 하나를 만든다.
VOut VSMain(uint id : SV_VertexID)
{
	const float2 uv = float2((id << 1) & 2, id & 2);   // (0,0) (2,0) (0,2)
	VOut o;
	o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	return o;
}

float4 PSMain(VOut i) : SV_Target
{
	// 같은 해상도라 픽셀 좌표로 바로 읽는다(샘플러 없음).
	const float3 c = gHdr.Load(int3(int2(i.pos.xy), 0)).rgb;
	return float4(saturate(c), 1.0);
}

// ── 10단계 — 최종 톤맵 (바인드리스) ──
//  ResourceDescriptorHeap 은 SM 6.6 부터라, 6.0 으로 컴파일하는 위 두 진입점 때는 이 부분을 빼야 컴파일된다.
#if __SHADER_TARGET_MAJOR > 6 || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6)
cbuffer TonemapCB : register(b0)   // 루트 상수 8개
{
	uint  gHdrIndex;       // HDR 컬러 SRV
	uint  gBloomIndex;     // 블룸 올리기 0단(반 해상도) SRV
	uint  gExposureIndex;  // 노출 버퍼 SRV (float4: 노출, 적응 log2 밝기, 목표, 유효)
	float gBloomStrength;  // 0.04 (Jimenez · LearnOpenGL 해설의 값)
	uint  gBloomEnabled;
	float gBloomScale;     // 1 / 블룸 단 수 (올리기 0단은 모든 단의 합이라)
	uint2 _pad0;
};
SamplerState gClampSampler : register(s0);

// ACES 근사 (Narkowicz 2015) — 설계 문서 6.7
float3 AcesFilm(float3 x)
{
	return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// 선형 → sRGB 전달 함수 (IEC 61966-2-1)
float3 LinearToSrgb(float3 c)
{
	const float3 lo = c * 12.92;
	const float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
	return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g, c.b <= 0.0031308 ? lo.b : hi.b);
}

float4 PSTonemap(VOut i) : SV_Target
{
	Texture2D<float4>        hdr      = ResourceDescriptorHeap[gHdrIndex];
	StructuredBuffer<float4> exposure = ResourceDescriptorHeap[gExposureIndex];
	float3 c = hdr.Load(int3(int2(i.pos.xy), 0)).rgb;
	if (gBloomEnabled != 0)
	{
		Texture2D<float4> bloom = ResourceDescriptorHeap[gBloomIndex];
		uint w, h;
		hdr.GetDimensions(w, h);
		const float2 uv = i.pos.xy / float2(w, h);
		// 올리기 0단 = 내리기 6단을 다 더한 값(장면의 약 6배) → 단 수로 나눠 «장면과 같은 밝기의 흐린 그림» 으로 섞는다.
		//  (처음엔 나누지 않아서 블룸이 화면 전체를 평균 +13/255 밝히는 안개가 됐다 — 섞기가 에너지를 지키려면 평균이어야 한다)
		c = lerp(c, bloom.SampleLevel(gClampSampler, uv, 0).rgb * gBloomScale, gBloomStrength);
	}
	c *= exposure[0].x;
	return float4(LinearToSrgb(AcesFilm(c)), 1.0);
}
#endif
